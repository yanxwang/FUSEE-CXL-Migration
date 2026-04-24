#ifndef FUSEE_CXL_KV_OPS_C_H_
#define FUSEE_CXL_KV_OPS_C_H_

// Option C (Lazy Release Consistency) KV operations on a CXL-resident hash
// table. Writers hold a per-bucket LFM lock, mutate the bucket, then bump a
// per-bucket write_epoch. Readers do not take the lock; they use seqlock-
// style retry (read epoch → read slots → re-read epoch; retry on mismatch).
//
// Phase 3 scope: fixed u64/u64 KV, single subtable (no directory splits).
// Extended layouts land in later phases.

#include "cxl_batch_ring.h"
#include "cxl_bucket_lock.h"
#include "cxl_hashtable.h"
#include "cxl_oplog.h"

#include <atomic>
#include <cstdint>
#include <stdint.h>
#include <thread>
#include <vector>

namespace fusee {

class CxlKvStoreC {
 public:
  // Carve a CXL region into header + bucket-lock table + bucket array, and
  // either initialize everything (init_region == true, exactly one host per
  // region should do this) or just attach.
  //
  // `region_base` must point to a CXL-shared mmap. `region_bytes` is the
  // usable size; if too small for `num_buckets`, returns -1.
  // `read_only` is accepted for API symmetry with A/B (where it skips the
  // replicator thread). C has no replicator, so the flag is ignored.
  int attach(void *region_base, size_t region_bytes, uint32_t num_buckets,
             int host_id, int num_hosts, bool init_region,
             bool read_only = false);

  // Returns the byte footprint required for a store of `num_buckets`.
  static size_t bytes_for(uint32_t num_buckets);

  // 0 on success, -1 on full bucket, -2 on key exists.
  int insert(uint64_t key, uint64_t value);
  // 0 on success, -1 on not found.
  int update(uint64_t key, uint64_t value);
  // 0 on success, -1 on not found.
  int remove(uint64_t key);
  // 0 on found (writes *out), -1 on not found.
  int search(uint64_t key, uint64_t *out) const;

  uint32_t num_buckets() const { return num_buckets_; }

  // API parity with CxlKvStoreA / CxlKvStoreB. C has no replicator thread
  // on the write path, but the Phase-3 batching flusher lives here: stop()
  // signals it, drains residual ring, and joins.
  void     stop() { stop_flusher(); }
  uint64_t replicated_ops() const { return 0; }

 private:
  uint32_t bucket_idx(uint64_t key) const {
    return static_cast<uint32_t>(fnv1a_u64(key) % num_buckets_);
  }

  uint32_t num_buckets_ = 0;
  int      host_id_ = -1;
  int      num_hosts_ = 0;
#if defined(FUSEE_PER_SLOT_LOCK) && FUSEE_PER_SLOT_LOCK
  SlotLockTable lock_table_;
#else
  BucketLockTable lock_table_;
#endif
  CxlKvBucket    *buckets_ = nullptr;
  OpLog          *oplog_ = nullptr;  // optional; set via enable_oplog()

  // Per-host DRAM bucket cache. Readers check cached_epoch_ against the CXL
  // bucket's write_epoch; on match, slots are served from DRAM without
  // flushing CXL cachelines. On miss, the bucket is re-fetched from CXL and
  // cached_epoch_ is advanced. Writers invalidate their own cache entry
  // after mutating CXL. cache_epoch_[i] == UINT64_MAX means "no valid
  // cached copy yet".
  mutable std::vector<CxlKvBucket> cache_buckets_;
  mutable std::vector<uint64_t>    cache_epoch_;
  bool     cache_enabled_ = false;

 public:
  // Toggle the DRAM cache on/off. Off by default so existing tests behave
  // identically. Enable when you want to see the cached read path in
  // benchmarks or when integrating into FUSEE proper.
  void enable_dram_cache(bool on);

 public:
  // Point at an external OpLog region (CXL-resident). begin/commit around
  // every mutating op once set. Not required; pre-existing tests that do not
  // care about recovery just leave it null.
  void enable_oplog(OpLog *log) { oplog_ = log; }

  // Walk the attached OpLog and replay every InProgress entry into this
  // store (calls insert/update/remove as appropriate). Returns the number
  // of entries acted on (0 if no log is attached). Idempotent: re-applying
  // an already-applied op is treated as success.
  uint64_t recover_from_oplog();

 public:
  // -------- Phase-3 UPDATE micro-batching (opt-in, off by default) --------
  //
  // Attach a host-local MAP_SHARED|MAP_ANONYMOUS ring for this store. Ring
  // body is NOT on CXL (peer host never reads it). `init_region` true on
  // exactly one process per host (the primary client before fork); others
  // spin until primary publishes init_done.
  //
  // After enable_batching(), UPDATE fast-path routes through the ring:
  // atomic fetch_add cursor -> write entry -> unlock (no slot lock taken).
  // Flusher thread (started by primary via start_flusher()) drains the ring
  // in cursor order into materialised slots + one bump_epoch per drain.
  int enable_batching(void *shm_base, std::size_t shm_bytes, uint32_t K,
                      uint32_t T_flush_us, bool init_region);

  // Primary-only: start / stop the flusher thread. On stop(), the
  // flusher drains any residual ring entries before joining.
  void start_flusher();
  void stop_flusher();

  bool batching_enabled() const { return batch_enabled_; }
  MicroBatchRing *batch_ring() { return &batch_ring_; }
  uint64_t ring_full_waits() const { return ring_full_waits_; }

 private:
  void flusher_loop();
  void drain_bucket(uint32_t bucket_idx);

  bool batch_enabled_ = false;
  MicroBatchRing batch_ring_;
  std::thread flusher_thread_;
  std::atomic<bool> flusher_started_{false};
  // Per-client ring-full wait counter (thread-local-ish, accumulated across
  // calls in this process).
  uint64_t ring_full_waits_ = 0;
};

} // namespace fusee

#endif // FUSEE_CXL_KV_OPS_C_H_
