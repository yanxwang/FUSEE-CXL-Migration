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
#include "cxl_oplog.h"
#include "cxl_pending_ring.h"
#include "cxl_same_host_queue.h"

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <thread>
#include <vector>

namespace fusee {

// 2e: batch push for B. Writer appends per-dst up to K entries before
// pushing the batch to the CXL ring. Toggled by FUSEE_B_BATCH_K (default
// 1 = no batching) and FUSEE_B_BATCH_TIMEOUT_US (default 10). Only the
// cross-host path is batched; same-host DRAM remains immediate.
constexpr int kBBatchMax = 256;

struct BBatchEntry {
  uint32_t bucket_idx;
  uint32_t slot_idx;
  uint64_t new_value;
  uint64_t op_id;
};

struct BBatchBuf {
  uint64_t    count = 0;
  uint64_t    first_ns = 0;
  BBatchEntry items[kBBatchMax];
};

class CxlKvStoreB {
 public:
  int attach(void *region_base, size_t region_bytes, uint32_t num_buckets,
             int host_id, int num_hosts, bool init_region,
             bool read_only = false);

  // See cxl_kv_ops_A.h for semantics. Same-host peers go through DRAM
  // queues instead of CXL rings; cross-host still uses CXL.
  void enable_same_host_bypass(DramInvalMatrix *mat, int num_clients_per_host);

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
  void enable_oplog(OpLog *log) { oplog_ = log; }

  // Replay every InProgress entry in the attached OpLog. During recovery the
  // ring push is skipped; peer DRAM caches will refresh via seqlock retry
  // next time they see a mismatched write_epoch.
  uint64_t recover_from_oplog();

  // Turn on the DRAM bucket cache. When on, Option B's ring-based push is
  // what it always should have been: readers serve from DRAM and only see
  // an invalidation when a peer's writer actually pushed one (detected by
  // this hosts replicator setting the invalid flag). No CXL load on the
  // read fast path.
  void enable_dram_cache(bool on);

 private:
  uint32_t bucket_idx(uint64_t key) const {
    return static_cast<uint32_t>(fnv1a_u64(key) % num_buckets_);
  }
  int dispatch_nowait(uint32_t b_idx, uint32_t s_idx, uint64_t value_word);
  // 2e: flush the batch buffer for a specific dst or all of them.
  int flush_cxl_batch(int dst);
  void flush_all_cxl_batches();
  void replicator_loop();

  uint32_t num_buckets_ = 0;
  int      host_id_ = -1;
  int      num_hosts_ = 0;
  BucketLockTable    lock_table_;
  CxlKvBucket       *buckets_ = nullptr;
  PendingRingMatrix *rings_ = nullptr;
  uint64_t local_tail_[kMaxHosts] = {};

  std::thread replicator_;
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> replicated_ops_{0};

  OpLog *oplog_ = nullptr;

  // DRAM cache. cache_epoch_[idx] is a per-bucket atomic; UINT64_MAX means
  // invalid (must refresh). Replicator sets it to UINT64_MAX on invalidation.
  // Reader checks it on the fast path without touching CXL.
  bool cache_enabled_ = false;
  mutable std::vector<CxlKvBucket> cache_buckets_;
  mutable std::vector<std::atomic<uint64_t>> cache_epoch_;

  bool recovery_mode_ = false;
  bool read_only_ = false;

  // Same-host DRAM bypass (2a).
  DramInvalMatrix *dram_mat_ = nullptr;
  int num_clients_per_host_ = 0;   // 0 = bypass disabled
  int my_cid_in_host_ = 0;
  int my_host_ = 0;
  int physical_hosts_ = 1;
  uint64_t dram_local_tail_[kSameHostMaxClients] = {};

  // 2e: per-(host_id_, dst) batch buffer. Allocated lazily on attach to
  // keep ops path cache-friendly even when batching is disabled.
  int       batch_k_ = 1;            // 1 = no batching
  uint64_t  batch_timeout_ns_ = 10000;
  std::vector<BBatchBuf> cxl_batches_;
};

} // namespace fusee

#endif // FUSEE_CXL_KV_OPS_B_H_
