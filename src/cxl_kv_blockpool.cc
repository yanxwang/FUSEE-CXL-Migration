#include "cxl_kv_blockpool.h"

#include <cassert>
#include <cstring>

extern "C" {
#include "common.h"  // flush_line, store_fence, full_fence
}

namespace fusee {

namespace {
inline std::size_t align_up(std::size_t n, std::size_t a) {
  return (n + a - 1) & ~(a - 1);
}
}  // namespace

std::size_t CxlKvBlockPool::bytes_for(uint32_t num_blocks_per_host,
                                       uint32_t block_size, int num_hosts) {
  std::size_t hdr = sizeof(Header);
  std::size_t cursors = static_cast<std::size_t>(num_hosts) * sizeof(HostCursor);
  std::size_t segs = static_cast<std::size_t>(num_hosts) *
                     static_cast<std::size_t>(num_blocks_per_host) *
                     block_size;
  return align_up(hdr + cursors, 64) + align_up(segs, 64);
}

int CxlKvBlockPool::attach(void *base, std::size_t bytes,
                           uint32_t num_blocks_per_host, uint32_t block_size,
                           int host_id, int num_hosts, bool init_region) {
  if (!base || num_hosts < 1 || num_hosts > kMaxPoolHosts) return -1;
  if (host_id < 0 || host_id >= num_hosts) return -1;
  if (block_size == 0 || num_blocks_per_host == 0) return -1;
  std::size_t need = bytes_for(num_blocks_per_host, block_size, num_hosts);
  if (bytes < need) return -1;

  base_ = reinterpret_cast<uint8_t *>(base);
  total_bytes_ = bytes;
  block_size_ = block_size;
  num_blocks_per_host_ = num_blocks_per_host;
  host_id_ = host_id;
  num_hosts_ = num_hosts;

  Header *hdr = reinterpret_cast<Header *>(base_);
  cursors_ = reinterpret_cast<HostCursor *>(base_ + sizeof(Header));
  header_bytes_ = align_up(sizeof(Header) +
                           static_cast<std::size_t>(num_hosts) * sizeof(HostCursor),
                           64);
  seg_base_ = base_ + header_bytes_ +
              static_cast<std::size_t>(host_id) *
              static_cast<std::size_t>(num_blocks_per_host) * block_size;

  if (init_region) {
    std::memset(base_, 0, need);
    hdr->magic = kPoolMagic;
    hdr->block_size = block_size;
    hdr->num_blocks_per_host = num_blocks_per_host;
    hdr->num_hosts = static_cast<uint32_t>(num_hosts);
    flush_region(base_, sizeof(Header) +
                 static_cast<std::size_t>(num_hosts) * sizeof(HostCursor));
    store_fence();
  } else {
    // Non-primary: spin until magic visible.
    while (true) {
      flush_line(&hdr->magic);
      full_fence();
      if (hdr->magic == kPoolMagic) break;
    }
  }
  return 0;
}

uint64_t CxlKvBlockPool::alloc() {
  if (!base_) return 0;
  uint64_t idx = cursors_[host_id_].bump.fetch_add(1, std::memory_order_acq_rel);
  if (idx >= num_blocks_per_host_) {
    // Exhausted; back off the cursor (best-effort) and return 0.
    cursors_[host_id_].bump.store(num_blocks_per_host_,
                                   std::memory_order_release);
    return 0;
  }
  uint64_t off = static_cast<uint64_t>(seg_base_ - base_) +
                 idx * static_cast<uint64_t>(block_size_);
  return off;
}

void CxlKvBlockPool::write(uint64_t off, const void *data, uint32_t len) {
  if (off == 0 || len == 0 || len > block_size_) return;
  uint8_t *dst = base_ + off;
  std::memcpy(dst, data, len);
  // Flush each touched cacheline.
  std::size_t flushed = 0;
  uint8_t *p = reinterpret_cast<uint8_t *>(
      reinterpret_cast<uintptr_t>(dst) & ~static_cast<uintptr_t>(63));
  while (p < dst + len) {
    flush_line(p);
    p += 64;
    flushed += 64;
  }
  store_fence();
  (void)flushed;
}

void CxlKvBlockPool::read(uint64_t off, void *out, uint32_t len) const {
  if (off == 0 || len == 0 || len > block_size_) return;
  const uint8_t *src = base_ + off;
  // Flush each cacheline so subsequent load goes to CXL memory (peer-host
  // writes become visible).
  uint8_t *p = reinterpret_cast<uint8_t *>(
      const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(
          reinterpret_cast<uintptr_t>(src) & ~static_cast<uintptr_t>(63))));
  uint8_t *end = const_cast<uint8_t *>(src + len);
  while (p < end) {
    flush_line(p);
    p += 64;
  }
  full_fence();
  std::memcpy(out, src, len);
}

}  // namespace fusee
