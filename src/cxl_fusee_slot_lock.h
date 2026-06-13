#ifndef FUSEE_CXL_FUSEE_SLOT_LOCK_H_
#define FUSEE_CXL_FUSEE_SLOT_LOCK_H_

// Per-slot LFM lock table for Protocol F.
//
// 14 LFM mutexes per bucket entry, one per slot.  Writers serialize at slot
// granularity, software-substituting for FUSEE's per-slot RDMA_CAS.
//
// Reader-side: Protocol F readers do NOT touch this table.  Slot atomicity
// + KV pair immutability make readers correct without any lock acquisition
// (docs/protocol_F_design_and_plan.md §2.5.3, §3).
//
// Memory budget — shared 200-host shm_mutex_t (cxl_shm_profiling).
//   sizeof(shm_mutex_t) = 3 * (MAX_HOST_NUM + 1) cachelines = ~38.6 KB
//   per bucket = 7 mutexes * 38.6 KB = 270 KB
//   at NUM_BUCKETS = 32 K (Fig 13 default):  ~8.6 GB lock state
//   at NUM_BUCKETS = 64 K (Fig 11 default):  ~17 GB lock state
//
// REVERTED 2026-06-08 from the Protocol F-local 2-host LFM
// (`fusee_lfm_t`).  The 2-host LFM only had `b[0..1]` arrays sized for
// 2 hosts — but Protocol F's multi-thread benchmarks use up to 128
// worker threads (64/host × 2 hosts), each needing a UNIQUE LFM `id`.
// With the 2-host LFM, all threads on host 0 shared `id=0` → they
// raced on `b[0]`, breaking mutual exclusion → silent data corruption.
//
// The shared shm_mutex_t (MAX_HOST_NUM=200) gives 200 LFM ids, enough
// for our 128-worker max.  Lock-acquire passes `worker_global_id` and
// `total_workers` (not physical host_id / num_hosts).

#include "cxl_fusee_bucket.h"

#include <stddef.h>
#include <stdint.h>

extern "C" {
#include "locks/lfm_lock.h"
}

namespace fusee {

struct CxlFuseeSlotLockEntry {
  shm_mutex_t slot_mutexes[kCxlFuseeSlotsPerBucket];   // 7 per bucket
};

class CxlFuseeSlotLockTable {
 public:
  CxlFuseeSlotLockTable() = default;

  // Place a table of `num_buckets` entries starting at `base`.
  // If `init_mutexes`, runs shm_mutex_init on each slot mutex (exactly one
  // process per cluster should do this during region init).
  void attach(void *base, uint32_t num_buckets, bool init_mutexes);

  static size_t bytes_for(uint32_t num_buckets);

  // Acquire / release the per-(bucket, slot) LFM mutex.
  // host_id is the caller's host index in [0, num_hosts); num_hosts is the
  // cluster size.  Required by the LFM algorithm so it knows which fields
  // of its per-host arrays to write/wait-on.
  // Returns contention count on success (>=0); UINT64_MAX on error.
  uint64_t lock_slot(uint32_t bucket_idx, int slot_idx,
                     int host_id, int num_hosts);
  void     unlock_slot(uint32_t bucket_idx, int slot_idx, int host_id);

  CxlFuseeSlotLockEntry *entry(uint32_t bucket_idx) const;
  uint32_t num_buckets() const { return num_buckets_; }
  bool     is_valid()    const { return entries_ != nullptr; }

 private:
  CxlFuseeSlotLockEntry *entries_     = nullptr;
  uint32_t               num_buckets_ = 0;
};

}  // namespace fusee

#endif  // FUSEE_CXL_FUSEE_SLOT_LOCK_H_
