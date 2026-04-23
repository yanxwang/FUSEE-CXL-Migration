#include "cxl_kv_ops_C.h"

#include <cassert>
#include <cstring>
#include <limits>

extern "C" {
#include "common.h"  // cacheline_u64, CACHELINE_LOAD/STORE, flush_line, fences
}

#include "cxl_latency_decomp_probe.h"

#if defined(FUSEE_LATENCY_DECOMP) && FUSEE_LATENCY_DECOMP
#include <time.h>
namespace {
inline uint64_t decomp_now_ns() {
  timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
} // namespace
#define DECOMP_DECL(name) uint64_t name = decomp_now_ns()
#define DECOMP_REC(stage, a, b) ::fusee::decomp_record(::fusee::stage, (b) - (a))
#else
#define DECOMP_DECL(name) ((void)0)
#define DECOMP_REC(stage, a, b) ((void)0)
#endif

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

#if defined(FUSEE_PER_SLOT_LOCK) && FUSEE_PER_SLOT_LOCK
using CBucketLockTable = SlotLockTable;
#else
using CBucketLockTable = BucketLockTable;
#endif

size_t CxlKvStoreC::bytes_for(uint32_t num_buckets) {
  size_t locks = CBucketLockTable::bytes_for(num_buckets);
  size_t after_locks = align_up(kHeaderBytes + locks, 64);
  size_t buckets = sizeof(CxlKvBucket) * num_buckets;
  return after_locks + buckets;
}

int CxlKvStoreC::attach(void *region_base, size_t region_bytes,
                        uint32_t num_buckets, int host_id, int num_hosts,
                        bool init_region, bool read_only) {
  (void)read_only;  // C has no replicator; accepted only for API parity.
  if (!region_base || num_buckets == 0) return -1;
  size_t need = bytes_for(num_buckets);
  if (region_bytes < need) return -1;

  num_buckets_ = num_buckets;
  host_id_ = host_id;
  num_hosts_ = num_hosts;

  auto *base = reinterpret_cast<char *>(region_base);
  void *locks_base = base + kHeaderBytes;
  lock_table_.attach(locks_base, num_buckets, init_region);

  size_t locks = CBucketLockTable::bytes_for(num_buckets);
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

template <class Entry>
static inline void bump_epoch(Entry *e) {
  // write_epoch is a cacheline_u64; both BucketLockEntry and SlotLockEntry
  // expose it at the same name. Per-bucket lock serialised the increment;
  // per-slot lock does NOT (concurrent writers on different slots of the
  // same bucket race here). Use atomic fetch-add so the count never drops
  // updates, and surround with the clflushopt+sfence pattern expected by
  // readers on peer hosts.
  uint64_t *p = (uint64_t *)&e->write_epoch.value;
  __atomic_fetch_add(p, 1ULL, __ATOMIC_ACQ_REL);
  flush_line(p);
  store_fence();
}

#if defined(FUSEE_PER_SLOT_LOCK) && FUSEE_PER_SLOT_LOCK
// ----- Per-slot lock write path (Phase-2b) -----
//
// UPDATE/DELETE: unlocked scan for matching key, lock the slot we found,
// re-verify key still matches under the slot lock, mutate, bump epoch,
// unlock. If the key moved during the race, scan the bucket once more
// under no lock (bounded retries) before giving up.
//
// INSERT: unlocked scan for duplicate + empty-slot candidate. Lock the
// candidate slot, verify it is still empty under lock, AND re-scan the
// other 6 slots for duplicate (another insert may have raced us into a
// different slot). If the candidate was taken by a racer, release and
// retry with another empty slot, bounded by kInsertRetries. If a dup
// emerged, release and return -2.

constexpr int kInsertRetries = 8;

int CxlKvStoreC::insert(uint64_t key, uint64_t value) {
  if (key == kEmptyKey) return -1;
  uint32_t idx = bucket_idx(key);
  CxlKvBucket *b = &buckets_[idx];

  for (int attempt = 0; attempt < kInsertRetries; attempt++) {
    DECOMP_DECL(__dt0);
    // Unlocked scan to find a candidate empty slot and dup key.
    for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
      flush_line(&b->slots[s].key);
    }
    full_fence();
    int empty_slot_i = -1;
    bool dup = false;
    for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
      uint64_t k = b->slots[s].key;
      if (k == key) { dup = true; break; }
      if (empty_slot_i < 0 && k == kEmptyKey) empty_slot_i = s;
    }
    if (dup) return -2;
    if (empty_slot_i < 0) return -1;  // full

    lock_table_.lock_slot(idx, empty_slot_i);
    DECOMP_DECL(__dt1);

    // Under slot lock, re-verify. Must also scan other slots for dup since
    // a racing insert could have landed in any of them.
    for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
      flush_line(&b->slots[s].key);
    }
    full_fence();
    bool dup_now = false;
    for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
      if (s == empty_slot_i) continue;
      if (b->slots[s].key == key) { dup_now = true; break; }
    }
    bool still_empty = (b->slots[empty_slot_i].key == kEmptyKey);
    DECOMP_DECL(__dt2);
    if (dup_now) {
      lock_table_.unlock_slot(idx, empty_slot_i);
      return -2;
    }
    if (!still_empty) {
      // Another insert took this slot; release and retry with another empty.
      lock_table_.unlock_slot(idx, empty_slot_i);
      continue;
    }

    CxlKvSlot *empty = &b->slots[empty_slot_i];
    uint64_t log_idx = 0;
    bool logged = false;
    if (oplog_) {
      log_idx = oplog_->begin(OpLogKind::Insert, key, idx, (uint64_t)empty_slot_i,
                              /*old_value=*/0, /*new_value=*/value);
      logged = true;
    }

    publish_slot(empty, key, value);
    DECOMP_DECL(__dt3);
    bump_epoch(lock_table_.entry(idx));
    DECOMP_DECL(__dt4);

    if (logged) oplog_->commit(log_idx);
    if (cache_enabled_) cache_epoch_[idx] = std::numeric_limits<uint64_t>::max();
    lock_table_.unlock_slot(idx, empty_slot_i);
    DECOMP_DECL(__dt5);
    DECOMP_REC(kDecompStageLock,    __dt0, __dt1);
    DECOMP_REC(kDecompStageScan,    __dt1, __dt2);
    DECOMP_REC(kDecompStagePublish, __dt2, __dt3);
    DECOMP_REC(kDecompStageEpoch,   __dt3, __dt4);
    DECOMP_REC(kDecompStageUnlock,  __dt4, __dt5);
    DECOMP_REC(kDecompStageTotal,   __dt0, __dt5);
    return 0;
  }
  return -1;  // too many retries — treat as full/contested
}

int CxlKvStoreC::update(uint64_t key, uint64_t value) {
  if (key == kEmptyKey) return -1;
  uint32_t idx = bucket_idx(key);
  CxlKvBucket *b = &buckets_[idx];

  for (int attempt = 0; attempt < kInsertRetries; attempt++) {
    DECOMP_DECL(__dt0);
    // Unlocked scan to find slot holding our key.
    for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
      flush_line(&b->slots[s].key);
    }
    full_fence();
    int target = -1;
    for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
      if (b->slots[s].key == key) { target = s; break; }
    }
    if (target < 0) return -1;  // not found

    lock_table_.lock_slot(idx, target);
    DECOMP_DECL(__dt1);

    // Re-verify key under slot lock.
    flush_line(&b->slots[target].key);
    full_fence();
    if (b->slots[target].key != key) {
      lock_table_.unlock_slot(idx, target);
      continue;  // racing delete+insert moved the key; try again
    }
    DECOMP_DECL(__dt2);

    uint64_t log_idx = 0;
    bool logged = false;
    if (oplog_) {
      log_idx = oplog_->begin(OpLogKind::Update, key, idx, (uint64_t)target,
                              b->slots[target].value, value);
      logged = true;
    }
    b->slots[target].value = value;
    flush_line(&b->slots[target].value);
    store_fence();
    DECOMP_DECL(__dt3);
    bump_epoch(lock_table_.entry(idx));
    DECOMP_DECL(__dt4);
    if (logged) oplog_->commit(log_idx);
    if (cache_enabled_) cache_epoch_[idx] = std::numeric_limits<uint64_t>::max();
    lock_table_.unlock_slot(idx, target);
    DECOMP_DECL(__dt5);
    DECOMP_REC(kDecompStageLock,    __dt0, __dt1);
    DECOMP_REC(kDecompStageScan,    __dt1, __dt2);
    DECOMP_REC(kDecompStagePublish, __dt2, __dt3);
    DECOMP_REC(kDecompStageEpoch,   __dt3, __dt4);
    DECOMP_REC(kDecompStageUnlock,  __dt4, __dt5);
    DECOMP_REC(kDecompStageTotal,   __dt0, __dt5);
    return 0;
  }
  return -1;
}

int CxlKvStoreC::remove(uint64_t key) {
  if (key == kEmptyKey) return -1;
  uint32_t idx = bucket_idx(key);
  CxlKvBucket *b = &buckets_[idx];

  for (int attempt = 0; attempt < kInsertRetries; attempt++) {
    for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
      flush_line(&b->slots[s].key);
    }
    full_fence();
    int target = -1;
    for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
      if (b->slots[s].key == key) { target = s; break; }
    }
    if (target < 0) return -1;

    lock_table_.lock_slot(idx, target);
    flush_line(&b->slots[target].key);
    full_fence();
    if (b->slots[target].key != key) {
      lock_table_.unlock_slot(idx, target);
      continue;
    }

    uint64_t log_idx = 0;
    bool logged = false;
    if (oplog_) {
      log_idx = oplog_->begin(OpLogKind::Delete, key, idx, (uint64_t)target,
                              b->slots[target].value, 0);
      logged = true;
    }
    b->slots[target].key = kEmptyKey;
    flush_line(&b->slots[target].key);
    store_fence();
    bump_epoch(lock_table_.entry(idx));
    if (logged) oplog_->commit(log_idx);
    if (cache_enabled_) cache_epoch_[idx] = std::numeric_limits<uint64_t>::max();
    lock_table_.unlock_slot(idx, target);
    return 0;
  }
  return -1;
}

#else  // FUSEE_PER_SLOT_LOCK

int CxlKvStoreC::insert(uint64_t key, uint64_t value) {
  if (key == kEmptyKey) return -1;
  uint32_t idx = bucket_idx(key);
  DECOMP_DECL(__dt0);
  lock_table_.lock(idx, host_id_, num_hosts_);
  DECOMP_DECL(__dt1);

  CxlKvBucket *b = &buckets_[idx];
  CxlKvSlot *empty = nullptr;
  int empty_slot_i = -1;
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
    if (!empty && k == kEmptyKey) { empty = &b->slots[s]; empty_slot_i = s; }
  }
  if (!empty) {
    lock_table_.unlock(idx, host_id_);
    return -1; // full
  }
  DECOMP_DECL(__dt2);

  uint64_t log_idx = 0;
  bool logged = false;
  if (oplog_) {
    log_idx = oplog_->begin(OpLogKind::Insert, key, idx, (uint64_t)empty_slot_i,
                            /*old_value=*/0, /*new_value=*/value);
    logged = true;
  }

  publish_slot(empty, key, value);
  DECOMP_DECL(__dt3);
  bump_epoch(lock_table_.entry(idx));
  DECOMP_DECL(__dt4);

  if (logged) oplog_->commit(log_idx);
  if (cache_enabled_) cache_epoch_[idx] = std::numeric_limits<uint64_t>::max();
  lock_table_.unlock(idx, host_id_);
  DECOMP_DECL(__dt5);
  DECOMP_REC(kDecompStageLock,    __dt0, __dt1);
  DECOMP_REC(kDecompStageScan,    __dt1, __dt2);
  DECOMP_REC(kDecompStagePublish, __dt2, __dt3);
  DECOMP_REC(kDecompStageEpoch,   __dt3, __dt4);
  DECOMP_REC(kDecompStageUnlock,  __dt4, __dt5);
  DECOMP_REC(kDecompStageTotal,   __dt0, __dt5);
  return 0;
}

int CxlKvStoreC::update(uint64_t key, uint64_t value) {
  if (key == kEmptyKey) return -1;
  uint32_t idx = bucket_idx(key);
  DECOMP_DECL(__dt0);
  lock_table_.lock(idx, host_id_, num_hosts_);
  DECOMP_DECL(__dt1);

  CxlKvBucket *b = &buckets_[idx];
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
    flush_line(&b->slots[s].key);
  }
  full_fence();
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
    if (b->slots[s].key == key) {
      DECOMP_DECL(__dt2);
      uint64_t log_idx = 0;
      bool logged = false;
      if (oplog_) {
        log_idx = oplog_->begin(OpLogKind::Update, key, idx, (uint64_t)s,
                                b->slots[s].value, value);
        logged = true;
      }
      b->slots[s].value = value;
      flush_line(&b->slots[s].value);
      store_fence();
      DECOMP_DECL(__dt3);
      bump_epoch(lock_table_.entry(idx));
      DECOMP_DECL(__dt4);
      if (logged) oplog_->commit(log_idx);
      if (cache_enabled_) cache_epoch_[idx] = std::numeric_limits<uint64_t>::max();
      lock_table_.unlock(idx, host_id_);
      DECOMP_DECL(__dt5);
      DECOMP_REC(kDecompStageLock,    __dt0, __dt1);
      DECOMP_REC(kDecompStageScan,    __dt1, __dt2);
      DECOMP_REC(kDecompStagePublish, __dt2, __dt3);
      DECOMP_REC(kDecompStageEpoch,   __dt3, __dt4);
      DECOMP_REC(kDecompStageUnlock,  __dt4, __dt5);
      DECOMP_REC(kDecompStageTotal,   __dt0, __dt5);
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
      uint64_t log_idx = 0;
      bool logged = false;
      if (oplog_) {
        log_idx = oplog_->begin(OpLogKind::Delete, key, idx, (uint64_t)s,
                                b->slots[s].value, 0);
        logged = true;
      }
      b->slots[s].key = kEmptyKey;
      flush_line(&b->slots[s].key);
      store_fence();
      bump_epoch(lock_table_.entry(idx));
      if (logged) oplog_->commit(log_idx);
      if (cache_enabled_) cache_epoch_[idx] = std::numeric_limits<uint64_t>::max();
      lock_table_.unlock(idx, host_id_);
      return 0;
    }
  }
  lock_table_.unlock(idx, host_id_);
  return -1;
}

#endif  // FUSEE_PER_SLOT_LOCK

int CxlKvStoreC::search(uint64_t key, uint64_t *out) const {
  if (key == kEmptyKey) return -1;
  uint32_t idx = bucket_idx(key);
  auto *le = const_cast<CBucketLockTable &>(lock_table_).entry(idx);
  CxlKvBucket *b = &buckets_[idx];

  // Fast path: DRAM cache hit. Read the CXL epoch once; if it matches the
  // cached epoch, scan the DRAM copy without flushing any slot cachelines.
  if (cache_enabled_) {
    uint64_t cached = cache_epoch_[idx];
    if (cached != std::numeric_limits<uint64_t>::max()) {
      uint64_t cxl_epoch = CACHELINE_LOAD(&le->write_epoch);
      if (cxl_epoch == cached) {
        const CxlKvBucket &cb = cache_buckets_[idx];
        for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
          if (cb.slots[s].key == key) {
            if (out) *out = cb.slots[s].value;
            return 0;
          }
        }
        return -1;
      }
    }
  }

  for (int attempt = 0; attempt < 8; attempt++) {
    uint64_t e1 = CACHELINE_LOAD(&le->write_epoch);

    uint64_t captured = 0;
    bool found = false;
    for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
      flush_line(&b->slots[s].key);
      flush_line(&b->slots[s].value);
    }
    full_fence();
    // Also snapshot the whole bucket into the DRAM cache while we are here.
    if (cache_enabled_) {
      cache_buckets_[idx] = *b;
    }
    for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
      if (b->slots[s].key == key) {
        captured = b->slots[s].value;
        found = true;
        break;
      }
    }

    uint64_t e2 = CACHELINE_LOAD(&le->write_epoch);
    if (e1 == e2) {
      if (cache_enabled_) cache_epoch_[idx] = e1;
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

uint64_t CxlKvStoreC::recover_from_oplog() {
  if (!oplog_) return 0;

  // Temporarily detach the oplog so that our redo-side insert/update/remove
  // calls do not re-log (and re-recover) themselves forever. Restore after.
  OpLog *saved = oplog_;
  oplog_ = nullptr;

  auto redo = +[](const OpLogEntry *e, void *user) -> int {
    auto *self = reinterpret_cast<CxlKvStoreC *>(user);
    int rc = 0;
    switch (e->kind) {
      case OpLogKind::Insert:
        rc = self->insert(e->key, e->new_value);
        // -2 == duplicate: the insert already took effect before the crash.
        if (rc == -2) rc = 0;
        break;
      case OpLogKind::Update:
        rc = self->update(e->key, e->new_value);
        // -1 == key not present: the pre-crash writer never reached the
        // bucket. Falling through to insert is safe because the bucket is
        // locked and we will not race any concurrent writer during recovery.
        if (rc == -1) {
          rc = self->insert(e->key, e->new_value);
          if (rc == -2) rc = 0;
        }
        break;
      case OpLogKind::Delete:
        rc = self->remove(e->key);
        if (rc == -1) rc = 0; // already gone
        break;
      default:
        rc = -1;
    }
    return rc;
  };

  uint64_t acted = saved->recover_redo(redo, this);
  oplog_ = saved;
  return acted;
}

void CxlKvStoreC::enable_dram_cache(bool on) {
  cache_enabled_ = on;
  if (on) {
    cache_buckets_.assign(num_buckets_, CxlKvBucket{});
    cache_epoch_.assign(num_buckets_, std::numeric_limits<uint64_t>::max());
  } else {
    cache_buckets_.clear();
    cache_epoch_.clear();
  }
}

} // namespace fusee
