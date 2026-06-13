#include "cxl_fusee_kvpair_pool.h"

#include "common.h"  // flush_line, store_fence, full_fence, flush_region

#include <chrono>
#include <cstring>
#include <cstdio>

namespace fusee {

namespace {

inline std::size_t align_up(std::size_t n, std::size_t a) {
  return (n + (a - 1)) & ~(a - 1);
}

}  // namespace

CxlFuseeKvPairPool::~CxlFuseeKvPairPool() {
  stop_reclaim_thread();
}

std::size_t CxlFuseeKvPairPool::bytes_for(uint64_t total_records,
                                          int num_hosts,
                                          uint32_t record_size) {
  if (num_hosts <= 0 || num_hosts > kFuseePoolMaxHosts) return 0;
  if (record_size == 0) return 0;
  std::size_t header     = sizeof(Header);
  std::size_t cursors    = sizeof(HostCursor) * kFuseePoolMaxHosts;
  std::size_t bitmap     = align_up((total_records + 7) / 8, 64);
  std::size_t records    = total_records * record_size;
  return align_up(header + cursors + bitmap, 64) + records;
}

int CxlFuseeKvPairPool::attach(void *base, std::size_t bytes,
                               uint64_t total_records,
                               int host_id, int num_hosts,
                               bool init_region,
                               uint32_t record_size) {
  if (!base || total_records == 0 || record_size == 0) return -1;
  if (num_hosts <= 0 || num_hosts > kFuseePoolMaxHosts) return -1;
  if (host_id < 0 || host_id >= num_hosts) return -1;
  std::size_t need = bytes_for(total_records, num_hosts, record_size);
  if (bytes < need) return -1;

  base_ = reinterpret_cast<uint8_t *>(base);
  total_bytes_ = bytes;
  total_records_ = total_records;
  records_per_host_ = total_records / static_cast<uint64_t>(num_hosts);
  host_id_ = host_id;
  num_hosts_ = num_hosts;
  record_size_ = record_size;

  std::size_t header_bytes  = sizeof(Header);
  std::size_t cursors_bytes = sizeof(HostCursor) * kFuseePoolMaxHosts;
  bitmap_bytes_ = align_up((total_records + 7) / 8, 64);
  bitmap_base_off_ = align_up(header_bytes + cursors_bytes, 64);
  records_base_off_ = align_up(bitmap_base_off_ + bitmap_bytes_, 64);

  auto *hdr = reinterpret_cast<Header *>(base_);
  cursors_ = reinterpret_cast<HostCursor *>(base_ + header_bytes);
  bitmap_  = base_ + bitmap_base_off_;
  records_ = base_ + records_base_off_;

  if (init_region) {
    std::memset(base_, 0, header_bytes + cursors_bytes + bitmap_bytes_);
    hdr->magic            = kFuseePoolMagic;
    hdr->total_records    = total_records_;
    hdr->num_hosts        = static_cast<uint32_t>(num_hosts_);
    hdr->record_size      = record_size_;
    hdr->records_base_off = records_base_off_;
    hdr->bitmap_base_off  = bitmap_base_off_;
    hdr->bitmap_bytes     = bitmap_bytes_;
    // Records region itself does not need to be zeroed up front — readers
    // will only see a record after a writer has published a slot pointing
    // to it (slot publication happens-after the write+flush per the
    // protocol). Skipping the memset saves multi-second init time on large
    // pools.
    flush_region(base_, header_bytes + cursors_bytes + bitmap_bytes_);
    store_fence();
  } else {
    flush_line(&hdr->magic);
    full_fence();
    if (hdr->magic != kFuseePoolMagic) return -1;
    if (hdr->total_records != total_records_) return -1;
    if (hdr->record_size != record_size_) return -1;
  }

  return 0;
}

uint64_t CxlFuseeKvPairPool::segment_to_off(int host_id, uint64_t idx) const {
  uint64_t global_idx = static_cast<uint64_t>(host_id) * records_per_host_ + idx;
  return records_base_off_ + global_idx * record_size_;
}

void CxlFuseeKvPairPool::off_to_segment(uint64_t off, int *out_host,
                                        uint64_t *out_idx) const {
  uint64_t rec_idx = (off - records_base_off_) / record_size_;
  int      h       = static_cast<int>(rec_idx / records_per_host_);
  uint64_t local   = rec_idx % records_per_host_;
  if (out_host) *out_host = h;
  if (out_idx)  *out_idx  = local;
}

uint64_t CxlFuseeKvPairPool::alloc_tiny() {
  if (!base_) return 0;

  // Local free list first.  Mutex guards against the background reclaim
  // thread + concurrent allocators in the same process.
  {
    std::lock_guard<std::mutex> g(free_list_mutex_);
    if (!local_free_list_.empty()) {
      uint64_t off = local_free_list_.back();
      local_free_list_.pop_back();
      return off;
    }
  }

  // Bump cursor on this host's segment.
  uint64_t idx = cursors_[host_id_].bump.fetch_add(1, std::memory_order_acq_rel);
  if (idx >= records_per_host_) {
    // Out of space in this host's private segment.  Do a synchronous
    // reclaim_pass as a fallback (the background thread should normally
    // keep the local free list populated).
    bump_exhausts_.fetch_add(1, std::memory_order_relaxed);
    uint32_t harvested = reclaim_pass();
    if (harvested == 0) return 0;
    std::lock_guard<std::mutex> g(free_list_mutex_);
    if (local_free_list_.empty()) return 0;
    uint64_t off = local_free_list_.back();
    local_free_list_.pop_back();
    // Undo the over-bumped cursor so the next call doesn't keep bump-spinning.
    cursors_[host_id_].bump.fetch_sub(1, std::memory_order_acq_rel);
    return off;
  }
  // Flush cursor cacheline so peer hosts see the new bump value.  Not
  // strictly required for correctness (peers don't allocate from our
  // segment) but keeps diagnostics like total allocated count consistent.
  flush_line(&cursors_[host_id_].bump);
  store_fence();
  return segment_to_off(host_id_, idx);
}

void CxlFuseeKvPairPool::free(uint64_t off) {
  if (!base_) return;
  if (off < records_base_off_) return;  // not a record offset
  uint64_t rec_idx = (off - records_base_off_) / record_size_;
  if (rec_idx >= total_records_) return;

  uint64_t byte_idx = rec_idx / 8;
  uint8_t  bit_mask = static_cast<uint8_t>(1u << (rec_idx % 8));
  // Atomic OR on the bitmap byte. clflushopt + sfence so peer hosts and
  // future reclaim passes see the bit.
  __atomic_fetch_or(reinterpret_cast<uint8_t *>(&bitmap_[byte_idx]),
                    bit_mask, __ATOMIC_ACQ_REL);
  flush_line(&bitmap_[byte_idx]);
  store_fence();
  free_calls_.fetch_add(1, std::memory_order_relaxed);
}

uint32_t CxlFuseeKvPairPool::reclaim_pass() {
  if (!base_) return 0;

  // Scan only this host's segment of the bitmap.
  uint64_t first_rec = static_cast<uint64_t>(host_id_) * records_per_host_;
  uint64_t last_rec  = first_rec + records_per_host_;  // exclusive
  uint64_t first_byte = first_rec / 8;
  uint64_t last_byte  = (last_rec + 7) / 8;

  // Flush the segment's bitmap range so peer-host frees become visible.
  flush_region(&bitmap_[first_byte], last_byte - first_byte);
  full_fence();

  uint32_t harvested = 0;
  // Collect freed offsets locally, then take the mutex once at the end.
  std::vector<uint64_t> harvested_offsets;
  for (uint64_t b = first_byte; b < last_byte; b++) {
    uint8_t byte = bitmap_[b];
    if (byte == 0) continue;
    while (byte != 0) {
      int bit = __builtin_ctz(byte);
      uint64_t rec_idx = b * 8 + static_cast<uint64_t>(bit);
      if (rec_idx >= last_rec) break;
      uint8_t mask = static_cast<uint8_t>(1u << bit);
      uint8_t old  = __atomic_fetch_and(
          reinterpret_cast<uint8_t *>(&bitmap_[b]),
          static_cast<uint8_t>(~mask), __ATOMIC_ACQ_REL);
      if (old & mask) {
        uint64_t off = records_base_off_ + rec_idx * record_size_;
        harvested_offsets.push_back(off);
        harvested++;
      }
      byte &= static_cast<uint8_t>(~mask);
    }
    flush_line(&bitmap_[b]);
  }
  store_fence();

  if (!harvested_offsets.empty()) {
    std::lock_guard<std::mutex> g(free_list_mutex_);
    local_free_list_.insert(local_free_list_.end(),
                            harvested_offsets.begin(),
                            harvested_offsets.end());
  }

  reclaimed_total_.fetch_add(harvested, std::memory_order_relaxed);
  return harvested;
}

void CxlFuseeKvPairPool::write(uint64_t off, const void *data, uint32_t len) {
  if (!base_ || off < records_base_off_ || off + len > total_bytes_) return;
  uint8_t *dst = base_ + off;
  std::memcpy(dst, data, len);
  // Tiny records (16 B) fit in one cacheline; for variable-length records
  // larger than a cacheline (future), flush per cacheline.
  flush_line(dst);
  if (len > 64) {
    uint8_t *p = dst + 64;
    uint8_t *end = dst + len;
    while (p < end) {
      flush_line(p);
      p += 64;
    }
  }
  store_fence();
}

void CxlFuseeKvPairPool::start_reclaim_thread(uint32_t interval_ms) {
  if (reclaim_thread_.joinable()) return;
  if (interval_ms == 0) interval_ms = 10;
  reclaim_stop_.store(false, std::memory_order_release);
  reclaim_thread_ = std::thread([this, interval_ms]() {
    while (!reclaim_stop_.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
      if (reclaim_stop_.load(std::memory_order_acquire)) break;
      if (!base_) continue;
      (void)reclaim_pass();
    }
  });
}

void CxlFuseeKvPairPool::stop_reclaim_thread() {
  reclaim_stop_.store(true, std::memory_order_release);
  if (reclaim_thread_.joinable()) reclaim_thread_.join();
}

void CxlFuseeKvPairPool::read(uint64_t off, void *out, uint32_t len) const {
  if (!base_ || off < records_base_off_ || off + len > total_bytes_) return;
  uint8_t *src = const_cast<uint8_t *>(base_ + off);
  flush_line(src);
  if (len > 64) {
    uint8_t *p = src + 64;
    uint8_t *end = src + len;
    while (p < end) {
      flush_line(p);
      p += 64;
    }
  }
  full_fence();
  std::memcpy(out, src, len);
}

}  // namespace fusee
