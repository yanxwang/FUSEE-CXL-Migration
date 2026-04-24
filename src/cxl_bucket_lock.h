#ifndef FUSEE_CXL_BUCKET_LOCK_H_
#define FUSEE_CXL_BUCKET_LOCK_H_

#include <stddef.h>
#include <stdint.h>

// Pull in LFM mutex from cxl_shm_profiling. CMake wires the include path via
// CXL_SHM_PROFILING_DIR. Phase 2 adds ticket_lock as an optional swap-in
// controlled by FUSEE_USE_TICKET_LOCK (build-time flag). Keeping both
// headers included so diagnostic code and tests can reference either kind.
extern "C" {
#include "locks/lfm_lock.h"
#include "locks/ticket_lock.h"
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
// Mutex type choice. -DFUSEE_USE_TICKET_LOCK=1 at build time swaps to
// ticket_mutex_t for O(N) bounded worst-case wait (Phase 2). Default keeps
// LFM for backward compatibility with small-N microbenches.
#if defined(FUSEE_USE_TICKET_LOCK) && FUSEE_USE_TICKET_LOCK
using bucket_mutex_t = ticket_mutex_t;
#else
using bucket_mutex_t = shm_mutex_t;
#endif

struct BucketLockEntry {
  bucket_mutex_t mutex;
  cacheline_u64  write_epoch;
  cacheline_u64  staging_scratch;
};

// Phase-2b: per-slot lock variant. The same per-bucket write_epoch is kept
// for reader seqlock compatibility, but the writer side serializes only
// against other writers that target the SAME slot. The slot mutex type is
// the same `bucket_mutex_t` typedef used for the per-bucket case (LFM by
// default, ticket_mutex with -DFUSEE_USE_TICKET_LOCK=ON), so the only
// difference between the per-bucket and per-slot paths is granularity,
// not lock algorithm — useful for apples-to-apples comparison.
constexpr int kSlotMutexesPerBucket = 7;

struct SlotLockEntry {
  bucket_mutex_t slot_mutexes[kSlotMutexesPerBucket];
  cacheline_u64  write_epoch;
  cacheline_u64  staging_scratch;
  // Phase-2.5 route_seq. Bumped by INSERT/DELETE (which can move a key to
  // a different slot) and NOT by UPDATE (which leaves slot keys unchanged).
  // UPDATE readers use it to skip the under-lock re-verify when the bucket's
  // slot layout has not changed between the unlocked pre-scan and the
  // lock_slot() return. Cross-host publishing follows the same CACHELINE_STORE
  // pattern as write_epoch.
  cacheline_u64  route_seq;
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

// Phase-2b: per-slot lock table. Used by protocol C write paths when
// -DFUSEE_PER_SLOT_LOCK=ON. Supplies a dedicated ticket_mutex_t per
// (bucket, slot) pair plus a per-bucket write_epoch for reader seqlock
// compatibility. Readers still only need entry(idx)->write_epoch.
class SlotLockTable {
 public:
  SlotLockTable() = default;

  void attach(void *base, uint32_t num_buckets, bool init_mutexes);
  static size_t bytes_for(uint32_t num_buckets);

  // Acquire / release one slot's mutex. host_id/num_hosts unused (ticket
  // locks are self-ordering), kept for API parity with BucketLockTable.
  void lock_slot(uint32_t bucket_idx, int slot_idx);
  void unlock_slot(uint32_t bucket_idx, int slot_idx);

  SlotLockEntry *entry(uint32_t bucket_idx) const;
  uint32_t num_buckets() const { return num_buckets_; }
  bool     is_valid() const { return entries_ != nullptr; }

 private:
  SlotLockEntry *entries_ = nullptr;
  uint32_t num_buckets_ = 0;
};

} // namespace fusee

#endif // FUSEE_CXL_BUCKET_LOCK_H_
