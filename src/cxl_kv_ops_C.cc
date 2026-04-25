#include "cxl_kv_ops_C.h"

#include <cassert>
#include <chrono>
#include <cstring>
#include <limits>
#include <thread>

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
static inline uint64_t bump_epoch(Entry *e) {
  // write_epoch is a cacheline_u64; both BucketLockEntry and SlotLockEntry
  // expose it at the same name. Per-bucket lock serialised the increment;
  // per-slot lock does NOT (concurrent writers on different slots of the
  // same bucket race here). Use atomic fetch-add so the count never drops
  // updates, and surround with the clflushopt+sfence pattern expected by
  // readers on peer hosts. Returns the new epoch for callers that want to
  // advance their local DRAM cache to match (iter-2 cache refresh).
  uint64_t *p = (uint64_t *)&e->write_epoch.value;
  uint64_t new_val = __atomic_add_fetch(p, 1ULL, __ATOMIC_ACQ_REL);
  flush_line(p);
  store_fence();
  return new_val;
}

#if defined(FUSEE_PER_SLOT_LOCK) && FUSEE_PER_SLOT_LOCK
// Phase-2.5 route_seq: INSERT and DELETE may move a key to a different slot;
// UPDATE never does. Writers that change slot layout bump this; UPDATE
// readers observe it before and after lock_slot() and skip the under-lock
// key re-verify when unchanged. Same cross-host flush pattern as write_epoch.
static inline uint64_t bump_route_seq(SlotLockEntry *e) {
  uint64_t *p = (uint64_t *)&e->route_seq.value;
  uint64_t new_val = __atomic_add_fetch(p, 1ULL, __ATOMIC_ACQ_REL);
  flush_line(p);
  store_fence();
  return new_val;
}

static inline uint64_t load_route_seq(SlotLockEntry *e) {
  return CACHELINE_LOAD(&e->route_seq);
}
#endif

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
    // Phase-2.6 flush-collapse: bucket is 128 B = 2 cachelines; one
    // clflushopt per line covers all 7 slots' (key, value) pairs.
    flush_line(&b->slots[0]);
    flush_line(&b->slots[4]);
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
    // a racing insert could have landed in any of them. Phase-2.6
    // flush-collapse: 2 clflushopts cover all 7 slots.
    flush_line(&b->slots[0]);
    flush_line(&b->slots[4]);
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
    // Iter-2: release the slot lock BEFORE bumping the per-bucket epoch.
    // Slot lock only serialised writers on the same slot; the atomic
    // bump_epoch below serialises globally on the bucket's write_epoch,
    // and readers seqlock on that epoch rather than on the slot lock.
    // Letting the next writer on this slot enter while we bump saves ~2 µs
    // from the hot-slot critical section.
    lock_table_.unlock_slot(idx, empty_slot_i);
    // Phase-2.5 route_seq: INSERT moves a key into a new slot; bump so any
    // UPDATE reader in flight abandons its cached (key->slot) mapping.
    bump_route_seq(lock_table_.entry(idx));
    uint64_t new_epoch = bump_epoch(lock_table_.entry(idx));
    DECOMP_DECL(__dt4);

    if (logged) oplog_->commit(log_idx);
    // Iter-2: refresh local DRAM cache with the slot we just wrote so
    // same-process reads hit DRAM immediately instead of roundtripping
    // to CXL. Peer-host readers still invalidate via write_epoch
    // mismatch. Correctness: our cache may race with another concurrent
    // writer on a DIFFERENT slot of this bucket, but cache_buckets_ is
    // per-process; there is no concurrent writer within this process.
    if (cache_enabled_) {
      cache_buckets_[idx].slots[empty_slot_i].key = key;
      cache_buckets_[idx].slots[empty_slot_i].value = value;
      cache_epoch_[idx] = new_epoch;
    }
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

  // Phase-3 micro-batching fast path: when batching is enabled, UPDATE
  // deposits into the per-host DRAM ring with no slot-lock and no per-op
  // CXL epoch bump. Flusher amortises one bump over K writes.
  if (batch_enabled_) {
    DECOMP_DECL(__dt0);
    flush_line(&b->slots[0]);
    flush_line(&b->slots[4]);
    full_fence();
    int target = -1;
    for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
      if (b->slots[s].key == key) { target = s; break; }
    }
    if (target < 0) return -1;
    DECOMP_DECL(__dt1);
    batch_ring_.append(idx, static_cast<uint16_t>(target), value,
                       &ring_full_waits_);
    // Read-your-writes: refresh local DRAM cache so same-process reads
    // observe the write before the flusher materialises it onto CXL.
    if (cache_enabled_) {
      cache_buckets_[idx].slots[target].value = value;
      // Advance cache_epoch_ optimistically; on next peer-host write
      // the CXL epoch will overtake ours and we'll refetch.
    }
    DECOMP_DECL(__dt2);
    DECOMP_DECL(__dt3);
    DECOMP_DECL(__dt4);
    DECOMP_DECL(__dt5);
    DECOMP_REC(kDecompStageLock,    __dt0, __dt0);  // no lock
    DECOMP_REC(kDecompStageScan,    __dt0, __dt1);
    DECOMP_REC(kDecompStagePublish, __dt1, __dt2);
    DECOMP_REC(kDecompStageEpoch,   __dt2, __dt2);  // no bump (amortised)
    DECOMP_REC(kDecompStageUnlock,  __dt2, __dt2);
    DECOMP_REC(kDecompStageTotal,   __dt0, __dt2);
    return 0;
  }

  for (int attempt = 0; attempt < kInsertRetries; attempt++) {
    DECOMP_DECL(__dt0);
    // Phase-2.5 route_seq: snapshot BEFORE the unlocked pre-scan so that
    // any INSERT/DELETE that completes between the pre-scan and our
    // lock_slot() return is detected and we fall back to an under-lock
    // re-verify. If the seq is unchanged, no such INSERT/DELETE can have
    // moved our target key away, so the re-verify is pure overhead.
    SlotLockEntry *le = lock_table_.entry(idx);
    uint64_t seq_before = load_route_seq(le);

    // Unlocked scan to find slot holding our key.
    // Phase-2.6 flush-collapse.
    flush_line(&b->slots[0]);
    flush_line(&b->slots[4]);
    full_fence();
    int target = -1;
    for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
      if (b->slots[s].key == key) { target = s; break; }
    }
    if (target < 0) return -1;  // not found

    lock_table_.lock_slot(idx, target);
    DECOMP_DECL(__dt1);

    // Phase-2.5 fast path: if no INSERT/DELETE moved anything while we
    // were grabbing the lock, trust the pre-scan result. Saves one CXL
    // clflushopt + mfence + load per UPDATE on workloads where
    // INSERT/DELETE pressure is low (A/B/F trans phase = zero
    // INSERT/DELETE → always fast path).
    if (load_route_seq(le) != seq_before) {
      // Slow path: re-verify key under slot lock. Phase-2.5-inner falls
      // back here when the layout actually changed.
      flush_line(&b->slots[target].key);
      full_fence();
      if (b->slots[target].key != key) {
        lock_table_.unlock_slot(idx, target);
        continue;  // racing delete+insert moved the key; try again
      }
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
    // Iter-2: unlock before bump_epoch. See insert() for rationale.
    lock_table_.unlock_slot(idx, target);
    uint64_t new_epoch = bump_epoch(lock_table_.entry(idx));
    DECOMP_DECL(__dt4);
    if (logged) oplog_->commit(log_idx);
    if (cache_enabled_) {
      cache_buckets_[idx].slots[target].key = key;
      cache_buckets_[idx].slots[target].value = value;
      cache_epoch_[idx] = new_epoch;
    }
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
    // Phase-2.6 flush-collapse.
    flush_line(&b->slots[0]);
    flush_line(&b->slots[4]);
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
    // Iter-2: unlock before bump_epoch.
    lock_table_.unlock_slot(idx, target);
    // Phase-2.5 route_seq: DELETE clears a slot, which changes the
    // (key -> slot) mapping observed by concurrent UPDATE pre-scans.
    bump_route_seq(lock_table_.entry(idx));
    uint64_t new_epoch = bump_epoch(lock_table_.entry(idx));
    if (logged) oplog_->commit(log_idx);
    if (cache_enabled_) {
      cache_buckets_[idx].slots[target].key = kEmptyKey;
      cache_epoch_[idx] = new_epoch;
    }
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

  // Phase-2.4 read-singleshot: one CXL read pass, no epoch revalidation.
  // x86 aligned u64 loads are atomic, so `key` and `value` are individually
  // torn-free; a slot pair mid-written may show (old_key, new_value) or
  // (new_key, old_value) but the returned `value` is still a value that
  // was committed at some instant. LRC semantics relax from "consistent
  // snapshot across the scan window" to "snapshot at some instant during
  // scan" — acceptable per docs/design_goals.md Option C definition.
  uint64_t e1 = CACHELINE_LOAD(&le->write_epoch);

  uint64_t captured = 0;
  bool found = false;
  // Phase-2.6 flush-collapse: 128 B bucket = 2 cachelines.
  flush_line(&b->slots[0]);
  flush_line(&b->slots[4]);
  full_fence();
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
  if (cache_enabled_) cache_epoch_[idx] = e1;
  if (found) {
    if (out) *out = captured;
    return 0;
  }
  return -1;
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

// ---------- Phase-3 micro-batching ----------

int CxlKvStoreC::enable_batching(void *shm_base, std::size_t shm_bytes,
                                 uint32_t K, uint32_t T_flush_us,
                                 bool init_region, uint32_t num_flushers) {
  if (batch_enabled_) return -1;
  if (batch_ring_.attach(shm_base, shm_bytes, num_buckets_, K, T_flush_us,
                         init_region, num_flushers) != 0) {
    return -1;
  }
  batch_enabled_ = true;
  return 0;
}

void CxlKvStoreC::drain_bucket(uint32_t idx) {
  BucketRingCursors *c = &batch_ring_.cursors()[idx];
  uint64_t append_raw = c->append_cursor.load(std::memory_order_acquire);
  uint64_t flush_cur  = __atomic_load_n(&c->flush_cursor, __ATOMIC_ACQUIRE);
  if (append_raw == flush_cur) return;

  const uint32_t K = batch_ring_.K();
  // Cap drain window at flush_cur + K: any writer that already claimed a
  // pos >= flush_cur + K is spin-waiting for flush_cursor to advance
  // BEFORE writing its entry. If the flusher tries to spin on that entry's
  // ready flag it deadlocks against the writer. Positions in
  // [flush_cur, flush_cur + K) have writers that either already published
  // their flag or are in the short non-blocking critical section just
  // before the RELEASE store — flusher's short spin on flags is bounded.
  uint64_t append_end = append_raw;
  if (append_end > flush_cur + K) append_end = flush_cur + K;
  RingEntry *ring_base = batch_ring_.ring() +
      static_cast<std::size_t>(idx) * K;

#if defined(FUSEE_BATCH_MERGE_SAME_KEY) && FUSEE_BATCH_MERGE_SAME_KEY
  // Collapse to last-writer-wins per slot. At most 7 slots in a bucket,
  // so a tiny fixed-size array beats a hashmap.
  int64_t latest_pos[kCxlKvSlotsPerBucket];
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) latest_pos[s] = -1;
  uint64_t p = flush_cur;
  const uint64_t end = append_end;
  while (p < end) {
    RingEntry *e = &ring_base[p % K];
    // Wait for the ready flag to be published by the writer.
    while (__atomic_load_n(&e->flags, __ATOMIC_ACQUIRE) == 0) {
      __builtin_ia32_pause();
    }
    if (e->slot_idx < kCxlKvSlotsPerBucket) {
      latest_pos[e->slot_idx] = static_cast<int64_t>(p);
    }
    p++;
  }
  // Apply each slot's final value.
  CxlKvBucket *bucket = &buckets_[idx];
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
    if (latest_pos[s] < 0) continue;
    RingEntry *e = &ring_base[latest_pos[s] % K];
    bucket->slots[s].value = e->new_value;
  }
#else
  // No merge: apply in cursor order.
  CxlKvBucket *bucket = &buckets_[idx];
  uint64_t p = flush_cur;
  const uint64_t end = append_end;
  while (p < end) {
    RingEntry *e = &ring_base[p % K];
    while (__atomic_load_n(&e->flags, __ATOMIC_ACQUIRE) == 0) {
      __builtin_ia32_pause();
    }
    if (e->slot_idx < kCxlKvSlotsPerBucket) {
      bucket->slots[e->slot_idx].value = e->new_value;
    }
    p++;
  }
#endif

  // Single-bucket publish: 2 flushes cover both cachelines.
  flush_line(&bucket->slots[0]);
  flush_line(&bucket->slots[4]);
  store_fence();

  // Single epoch bump amortises all K entries (or merged subset).
  uint64_t new_epoch = bump_epoch(lock_table_.entry(idx));
  (void)new_epoch;

  // Clear ready flags for the range we just drained so the next wrap-around
  // can tell "not yet written" from stale.
  p = flush_cur;
  while (p < end) {
    RingEntry *e = &ring_base[p % K];
    __atomic_store_n(&e->flags, static_cast<uint16_t>(0), __ATOMIC_RELEASE);
    p++;
  }

  // Advance flush_cursor last — after this, producers that were spinning
  // on ring-full see the window open.
  __atomic_store_n(&c->flush_cursor, append_end, __ATOMIC_RELEASE);
  // Release the `queued` flag so a subsequent writer's first append will
  // re-enqueue this bucket. Must come AFTER flush_cursor advance so any
  // writer that already CAS-won after us sees our new flush_cursor.
  c->queued.store(0, std::memory_order_release);
}

void CxlKvStoreC::flusher_loop(int my_id) {
  BatchRingHeader *hdr = batch_ring_.header();
  DirtyQueueShard &dq = hdr->dq[my_id];
  const uint32_t N = batch_ring_.num_flushers();
  const uint32_t T_us = batch_ring_.T_flush_us();
  auto get_us = []() -> uint64_t {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL
         + static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
  };
  // Full-table scan is O(num_buckets / N) per flusher (we walk only our
  // own partition). We do NOT scan every T_us. Instead we scan only when
  // the dirty queue has been empty for at least `kIdleScanUs` — under
  // steady workload the queue is rarely empty, so the scan essentially
  // never fires; at shutdown the dedicated drain in stop_flusher covers
  // any residual entries.
  const uint64_t kIdleScanUs = 5000;  // 5 ms idle → scan
  uint64_t last_activity_us = get_us();
  while (!hdr->stop.load(std::memory_order_acquire)) {
    bool did_any = false;
    for (int batch = 0; batch < 1024; batch++) {
      uint64_t tail = dq.dq_tail.load(std::memory_order_acquire);
      uint64_t head = __atomic_load_n(&dq.dq_head, __ATOMIC_ACQUIRE);
      if (head >= tail) break;
      uint32_t idx = __atomic_load_n(
          &dq.dq_slots[head % kDirtyQueueCapacity], __ATOMIC_ACQUIRE);
      __atomic_store_n(&dq.dq_head, head + 1, __ATOMIC_RELEASE);
      drain_bucket(idx);
      did_any = true;
    }
    if (did_any) {
      last_activity_us = get_us();
      continue;
    }
    uint64_t now = get_us();
    if (now - last_activity_us >= kIdleScanUs) {
      // Long-idle fallback scan: my partition only.
      for (uint32_t idx = (uint32_t)my_id; idx < batch_ring_.num_buckets();
           idx += N) {
        if (batch_ring_.has_pending(idx)) drain_bucket(idx);
      }
      last_activity_us = now;
      continue;
    }
    // Short nap so we do not pin the core.
    (void)T_us;  // kept for future adaptive sleep tuning
    timespec ts = {0, 10 * 1000};  // 10 us
    nanosleep(&ts, nullptr);
  }
  // Final drain — partition only.
  for (uint32_t idx = (uint32_t)my_id; idx < batch_ring_.num_buckets();
       idx += N) {
    if (batch_ring_.has_pending(idx)) drain_bucket(idx);
  }
}

void CxlKvStoreC::start_flusher() {
  if (!batch_enabled_) return;
  bool expected = false;
  if (!flusher_started_.compare_exchange_strong(expected, true)) return;
  uint32_t N = batch_ring_.num_flushers();
  if (N == 0) N = 1;
  flusher_threads_.reserve(N);
  for (uint32_t i = 0; i < N; i++) {
    flusher_threads_.emplace_back(
        [this, i]() { this->flusher_loop(static_cast<int>(i)); });
  }
}

void CxlKvStoreC::stop_flusher() {
  if (!flusher_started_.load()) return;
  BatchRingHeader *hdr = batch_ring_.header();
  hdr->stop.store(true, std::memory_order_release);
  for (auto &t : flusher_threads_) {
    if (t.joinable()) t.join();
  }
  flusher_threads_.clear();
  flusher_started_.store(false);
}

// ---------- Iter-4 variadic-length API (block pool-backed) ----------
//
// Dual-path design (see docs/task_plan_20260424_variable_kv_size.md
// §4.4): if `blockpool_` is null OR `value_len <= 8`, fall back to the
// inline u64 fast path (zero overhead vs iter-3). Otherwise allocate a
// pool block, write the bytes there, and publish the offset in the
// slot's `value` field — the slot field semantically becomes a pool
// offset (`blk_off`) but the on-disk bytes / layout / size are
// unchanged.

int CxlKvStoreC::insert(uint64_t key, const void *value, uint32_t value_len) {
  if (!value || value_len == 0) return -1;
  if (!blockpool_ || value_len <= sizeof(uint64_t)) {
    // Inline fast path: pack the bytes into a u64 and route through
    // the legacy insert(key, u64).
    uint64_t v = 0;
    std::memcpy(&v, value, value_len);
    return insert(key, v);
  }
  if (value_len > blockpool_->block_size()) return -1;
  uint64_t off = blockpool_->alloc();
  if (off == 0) return -1;
  blockpool_->write(off, value, value_len);
  // Publish the offset as the slot's "value" field.
  return insert(key, off);
}

int CxlKvStoreC::update(uint64_t key, const void *value, uint32_t value_len) {
  if (!value || value_len == 0) return -1;
  if (!blockpool_ || value_len <= sizeof(uint64_t)) {
    uint64_t v = 0;
    std::memcpy(&v, value, value_len);
    return update(key, v);
  }
  if (value_len > blockpool_->block_size()) return -1;
  uint64_t new_off = blockpool_->alloc();
  if (new_off == 0) return -1;
  blockpool_->write(new_off, value, value_len);
  // Substitute new offset in the existing UPDATE path. The old slot's
  // offset becomes garbage (lazy-free stub — see plan §8.4).
  int rc = update(key, new_off);
  // No need to read the old offset: free_lazy is a no-op stub for now;
  // sweep workloads stay well within the 2 GiB pool / host. iter-5 GC
  // will reclaim.
  return rc;
}

int CxlKvStoreC::search(uint64_t key, void *out_buf, uint32_t out_cap,
                        uint32_t *out_len) const {
  if (!out_buf) return -1;
  uint64_t v = 0;
  int rc = search(key, &v);
  if (rc != 0) return rc;
  if (!blockpool_) {
    // Inline fast path: bytes live in the slot value field directly.
    uint32_t n = out_cap < sizeof(uint64_t) ? out_cap
                                            : (uint32_t)sizeof(uint64_t);
    std::memcpy(out_buf, &v, n);
    if (out_len) *out_len = n;
    return 0;
  }
  // Pool path: v is the offset.
  uint32_t bs = blockpool_->block_size();
  uint32_t n = out_cap < bs ? out_cap : bs;
  blockpool_->read(v, out_buf, n);
  if (out_len) *out_len = n;
  return 0;
}

} // namespace fusee
