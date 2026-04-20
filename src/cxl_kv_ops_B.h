#ifndef FUSEE_CXL_KV_OPS_B_H_
#define FUSEE_CXL_KV_OPS_B_H_

// Option B: eager push / fire-and-forget replication.
//
// Writer holds per-bucket lock, writes the slot, enqueues to each other
// host's pending ring (same matrix layout as Option A), then releases the
// lock without waiting for any ACK. The replicator on each dst consumes
// the ring, performs a simulated apply, and clears op_id so the slot can
// be reused.
//
// Relative to Option A, B drops the ACK-wait phase in the writer. Relative
// to Option C, B adds the push so readers that cache bucket contents
// locally can be invalidated eagerly (the cache itself is future work —
// for now readers still do seqlock-style re-reads).

#include "cxl_bucket_lock.h"
#include "cxl_hashtable.h"
#include "cxl_pending_ring.h"

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <thread>

namespace fusee {

class CxlKvStoreB {
 public:
  int attach(void *region_base, size_t region_bytes, uint32_t num_buckets,
             int host_id, int num_hosts, bool init_region);
  void stop();

  static size_t bytes_for(uint32_t num_buckets);

  int insert(uint64_t key, uint64_t value);
  int update(uint64_t key, uint64_t value);
  int remove(uint64_t key);
  int search(uint64_t key, uint64_t *out) const;

  uint32_t num_buckets() const { return num_buckets_; }
  uint64_t replicated_ops() const {
    return replicated_ops_.load(std::memory_order_relaxed);
  }

 private:
  uint32_t bucket_idx(uint64_t key) const {
    return static_cast<uint32_t>(fnv1a_u64(key) % num_buckets_);
  }
  int dispatch_nowait(uint32_t b_idx, uint32_t s_idx, uint64_t value_word);
  void replicator_loop();

  uint32_t num_buckets_ = 0;
  int      host_id_ = -1;
  int      num_hosts_ = 0;
  BucketLockTable    lock_table_;
  CxlKvBucket       *buckets_ = nullptr;
  PendingRingMatrix *rings_ = nullptr;
  uint64_t local_tail_[kMaxHosts] = {0, 0, 0, 0};

  std::thread replicator_;
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> replicated_ops_{0};
};

} // namespace fusee

#endif // FUSEE_CXL_KV_OPS_B_H_
