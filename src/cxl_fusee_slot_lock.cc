#include "cxl_fusee_slot_lock.h"

extern "C" {
#include "common.h"   // flush_line, store_fence, CACHELINE_STORE, SHM_MUTEX_MAGIC
}

#include <cstddef>

namespace fusee {

size_t CxlFuseeSlotLockTable::bytes_for(uint32_t num_buckets) {
  return sizeof(CxlFuseeSlotLockEntry) * num_buckets;
}

void CxlFuseeSlotLockTable::attach(void *base, uint32_t num_buckets,
                                   bool init_mutexes) {
  entries_     = static_cast<CxlFuseeSlotLockEntry *>(base);
  num_buckets_ = num_buckets;
  if (init_mutexes) {
    // FAST PATH: assume the bulk memset in CxlKvStoreF::attach already
    // zeroed every byte of the lock-table region and the bulk
    // flush_region already pushed it to CXL.  All we need to do is
    // publish the SHM_MUTEX_MAGIC per mutex.  This avoids the
    // shm_mutex_init() per-mutex memset(38KB) + flush_region(38KB) +
    // magic store that would otherwise dominate attach time at large
    // num_buckets (was ~30 s at 65K buckets; now ~50 ms).
    for (uint32_t i = 0; i < num_buckets; i++) {
      for (int s = 0; s < kCxlFuseeSlotsPerBucket; s++) {
        auto *m = &entries_[i].slot_mutexes[s];
        CACHELINE_STORE(&m->magic, SHM_MUTEX_MAGIC);
        flush_line(&m->magic);
      }
    }
    store_fence();
  }
}

uint64_t CxlFuseeSlotLockTable::lock_slot(uint32_t bucket_idx, int slot_idx,
                                          int host_id, int num_hosts) {
  return shm_mutex_lock(&entries_[bucket_idx].slot_mutexes[slot_idx],
                        host_id, num_hosts);
}

void CxlFuseeSlotLockTable::unlock_slot(uint32_t bucket_idx, int slot_idx,
                                        int host_id) {
  shm_mutex_unlock(&entries_[bucket_idx].slot_mutexes[slot_idx], host_id);
}

CxlFuseeSlotLockEntry *CxlFuseeSlotLockTable::entry(uint32_t bucket_idx) const {
  return &entries_[bucket_idx];
}

}  // namespace fusee
