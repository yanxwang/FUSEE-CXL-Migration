#ifndef FUSEE_CXL_KV_OPS_A_H_
#define FUSEE_CXL_KV_OPS_A_H_

// Option A: sync replication with ACK (A-v2 SPSC-ring design).
//
// Writer holds per-bucket lock, writes the slot, then enqueues an entry in
// every OTHER host's pending ring and spins waiting for each to mark
// processed. Reader follows the same seqlock pattern as Option C.
//
// Region layout (extends the Option C layout with pending rings at the end):
//   [0]                             : reserved header (4 KiB)
//   [4 KiB]                         : BucketLockEntry[num_buckets]
//   [aligned up]                    : CxlKvBucket[num_buckets]
//   [aligned up]                    : PendingRingMatrix
//
// Replicator thread on each host polls the (*, my_id) column of the matrix.
// For now the "pull" work is simulated by a CACHELINE_LOAD on the entry
// itself — there is no separate staging area; the authoritative bucket is
// already shared on CXL. This matches the mini-bench simulated-pull model.

#include "cxl_bucket_lock.h"
#include "cxl_hashtable.h"
#include "cxl_oplog.h"
#include "cxl_pending_ring.h"

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <thread>
#include <vector>

namespace fusee {

class CxlKvStoreA {
 public:
  // Attach/init. Exactly one host per region must pass init_region=true.
  // Starts the replicator thread as a side effect on every host.
  int attach(void *region_base, size_t region_bytes, uint32_t num_buckets,
             int host_id, int num_hosts, bool init_region);

  // Stops the replicator thread. Call before destroying the CXL region.
  void stop();

  static size_t bytes_for(uint32_t num_buckets);

  int insert(uint64_t key, uint64_t value);
  int update(uint64_t key, uint64_t value);
  int remove(uint64_t key);
  int search(uint64_t key, uint64_t *out) const;

  uint32_t num_buckets() const { return num_buckets_; }

  // Diagnostics.
  uint64_t replicated_ops() const {
    return replicated_ops_.load(std::memory_order_relaxed);
  }
  uint64_t ack_timeouts_to(int dst) const {
    return (dst >= 0 && dst < kMaxHosts)
               ? ack_timeouts_[dst].load(std::memory_order_relaxed)
               : 0;
  }

  // Optional OpLog integration for crash recovery. Same shape as CxlKvStoreC.
  void enable_oplog(OpLog *log) { oplog_ = log; }

  // DRAM cache with ring-driven invalidation. With A, the writer waits for
  // every replicator's ACK before returning — so after return, no host can
  // serve a stale cached read. Stronger than B's fire-and-forget variant.
  void enable_dram_cache(bool on);

 private:
  uint32_t bucket_idx(uint64_t key) const {
    return static_cast<uint32_t>(fnv1a_u64(key) % num_buckets_);
  }

  // Writer-side helper: enqueue an op to each dst != host_id and spin on ACK.
  int dispatch_and_wait(uint32_t b_idx, uint32_t s_idx, uint64_t value_word);

  void replicator_loop();

  uint32_t num_buckets_ = 0;
  int      host_id_ = -1;
  int      num_hosts_ = 0;
  BucketLockTable   lock_table_;
  CxlKvBucket      *buckets_ = nullptr;
  PendingRingMatrix *rings_ = nullptr;

  // Per-dst producer tail mirror: single-producer cursor lives on src side so
  // we do not need atomic-fetch-add on CXL.
  uint64_t local_tail_[kMaxHosts] = {0, 0, 0, 0};

  // Replicator thread state.
  std::thread replicator_;
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> replicated_ops_{0};
  std::atomic<uint64_t> ack_timeouts_[kMaxHosts] = {};

  OpLog *oplog_ = nullptr;

  bool cache_enabled_ = false;
  mutable std::vector<CxlKvBucket> cache_buckets_;
  mutable std::vector<std::atomic<uint64_t>> cache_epoch_;
};

} // namespace fusee

#endif // FUSEE_CXL_KV_OPS_A_H_
