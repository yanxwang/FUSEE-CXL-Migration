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

} // namespace fusee
