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

#include "cxl_a_cache_epoch_arr.h"
#include "cxl_a_local_aggregator.h"
#include "cxl_bucket_lock.h"
#include "cxl_hashtable.h"
#include "cxl_oplog.h"
#include "cxl_pending_ring.h"
#include "cxl_per_host_ring.h"
#include "cxl_same_host_queue.h"

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <thread>
#include <vector>

namespace fusee {

class CxlKvStoreA {
 public:
  // Attach/init. Exactly one host per region must pass init_region=true.
  // Starts the replicator thread as a side effect on every host, unless
  // read_only is true — in which case no ring state is consumed, no
  // replicator is spawned, and any write operation (insert/update/remove)
  // will abort() at runtime. This lets pure-read clients scale intra-host
  // without needing per-client rings (Phase 4). See
  // docs/ABC_throughput_improvement_plan.md §Phase 1.
  int attach(void *region_base, size_t region_bytes, uint32_t num_buckets,
             int host_id, int num_hosts, bool init_region,
             bool read_only = false);

  // Phase-4 follow-up: enable same-host DRAM bypass for writes. Writer will
  // push invalidations to same-host peers via DRAM queues (O(1) work per
  // same-host peer, cache-coherent, no CXL fabric hit) instead of CXL
  // rings. Cross-host peers still use the CXL ring.
  //   `mat` = process-shared anonymous mmap of DramInvalMatrix, allocated
  //           and zeroed by the parent before fork.
  //   `num_clients_per_host` = clients count in one physical host
  //           (total_workers / physical_hosts).
  // Must be called after attach() and before any op. Safe to omit
  // (bypass off; all pushes go via CXL as before).
  void enable_same_host_bypass(DramInvalMatrix *mat, int num_clients_per_host);

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

  // Replay every InProgress entry in the attached OpLog into this store.
  // While recovery runs, ring dispatch (and the ACK wait) is short-circuited
  // so redo does not hang on peers that have not restarted yet. The
  // authoritative CXL slot write + epoch bump still happens, so peers that
  // come back later will see updated values via their next seqlock retry.
  // Returns number of entries acted on (0 if no log attached).
  uint64_t recover_from_oplog();

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

  // iter-1A Solution 1 (data structures only — opt-in via
  // FUSEE_PER_HOST_RING=1 env at attach). Producer-side enqueue and
  // consumer-side replicator dispatch are NOT wired in this iter; the
  // matrix attaches but stays unused unless a future iter rewires
  // dispatch_and_wait + replicator_loop. See
  // docs/iters/iter1A_baseline_summary_<date>.md §"Phase 5 status".
  PerHostOutMatrix *per_host_rings_ = nullptr;
  bool              per_host_rings_enabled_ = false;

  // iter-2A-revised: full N:1:1:N path (this becomes the active path
  // when per_host_rings_enabled_ is true). DRAM aggregator + per-host
  // sender thread + atomic_store-via-coherence cache_epoch_arr.
  LocalAggregatorRegion *aggregator_ = nullptr;   // DRAM, MAP_SHARED
  CacheEpochArr         *cache_epoch_arr_ = nullptr;  // DRAM, MAP_SHARED
  int phys_hosts_pr_       = 1;
  int my_phys_host_pr_     = 0;
  int clients_per_host_pr_ = 1;
  int my_cid_in_host_pr_   = 0;
  // Sender thread state. Spawned only on each host's primary client
  // (host_id within physical host == 0).
  std::thread sender_thread_;
  std::atomic<bool> sender_started_{false};
  uint32_t sender_batch_k_ = 4;
  uint64_t sender_batch_t_ns_ = 20000ULL;  // 20 µs
  int sender_core_ = -1;     // <0 = no pinning
  int receiver_core_ = -1;
 public:
  // iter-2A-revised wiring API. Call after attach() on every client;
  // primary client (cid_in_host == 0) attaches with init_region=true
  // for the aggregator + cache_epoch_arr regions and spawns the sender
  // thread. All clients pass the same `aggr_region` and
  // `cache_epoch_arr` pre-fork-mmap'd addresses.
  int enable_per_host_ring(LocalAggregatorRegion *aggr_region,
                           CacheEpochArr *epoch_arr,
                           uint32_t batch_k,
                           uint64_t batch_t_ns,
                           int sender_core,
                           int receiver_core);
  // Stop the sender thread (primary only). Idempotent.
  void stop_per_host_sender();
  // Lookup current cache_epoch (for diagnostics + reader path).
  uint64_t cache_epoch_load(uint32_t bucket_idx) const;

 private:
  void sender_loop();
  // Per-worker slot index for worker_ack_buf. Set in
  // enable_per_host_ring. <0 if not configured.
  int my_worker_slot_pr_ = -1;

  // Per-dst producer tail mirror: single-producer cursor lives on src side so
  // we do not need atomic-fetch-add on CXL.
  uint64_t local_tail_[kMaxHosts] = {};

  // Replicator thread state.
  std::thread replicator_;
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> replicated_ops_{0};
  std::atomic<uint64_t> ack_timeouts_[kMaxHosts] = {};

  OpLog *oplog_ = nullptr;

  bool cache_enabled_ = false;
  mutable std::vector<CxlKvBucket> cache_buckets_;
  mutable std::vector<std::atomic<uint64_t>> cache_epoch_;

  // When true, dispatch_and_wait skips the ring enqueue + ACK wait. Flipped
  // on by recover_from_oplog around the redo pass only.
  bool recovery_mode_ = false;

  // Phase 1: if true, this client did not spawn a replicator and must not
  // call any mutating op. Trip-wire set in attach(read_only=true).
  bool read_only_ = false;

  // Same-host DRAM bypass (2a).
  DramInvalMatrix *dram_mat_ = nullptr;
  int num_clients_per_host_ = 0;   // 0 = bypass disabled
  int my_cid_in_host_ = 0;
  int my_host_ = 0;
  int physical_hosts_ = 1;
  uint64_t dram_local_tail_[kSameHostMaxClients] = {};
};

} // namespace fusee

#endif // FUSEE_CXL_KV_OPS_A_H_
