#include "cxl_kv_ops_C.h"

#include <cassert>
#include <cstring>

extern "C" {
#include "common.h"  // cacheline_u64, CACHELINE_LOAD/STORE, flush_line, fences
}

namespace fusee {

// Region layout (static offsets, derived from `num_buckets`):
//   [0                                   .. HDR_BYTES)      header (future)
//   [HDR_BYTES                           .. HDR + LOCKS)    BucketLockEntry[]
//   [HDR + LOCKS, cacheline-aligned      .. ... )           CxlKvBucket[]
//
// For Phase 3 the header is reserved but unused; we just start locks at a
// fixed 4 KiB offset so later phases can slot in a magic/version header.

constexpr size_t kHeaderBytes = 4096;

static inline size_t align_up(size_t n, size_t a) {
  return (n + a - 1) & ~(a - 1);
}

size_t CxlKvStoreC::bytes_for(uint32_t num_buckets) {
  size_t locks = BucketLockTable::bytes_for(num_buckets);
  size_t after_locks = align_up(kHeaderBytes + locks, 64);
  size_t buckets = sizeof(CxlKvBucket) * num_buckets;
  return after_locks + buckets;
}

int CxlKvStoreC::attach(void *region_base, size_t region_bytes,
                        uint32_t num_buckets, int host_id, int num_hosts,
                        bool init_region) {
  if (!region_base || num_buckets == 0) return -1;
  size_t need = bytes_for(num_buckets);
  if (region_bytes < need) return -1;

  num_buckets_ = num_buckets;
  host_id_ = host_id;
  num_hosts_ = num_hosts;

  auto *base = reinterpret_cast<char *>(region_base);
  void *locks_base = base + kHeaderBytes;
  lock_table_.attach(locks_base, num_buckets, init_region);

  size_t locks = BucketLockTable::bytes_for(num_buckets);
  size_t after_locks = align_up(kHeaderBytes + locks, 64);
  buckets_ = reinterpret_cast<CxlKvBucket *>(base + after_locks);

  if (init_region) {
    // Zero every slot so (key == kEmptyKey) means empty.
    for (uint32_t b = 0; b < num_buckets_; b++) {
      for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
        buckets_[b].slots[s].key = kEmptyKey;
        buckets_[b].slots[s].value = 0;
      }
      // Best-effort publish — readers on other hosts wait on write_epoch
      // transitions before they care about bucket contents anyway.
      flush_region(&buckets_[b], sizeof(CxlKvBucket));
    }
    store_fence();
  }

  return 0;
}

// Write-side helpers: publish a slot's fields in a safe order. Value first,
// then key, then epoch bump (done by caller). A racing reader is protected by
// seqlock retry on epoch mismatch; this ordering only matters if a reader
// tries to be optimistic and skip the final epoch check — we still always
// do that check, so the ordering is belt-and-suspenders.
static inline void publish_slot(CxlKvSlot *slot, uint64_t key, uint64_t value) {
  slot->value = value;
  flush_line(&slot->value);
  store_fence();
  slot->key = key;
  flush_line(&slot->key);
  store_fence();
}

static inline void bump_epoch(BucketLockEntry *e) {
  // write_epoch is a cacheline_u64; CACHELINE_STORE fences internally.
  uint64_t cur = CACHELINE_LOAD(&e->write_epoch);
  CACHELINE_STORE(&e->write_epoch, cur + 1);
}

int CxlKvStoreC::insert(uint64_t key, uint64_t value) {
  if (key == kEmptyKey) return -1;
  uint32_t idx = bucket_idx(key);
  lock_table_.lock(idx, host_id_, num_hosts_);

  CxlKvBucket *b = &buckets_[idx];
  CxlKvSlot *empty = nullptr;
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
    flush_line(&b->slots[s].key);
  }
  full_fence();
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
    uint64_t k = b->slots[s].key;
    if (k == key) {
      lock_table_.unlock(idx, host_id_);
      return -2; // duplicate
    }
    if (!empty && k == kEmptyKey) empty = &b->slots[s];
  }
  if (!empty) {
    lock_table_.unlock(idx, host_id_);
    return -1; // full
  }
  publish_slot(empty, key, value);
  bump_epoch(lock_table_.entry(idx));
  lock_table_.unlock(idx, host_id_);
  return 0;
}

int CxlKvStoreC::update(uint64_t key, uint64_t value) {
  if (key == kEmptyKey) return -1;
  uint32_t idx = bucket_idx(key);
  lock_table_.lock(idx, host_id_, num_hosts_);

  CxlKvBucket *b = &buckets_[idx];
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
    flush_line(&b->slots[s].key);
  }
  full_fence();
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
    if (b->slots[s].key == key) {
      b->slots[s].value = value;
      flush_line(&b->slots[s].value);
      store_fence();
      bump_epoch(lock_table_.entry(idx));
      lock_table_.unlock(idx, host_id_);
      return 0;
    }
  }
  lock_table_.unlock(idx, host_id_);
  return -1;
}

int CxlKvStoreC::remove(uint64_t key) {
  if (key == kEmptyKey) return -1;
  uint32_t idx = bucket_idx(key);
  lock_table_.lock(idx, host_id_, num_hosts_);

  CxlKvBucket *b = &buckets_[idx];
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
    flush_line(&b->slots[s].key);
  }
  full_fence();
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
    if (b->slots[s].key == key) {
      b->slots[s].key = kEmptyKey;
      flush_line(&b->slots[s].key);
      store_fence();
      bump_epoch(lock_table_.entry(idx));
      lock_table_.unlock(idx, host_id_);
      return 0;
    }
  }
  lock_table_.unlock(idx, host_id_);
  return -1;
}

int CxlKvStoreC::search(uint64_t key, uint64_t *out) const {
  if (key == kEmptyKey) return -1;
  uint32_t idx = bucket_idx(key);
  BucketLockEntry *le = const_cast<BucketLockTable &>(lock_table_).entry(idx);
  CxlKvBucket *b = &buckets_[idx];

  for (int attempt = 0; attempt < 8; attempt++) {
    uint64_t e1 = CACHELINE_LOAD(&le->write_epoch);

    uint64_t captured = 0;
    bool found = false;
    for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
      flush_line(&b->slots[s].key);
      flush_line(&b->slots[s].value);
    }
    full_fence();
    for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
      if (b->slots[s].key == key) {
        captured = b->slots[s].value;
        found = true;
        break;
      }
    }

    uint64_t e2 = CACHELINE_LOAD(&le->write_epoch);
    if (e1 == e2) {
      if (found) {
        if (out) *out = captured;
        return 0;
      }
      return -1;
    }
    // Epoch advanced mid-read; retry.
  }
  return -1; // too many retries; treat as not found.
}

} // namespace fusee
