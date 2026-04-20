#ifndef FUSEE_CXL_BUCKET_LOCK_H_
#define FUSEE_CXL_BUCKET_LOCK_H_

#include <stddef.h>
#include <stdint.h>

// Pull in LFM mutex from cxl_shm_profiling. CMake wires the include path via
// CXL_SHM_PROFILING_DIR.
extern "C" {
#include "locks/lfm_lock.h"
}

namespace fusee {

// One lock-table entry per RACE-hash bucket. Cacheline-aligned.
//
// Protocol-specific fields are appended to this struct as phases land.
// Phase 2: mutex only.
// Phase 3 (Option C): write_epoch — incremented under the lock on every
//                     mutating op; readers use seqlock-style retry.
// Phase 7 (Option B): staging_scratch — scratch cacheline for eager-push
//                     staging before the replicator consumes it.
// Option A uses its own SPSC rings in a separate sub-region (see
// docs/option_a_side_track.md); those do not live here.
struct BucketLockEntry {
  shm_mutex_t   mutex;
  cacheline_u64 write_epoch;
  cacheline_u64 staging_scratch;
};

// Thin view over a contiguous array of BucketLockEntry planted in a CXL
// region. The table does not own the underlying memory -- callers pass in a
// mmap'd base (typically a CXLRegion produced by cxl_mm).
class BucketLockTable {
 public:
  BucketLockTable() = default;

  // Place a table of `num_buckets` entries starting at `base`. Must be called
  // before any lock/unlock. If `init_mutexes` is true, runs shm_mutex_init on
  // each entry (exactly one process in the cluster should do this during
  // region setup -- typically host 0).
  void attach(void *base, uint32_t num_buckets, bool init_mutexes);

  // Byte footprint of a table for `num_buckets` buckets.
  static size_t bytes_for(uint32_t num_buckets);

  // Per-bucket operations. id = caller's host index in [0, num_hosts).
  // Returns contention count on success, UINT64_MAX on error.
  uint64_t lock(uint32_t bucket_idx, int host_id, int num_hosts);
  void     unlock(uint32_t bucket_idx, int host_id);

  BucketLockEntry *entry(uint32_t bucket_idx) const;
  uint32_t num_buckets() const { return num_buckets_; }
  bool     is_valid() const { return entries_ != nullptr; }

 private:
  BucketLockEntry *entries_ = nullptr;
  uint32_t num_buckets_ = 0;
};

} // namespace fusee

#endif // FUSEE_CXL_BUCKET_LOCK_H_
