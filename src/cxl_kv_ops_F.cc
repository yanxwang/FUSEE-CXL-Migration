#include "cxl_kv_ops_F.h"
#include "cxl_kv_ops_F_decomp.h"

#include "common.h"  // flush_line, store_fence, full_fence, flush_region

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace fusee {

thread_local int CxlKvStoreF::t_worker_id_ = -1;

namespace {

constexpr uint64_t kFuseeFHeaderMagic = 0x46555345505f4831ULL;  // "FUSEEP_H1"

inline std::size_t align_up(std::size_t n, std::size_t a) {
  return (n + (a - 1)) & ~(a - 1);
}

struct alignas(64) HeaderF {
  uint64_t magic;
  uint32_t num_buckets;
  uint32_t num_hosts;
  uint64_t total_records;
  uint64_t bucket_array_off;
  uint64_t pool_off;
  uint64_t init_done_bit;   // host 0 sets after init, host 1+ waits
  char _pad[64 - 48];
};
static_assert(sizeof(HeaderF) == 64);

constexpr std::size_t kHeaderBytes = sizeof(HeaderF);

constexpr int kInsertRetries = 8;

// Default pool multiplier for the API-parity overloads.  4 KV-pair records
// per slot covers a 4x out-of-place UPDATE churn before the reclaim path
// has to kick in.  Tune via FUSEE_F_TOTAL_RECORDS env var if needed.
constexpr uint64_t kFuseeFDefaultPoolMultiplier = 4ULL * kCxlFuseeSlotsPerBucket;

uint64_t env_total_records(uint32_t num_buckets) {
  const char *e = std::getenv("FUSEE_F_TOTAL_RECORDS");
  if (e && e[0]) return std::strtoull(e, nullptr, 0);
  return kFuseeFDefaultPoolMultiplier * num_buckets;
}

int env_num_hosts() {
  const char *e = std::getenv("FUSEE_F_NUM_HOSTS");
  if (e && e[0]) return std::atoi(e);
  return 2;
}

uint32_t env_reclaim_interval_ms() {
  const char *e = std::getenv("FUSEE_F_RECLAIM_INTERVAL_MS");
  if (e && e[0]) {
    long v = std::strtol(e, nullptr, 0);
    if (v > 0 && v < 60000) return static_cast<uint32_t>(v);
  }
  return 10;  // FUSEE §4.4 default
}

}  // namespace

std::size_t CxlKvStoreF::bytes_for(uint32_t num_buckets,
                                   uint64_t total_records,
                                   int      num_hosts,
                                   uint32_t record_size) {
  std::size_t hdr     = kHeaderBytes;
  std::size_t locks   = align_up(
      CxlFuseeSlotLockTable::bytes_for(num_buckets), 64);
  std::size_t buckets = align_up(num_buckets * sizeof(CxlFuseeBucket), 64);
  std::size_t pool    = CxlFuseeKvPairPool::bytes_for(
      total_records, num_hosts, record_size);
  return align_up(hdr + locks, 64) + buckets + pool;
}

std::size_t CxlKvStoreF::bytes_for(uint32_t num_buckets) {
  return bytes_for(num_buckets, env_total_records(num_buckets), env_num_hosts());
}

uint64_t CxlKvStoreF::slot_abs_off(uint32_t bucket, int slot) const {
  // Account for the FUSEE-shaped 8 B bucket header (local_depth + prefix).
  return buckets_off_
       + static_cast<uint64_t>(bucket) * sizeof(CxlFuseeBucket)
       + offsetof(CxlFuseeBucket, slots)
       + static_cast<uint64_t>(slot)   * sizeof(CxlFuseeSlot);
}

// Pick the LFM (id, num_ids) pair for this op.  Workers that called
// set_worker_id() get a unique id within total_workers_ (must be set
// via set_total_workers()).  Else fall back to (host_id, num_hosts) —
// only safe for single-thread-per-host runs.
static inline int lfm_id_of(int t_worker_id, int host_id_fallback) {
  return (t_worker_id >= 0) ? t_worker_id : host_id_fallback;
}
static inline int lfm_n_of(int total_workers, int num_hosts_fallback) {
  return (total_workers > 0) ? total_workers : num_hosts_fallback;
}

int CxlKvStoreF::attach(void *region_base, std::size_t region_bytes,
                        uint32_t num_buckets,
                        int host_id, int num_hosts, bool init_region,
                        bool /*read_only*/) {
  return attach(region_base, region_bytes, num_buckets,
                env_total_records(num_buckets),
                host_id, num_hosts, init_region,
                kFuseePoolTinyRecordSize);
}

int CxlKvStoreF::attach(void *region_base, std::size_t region_bytes,
                        uint32_t num_buckets, uint64_t total_records,
                        int host_id, int num_hosts, bool init_region,
                        uint32_t record_size) {
  if (!region_base || num_buckets == 0 || record_size == 0) return -1;
  if (record_size < 16) return -1;   // need at least key(8) + value(>=8)
  std::size_t need = bytes_for(num_buckets, total_records, num_hosts, record_size);
  if (region_bytes < need) return -1;

  region_base_  = reinterpret_cast<uint8_t *>(region_base);
  region_bytes_ = region_bytes;
  num_buckets_  = num_buckets;
  host_id_      = host_id;
  num_hosts_    = num_hosts;

  std::size_t locks_bytes = align_up(
      CxlFuseeSlotLockTable::bytes_for(num_buckets), 64);
  std::size_t locks_off = align_up(kHeaderBytes, 64);
  buckets_off_ = locks_off + locks_bytes;
  std::size_t buckets_bytes =
      align_up(num_buckets * sizeof(CxlFuseeBucket), 64);
  std::size_t pool_off = buckets_off_ + buckets_bytes;

  auto *hdr = reinterpret_cast<HeaderF *>(region_base_);
  buckets_  = reinterpret_cast<CxlFuseeBucket *>(region_base_ + buckets_off_);

  if (init_region) {
    std::memset(region_base_, 0, kHeaderBytes);
    // Zero the lock-table region before mutex init writes magic words.
    std::memset(region_base_ + locks_off, 0, locks_bytes);
    // Bucket array zeroed → every slot.packed == 0 → empty.
    std::memset(region_base_ + buckets_off_, 0, buckets_bytes);
    lock_table_.attach(region_base_ + locks_off, num_buckets_,
                       /*init_mutexes=*/true);
    hdr->magic            = kFuseeFHeaderMagic;
    hdr->num_buckets      = num_buckets_;
    hdr->num_hosts        = static_cast<uint32_t>(num_hosts_);
    hdr->total_records    = total_records;
    hdr->bucket_array_off = buckets_off_;
    hdr->pool_off         = pool_off;
    hdr->init_done_bit    = 0;
    flush_region(region_base_, kHeaderBytes);
    flush_region(region_base_ + locks_off, locks_bytes);
    flush_region(region_base_ + buckets_off_, buckets_bytes);
    store_fence();
  } else {
    // Wait on init_done_bit before reading any header field.
    for (;;) {
      flush_line(&hdr->init_done_bit);
      full_fence();
      if (hdr->init_done_bit) break;
      // Yield politely.
      for (int i = 0; i < 1024; i++) __builtin_ia32_pause();
    }
    if (hdr->magic != kFuseeFHeaderMagic) return -1;
    if (hdr->num_buckets != num_buckets_)  return -1;
    // Peer: attach lock table without re-initializing mutexes.
    lock_table_.attach(region_base_ + locks_off, num_buckets_,
                       /*init_mutexes=*/false);
  }

  // Pool attaches into the tail of the region.
  if (pool_.attach(region_base_ + pool_off,
                   region_bytes_ - pool_off,
                   total_records, host_id_, num_hosts_,
                   init_region, record_size) != 0) {
    return -1;
  }

  if (init_region) {
    // Publish init_done so peer hosts can attach.
    hdr->init_done_bit = 1;
    flush_line(&hdr->init_done_bit);
    store_fence();
  }

  // Start the background reclaim thread now that the pool is live (FUSEE
  // §4.4).  Keeps the alloc critical path off the bitmap-scan path.
  pool_.start_reclaim_thread(env_reclaim_interval_ms());

  return 0;
}

int CxlKvStoreF::insert(uint64_t key, uint64_t value) {
  return insert_blob(key, &value, sizeof(value));
}

int CxlKvStoreF::insert_blob(uint64_t key, const void *value, uint32_t value_len) {
  if (key == 0) return -1;
  if (8 + value_len > pool_.record_size()) return -1;
  uint32_t idx1 = bucket_idx(key);
  uint32_t idx2 = bucket_idx2(key);
  CxlFuseeBucket *b1 = &buckets_[idx1];
  CxlFuseeBucket *b2 = &buckets_[idx2];
  bool single = (idx1 == idx2);  // degenerate cuckoo collision
  uint8_t fp = cxl_fusee_fp(key);

  F_DECOMP_TS(_t_total_start);

  for (int attempt = 0; attempt < kInsertRetries; attempt++) {
    F_DECOMP_TS(_t0);
    flush_line(b1);
    if (!single) flush_line(b2);
    full_fence();
    F_DECOMP_TS(_t1);
    F_DECOMP_REC(kF_INS_PRE_FLUSH, _t0, _t1);

    // FUSEE-strict pre-scan: count empties in each candidate bucket;
    // pick the bucket with MORE free slots (mirror FUSEE
    // find_empty_slot's load-balance choice).  Tie → first bucket.  No
    // dup check — FUSEE assumes the workload guarantees uniqueness.
    int b1_empty = -1, b1_count = 0;
    for (int s = 0; s < kCxlFuseeSlotsPerBucket; s++) {
      if (b1->slots[s].packed == kCxlFuseeEmptySlot) {
        if (b1_empty < 0) b1_empty = s;
        b1_count++;
      }
    }
    int b2_empty = -1, b2_count = 0;
    if (!single) {
      for (int s = 0; s < kCxlFuseeSlotsPerBucket; s++) {
        if (b2->slots[s].packed == kCxlFuseeEmptySlot) {
          if (b2_empty < 0) b2_empty = s;
          b2_count++;
        }
      }
    }

    CxlFuseeBucket *chosen_b;
    uint32_t        chosen_idx;
    int             chosen_slot;
    if (b1_count >= b2_count && b1_empty >= 0) {
      chosen_b = b1; chosen_idx = idx1; chosen_slot = b1_empty;
    } else if (b2_empty >= 0) {
      chosen_b = b2; chosen_idx = idx2; chosen_slot = b2_empty;
    } else {
      return -1;  // both buckets full
    }
    F_DECOMP_TS(_t2);
    F_DECOMP_REC(kF_INS_PRE_SCAN, _t1, _t2);

    uint64_t new_off = pool_.alloc_tiny();
    if (new_off == 0) return -1;
    F_DECOMP_TS(_t3);
    F_DECOMP_REC(kF_INS_ALLOC, _t2, _t3);

    // Write key (8 B) at record offset 0, then value (value_len B) at offset 8.
    pool_.write(new_off, &key, sizeof(key));
    pool_.write(new_off + 8, value, value_len);
    F_DECOMP_TS(_t4);
    F_DECOMP_REC(kF_INS_WRITE_PAIR, _t3, _t4);

    lock_table_.lock_slot(chosen_idx, chosen_slot, lfm_id_of(t_worker_id_, host_id_), lfm_n_of(total_workers_, num_hosts_));
    F_DECOMP_TS(_t5);
    F_DECOMP_REC(kF_INS_LOCK, _t4, _t5);

    flush_line(&chosen_b->slots[chosen_slot]);
    full_fence();
    bool still_empty = (chosen_b->slots[chosen_slot].packed ==
                        kCxlFuseeEmptySlot);
    F_DECOMP_TS(_t6);
    F_DECOMP_REC(kF_INS_VERIFY, _t5, _t6);

    if (!still_empty) {
      lock_table_.unlock_slot(chosen_idx, chosen_slot, lfm_id_of(t_worker_id_, host_id_));
      pool_.free(new_off);
      continue;
    }

    chosen_b->slots[chosen_slot].packed =
        cxl_fusee_slot_pack(new_off, kFuseeSizeClassTiny, fp);
    flush_line(&chosen_b->slots[chosen_slot]);
    store_fence();
    F_DECOMP_TS(_t7);
    F_DECOMP_REC(kF_INS_PUBLISH, _t6, _t7);

    lock_table_.unlock_slot(chosen_idx, chosen_slot, lfm_id_of(t_worker_id_, host_id_));
    F_DECOMP_TS(_t8);
    F_DECOMP_REC(kF_INS_UNLOCK, _t7, _t8);

    index_cache_.insert(key, slot_abs_off(chosen_idx, chosen_slot), new_off);
    F_DECOMP_TS(_t9);
    F_DECOMP_REC(kF_INS_CACHE, _t8, _t9);
    F_DECOMP_REC(kF_INS_TOTAL, _t_total_start, _t9);
    return 0;
  }
  return -1;
}

int CxlKvStoreF::search(uint64_t key, uint64_t *out) const {
  uint32_t io_len = sizeof(uint64_t);
  return search_blob(key, out, &io_len);
}

int CxlKvStoreF::search_blob(uint64_t key, void *value_out,
                             uint32_t *io_value_len) const {
  if (key == 0) return -1;
  uint32_t want_len = io_value_len ? *io_value_len : 0;
  if (8 + want_len > pool_.record_size()) return -1;
  uint8_t fp = cxl_fusee_fp(key);

  F_DECOMP_TS(_t_total_start);

  // --- Fast path: cache hit, not bypassed (FUSEE §4.6) ---
  F_DECOMP_TS(_t0);
  CxlFuseeIndexCacheEntry cached{};
  bool cached_ok = index_cache_.lookup(key, &cached);
  F_DECOMP_TS(_t1);
  F_DECOMP_REC(kF_SRC_CACHE_LOOKUP, _t0, _t1);

  if (cached_ok && !cached.bypass) {
    auto *slot_ptr = reinterpret_cast<CxlFuseeSlot *>(
        region_base_ + cached.slot_addr);
    flush_line(slot_ptr);
    full_fence();
    uint64_t packed = slot_ptr->packed;
    uint64_t cur_blk_off = cxl_fusee_slot_blk_off(packed);

    uint64_t cached_key = 0;
    pool_.read(cached.kvpair_addr, &cached_key, sizeof(cached_key));

    if (cur_blk_off == cached.kvpair_addr) {
      if (cached_key == key) {
        F_DECOMP_TS(_t2);
        F_DECOMP_REC(kF_SRC_FAST_PATH, _t1, _t2);
        index_cache_.record_hit(key);
        F_DECOMP_TS(_t3);
        F_DECOMP_REC(kF_SRC_CACHE_UPDATE, _t2, _t3);
        F_DECOMP_REC(kF_SRC_TOTAL, _t_total_start, _t3);
        if (value_out && want_len > 0) {
          pool_.read(cached.kvpair_addr + 8, value_out, want_len);
        }
        if (io_value_len) *io_value_len = want_len;
        return 0;
      }
    } else {
      index_cache_.record_miss(key);
      uint64_t fresh_key = 0;
      pool_.read(cur_blk_off, &fresh_key, sizeof(fresh_key));
      if (fresh_key == key) {
        F_DECOMP_TS(_t2);
        F_DECOMP_REC(kF_SRC_FAST_PATH, _t1, _t2);
        index_cache_.insert(key, cached.slot_addr, cur_blk_off);
        F_DECOMP_TS(_t3);
        F_DECOMP_REC(kF_SRC_CACHE_UPDATE, _t2, _t3);
        F_DECOMP_REC(kF_SRC_TOTAL, _t_total_start, _t3);
        if (value_out && want_len > 0) {
          pool_.read(cur_blk_off + 8, value_out, want_len);
        }
        if (io_value_len) *io_value_len = want_len;
        return 0;
      }
    }
  }

  // --- Slow path: FUSEE-style cuckoo scan both candidate buckets ---
  F_DECOMP_TS(_t_slow_start);
  uint32_t idx1 = bucket_idx(key);
  uint32_t idx2 = bucket_idx2(key);
  CxlFuseeBucket *b1 = &buckets_[idx1];
  CxlFuseeBucket *b2 = &buckets_[idx2];
  bool single_b = (idx1 == idx2);
  flush_line(b1);
  if (!single_b) flush_line(b2);
  full_fence();

  for (int bb = 0; bb < (single_b ? 1 : 2); bb++) {
    CxlFuseeBucket *bp = (bb == 0) ? b1 : b2;
    uint32_t        bi = (bb == 0) ? idx1 : idx2;
    for (int s = 0; s < kCxlFuseeSlotsPerBucket; s++) {
      uint64_t packed = bp->slots[s].packed;
      if (packed == kCxlFuseeEmptySlot) continue;
      if (cxl_fusee_slot_fp(packed) != fp) continue;
      uint64_t blk_off = cxl_fusee_slot_blk_off(packed);
      uint64_t pair_key = 0;
      pool_.read(blk_off, &pair_key, sizeof(pair_key));
      if (pair_key == key) {
        F_DECOMP_TS(_t_slow_end);
        F_DECOMP_REC(kF_SRC_SLOW_PATH, _t_slow_start, _t_slow_end);
        index_cache_.insert(key, slot_abs_off(bi, s), blk_off);
        F_DECOMP_TS(_t_cache_end);
        F_DECOMP_REC(kF_SRC_CACHE_UPDATE, _t_slow_end, _t_cache_end);
        F_DECOMP_REC(kF_SRC_TOTAL, _t_total_start, _t_cache_end);
        if (value_out && want_len > 0) {
          pool_.read(blk_off + 8, value_out, want_len);
        }
        if (io_value_len) *io_value_len = want_len;
        return 0;
      }
    }
  }
  F_DECOMP_TS(_t_slow_end_miss);
  F_DECOMP_REC(kF_SRC_SLOW_PATH, _t_slow_start, _t_slow_end_miss);
  F_DECOMP_REC(kF_SRC_TOTAL, _t_total_start, _t_slow_end_miss);
  return -1;
}

namespace {

// Locate the (bucket, slot) holding the key across both cuckoo
// candidate buckets.  Returns slot index in *out_slot and which bucket
// (0 or 1) in *out_which.  Returns -1 if not found.
// Caller has already flushed both bucket cachelines and issued mfence.
int find_slot_for_key_cuckoo(CxlFuseeBucket *b1, CxlFuseeBucket *b2,
                             bool single,
                             uint64_t key, uint8_t fp,
                             const CxlFuseeKvPairPool &pool,
                             int *out_which) {
  for (int s = 0; s < kCxlFuseeSlotsPerBucket; s++) {
    uint64_t packed = b1->slots[s].packed;
    if (packed == kCxlFuseeEmptySlot) continue;
    if (cxl_fusee_slot_fp(packed) != fp) continue;
    uint64_t blk_off = cxl_fusee_slot_blk_off(packed);
    uint64_t pair_key = 0;
    pool.read(blk_off, &pair_key, sizeof(pair_key));
    if (pair_key == key) { *out_which = 0; return s; }
  }
  if (single) return -1;
  for (int s = 0; s < kCxlFuseeSlotsPerBucket; s++) {
    uint64_t packed = b2->slots[s].packed;
    if (packed == kCxlFuseeEmptySlot) continue;
    if (cxl_fusee_slot_fp(packed) != fp) continue;
    uint64_t blk_off = cxl_fusee_slot_blk_off(packed);
    uint64_t pair_key2 = 0;
    pool.read(blk_off, &pair_key2, sizeof(pair_key2));
    if (pair_key2 == key) { *out_which = 1; return s; }
  }
  return -1;
}

}  // namespace

int CxlKvStoreF::update(uint64_t key, uint64_t value) {
  return update_blob(key, &value, sizeof(value));
}

int CxlKvStoreF::update_blob(uint64_t key, const void *value, uint32_t value_len) {
  if (key == 0) return -1;
  if (8 + value_len > pool_.record_size()) return -1;
  uint32_t idx1 = bucket_idx(key);
  uint32_t idx2 = bucket_idx2(key);
  CxlFuseeBucket *b1 = &buckets_[idx1];
  CxlFuseeBucket *b2 = &buckets_[idx2];
  bool single = (idx1 == idx2);
  uint8_t fp = cxl_fusee_fp(key);

  F_DECOMP_TS(_t_total_start);

  for (int attempt = 0; attempt < kInsertRetries; attempt++) {
    F_DECOMP_TS(_t0);
    flush_line(b1);
    if (!single) flush_line(b2);
    full_fence();
    int which = 0;
    int s = find_slot_for_key_cuckoo(b1, b2, single, key, fp, pool_, &which);
    F_DECOMP_TS(_t1);
    F_DECOMP_REC(kF_UPD_FLUSH_SCAN, _t0, _t1);
    if (s < 0) return -1;
    CxlFuseeBucket *b = (which == 0) ? b1 : b2;
    uint32_t        idx = (which == 0) ? idx1 : idx2;

    uint64_t new_off = pool_.alloc_tiny();
    if (new_off == 0) return -1;
    pool_.write(new_off, &key, sizeof(key));
    pool_.write(new_off + 8, value, value_len);
    F_DECOMP_TS(_t2);
    F_DECOMP_REC(kF_UPD_ALLOC_WRITE, _t1, _t2);

    lock_table_.lock_slot(idx, s, lfm_id_of(t_worker_id_, host_id_), lfm_n_of(total_workers_, num_hosts_));
    F_DECOMP_TS(_t3);
    F_DECOMP_REC(kF_UPD_LOCK, _t2, _t3);

    // FUSEE-strict CAS semantics: any slot change → return 0 (treat as
    // applied by the concurrent writer).
    flush_line(&b->slots[s]);
    full_fence();
    uint64_t packed_now = b->slots[s].packed;
    if (packed_now == kCxlFuseeEmptySlot ||
        cxl_fusee_slot_fp(packed_now) != fp) {
      lock_table_.unlock_slot(idx, s, lfm_id_of(t_worker_id_, host_id_));
      pool_.free(new_off);
      return 0;
    }
    uint64_t cur_blk_off = cxl_fusee_slot_blk_off(packed_now);
    uint64_t cur_pair_key = 0;
    pool_.read(cur_blk_off, &cur_pair_key, sizeof(cur_pair_key));
    if (cur_pair_key != key) {
      lock_table_.unlock_slot(idx, s, lfm_id_of(t_worker_id_, host_id_));
      pool_.free(new_off);
      return 0;
    }
    uint64_t old_blk_off = cur_blk_off;
    F_DECOMP_TS(_t4);
    F_DECOMP_REC(kF_UPD_VERIFY, _t3, _t4);

    b->slots[s].packed = cxl_fusee_slot_pack(new_off,
                                             kFuseeSizeClassTiny, fp);
    flush_line(&b->slots[s]);
    store_fence();
    F_DECOMP_TS(_t5);
    F_DECOMP_REC(kF_UPD_PUBLISH, _t4, _t5);

    lock_table_.unlock_slot(idx, s, lfm_id_of(t_worker_id_, host_id_));
    F_DECOMP_TS(_t6);
    F_DECOMP_REC(kF_UPD_UNLOCK, _t5, _t6);

    pool_.free(old_blk_off);
    F_DECOMP_TS(_t7);
    F_DECOMP_REC(kF_UPD_FREE, _t6, _t7);

    index_cache_.insert(key, slot_abs_off(idx, s), new_off);
    F_DECOMP_TS(_t8);
    F_DECOMP_REC(kF_UPD_CACHE, _t7, _t8);
    F_DECOMP_REC(kF_UPD_TOTAL, _t_total_start, _t8);
    return 0;
  }
  return -1;
}

int CxlKvStoreF::remove(uint64_t key) {
  if (key == 0) return -1;
  uint32_t idx1 = bucket_idx(key);
  uint32_t idx2 = bucket_idx2(key);
  CxlFuseeBucket *b1 = &buckets_[idx1];
  CxlFuseeBucket *b2 = &buckets_[idx2];
  bool single = (idx1 == idx2);
  uint8_t fp = cxl_fusee_fp(key);

  F_DECOMP_TS(_t_total_start);

  for (int attempt = 0; attempt < kInsertRetries; attempt++) {
    F_DECOMP_TS(_t0);
    flush_line(b1);
    if (!single) flush_line(b2);
    full_fence();
    int which = 0;
    int s = find_slot_for_key_cuckoo(b1, b2, single, key, fp, pool_, &which);
    F_DECOMP_TS(_t1);
    F_DECOMP_REC(kF_DEL_FLUSH_SCAN, _t0, _t1);
    if (s < 0) return -1;
    CxlFuseeBucket *b = (which == 0) ? b1 : b2;
    uint32_t        idx = (which == 0) ? idx1 : idx2;

    lock_table_.lock_slot(idx, s, lfm_id_of(t_worker_id_, host_id_), lfm_n_of(total_workers_, num_hosts_));
    F_DECOMP_TS(_t2);
    F_DECOMP_REC(kF_DEL_LOCK, _t1, _t2);

    // FUSEE-strict CAS semantics: if the slot was modified by another
    // writer between our pre-scan and lock acquire, treat our DELETE as
    // "linearizably applied" (FUSEE modify_primary_idx_sync sets
    // KV_OPS_SUCCESS for DELETE on CAS-fail).
    flush_line(&b->slots[s]);
    full_fence();
    uint64_t packed_now = b->slots[s].packed;
    if (packed_now == kCxlFuseeEmptySlot ||
        cxl_fusee_slot_fp(packed_now) != fp) {
      lock_table_.unlock_slot(idx, s, lfm_id_of(t_worker_id_, host_id_));
      return 0;
    }
    uint64_t cur_blk_off = cxl_fusee_slot_blk_off(packed_now);
    uint64_t cur_pair_key = 0;
    pool_.read(cur_blk_off, &cur_pair_key, sizeof(cur_pair_key));
    if (cur_pair_key != key) {
      lock_table_.unlock_slot(idx, s, lfm_id_of(t_worker_id_, host_id_));
      return 0;
    }
    uint64_t old_blk_off = cur_blk_off;
    F_DECOMP_TS(_t3);
    F_DECOMP_REC(kF_DEL_VERIFY, _t2, _t3);

    b->slots[s].packed = kCxlFuseeEmptySlot;
    flush_line(&b->slots[s]);
    store_fence();
    F_DECOMP_TS(_t4);
    F_DECOMP_REC(kF_DEL_CLEAR, _t3, _t4);

    lock_table_.unlock_slot(idx, s, lfm_id_of(t_worker_id_, host_id_));
    F_DECOMP_TS(_t5);
    F_DECOMP_REC(kF_DEL_UNLOCK, _t4, _t5);

    pool_.free(old_blk_off);
    F_DECOMP_TS(_t6);
    F_DECOMP_REC(kF_DEL_FREE, _t5, _t6);

    index_cache_.evict(key);
    F_DECOMP_TS(_t7);
    F_DECOMP_REC(kF_DEL_CACHE, _t6, _t7);
    F_DECOMP_REC(kF_DEL_TOTAL, _t_total_start, _t7);
    return 0;
  }
  return -1;
}

}  // namespace fusee
