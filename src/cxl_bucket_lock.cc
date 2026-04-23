#include "cxl_bucket_lock.h"

#include <cassert>

extern "C" {
#include "common.h"  // CACHELINE_STORE
}

namespace fusee {

size_t BucketLockTable::bytes_for(uint32_t num_buckets) {
  return sizeof(BucketLockEntry) * num_buckets;
}

void BucketLockTable::attach(void *base, uint32_t num_buckets,
                             bool init_mutexes) {
  assert(base != nullptr);
  entries_ = reinterpret_cast<BucketLockEntry *>(base);
  num_buckets_ = num_buckets;
  if (init_mutexes) {
    for (uint32_t i = 0; i < num_buckets; i++) {
#if defined(FUSEE_USE_TICKET_LOCK) && FUSEE_USE_TICKET_LOCK
      ticket_mutex_init(&entries_[i].mutex);
#else
      shm_mutex_init(&entries_[i].mutex);
#endif
      CACHELINE_STORE(&entries_[i].write_epoch, 0ULL);
      CACHELINE_STORE(&entries_[i].staging_scratch, 0ULL);
    }
  }
}

BucketLockEntry *BucketLockTable::entry(uint32_t bucket_idx) const {
  return &entries_[bucket_idx];
}

uint64_t BucketLockTable::lock(uint32_t bucket_idx, int host_id,
                               int num_hosts) {
#if defined(FUSEE_USE_TICKET_LOCK) && FUSEE_USE_TICKET_LOCK
  (void)host_id; (void)num_hosts;
  ticket_mutex_lock(&entries_[bucket_idx].mutex);
  return 0;
#else
  return shm_mutex_lock(&entries_[bucket_idx].mutex, host_id, num_hosts);
#endif
}

void BucketLockTable::unlock(uint32_t bucket_idx, int host_id) {
#if defined(FUSEE_USE_TICKET_LOCK) && FUSEE_USE_TICKET_LOCK
  (void)host_id;
  ticket_mutex_unlock(&entries_[bucket_idx].mutex);
#else
  shm_mutex_unlock(&entries_[bucket_idx].mutex, host_id);
#endif
}

// ---------- SlotLockTable (Phase-2b per-slot lock) ----------

size_t SlotLockTable::bytes_for(uint32_t num_buckets) {
  return sizeof(SlotLockEntry) * num_buckets;
}

void SlotLockTable::attach(void *base, uint32_t num_buckets,
                           bool init_mutexes) {
  assert(base != nullptr);
  entries_ = reinterpret_cast<SlotLockEntry *>(base);
  num_buckets_ = num_buckets;
  if (init_mutexes) {
    for (uint32_t i = 0; i < num_buckets; i++) {
      for (int s = 0; s < kSlotMutexesPerBucket; s++) {
#if defined(FUSEE_USE_TICKET_LOCK) && FUSEE_USE_TICKET_LOCK
        ticket_mutex_init(&entries_[i].slot_mutexes[s]);
#else
        shm_mutex_init(&entries_[i].slot_mutexes[s]);
#endif
      }
      CACHELINE_STORE(&entries_[i].write_epoch, 0ULL);
      CACHELINE_STORE(&entries_[i].staging_scratch, 0ULL);
    }
  }
}

SlotLockEntry *SlotLockTable::entry(uint32_t bucket_idx) const {
  return &entries_[bucket_idx];
}

void SlotLockTable::lock_slot(uint32_t bucket_idx, int slot_idx) {
#if defined(FUSEE_USE_TICKET_LOCK) && FUSEE_USE_TICKET_LOCK
  ticket_mutex_lock(&entries_[bucket_idx].slot_mutexes[slot_idx]);
#else
  // LFM uses (host_id, num_hosts) — we can't expose those here without
  // threading them through the call sites. Since LFM's primary cost is
  // the O(num_hosts) scan over b[], pass a small synthetic (host_id=0,
  // num_hosts=1) pair so each process's forked client doesn't amplify
  // the scan. This keeps LFM's correctness (the mutex still serialises
  // all callers via the shared b[]/ready[]/done[] arrays).
  shm_mutex_lock(&entries_[bucket_idx].slot_mutexes[slot_idx], 0, 1);
#endif
}

void SlotLockTable::unlock_slot(uint32_t bucket_idx, int slot_idx) {
#if defined(FUSEE_USE_TICKET_LOCK) && FUSEE_USE_TICKET_LOCK
  ticket_mutex_unlock(&entries_[bucket_idx].slot_mutexes[slot_idx]);
#else
  shm_mutex_unlock(&entries_[bucket_idx].slot_mutexes[slot_idx], 0);
#endif
}

} // namespace fusee
