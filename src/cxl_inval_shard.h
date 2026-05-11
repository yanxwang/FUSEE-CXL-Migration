#ifndef FUSEE_CXL_INVAL_SHARD_H_
#define FUSEE_CXL_INVAL_SHARD_H_

// Protocol A v2 — parallel inval-broadcast worker pool (iter-11A Phase 2).
//
// Replaces iter-10A's single-thread InvalReceiver per ring with:
//   - 1 InvalDispatcher thread (cpu 69) — polls InvalRing[*][me],
//     reads InvalEntry, dispatches to a per-shard worker queue.
//   - N=8 InvalWorker threads (cpu 70-77) — each owns one shard;
//     pops from queue, calls cache_pool_set_stale + bumps bucket
//     epoch, then writes resp_op_id back into the original
//     InvalEntry on the producer's CXL ring.
//
// Sharding key = bucket_id (= hash(key) % NUM_BUCKETS) → same
// bucket always handled by same worker → per-bucket FIFO preserved
// per task plan §C14. (Note: cache_pool_set_stale is idempotent
// per-key, so strict per-bucket FIFO is over-spec for correctness;
// we keep it for plan compliance + future-proofing if other inval
// op kinds are added.)
//
// Queue is DRAM-only (within one host's process), MPSC per shard:
//   - dispatcher = single producer
//   - worker = single consumer
// Capacity = kInvalShardQueueDepth. On overflow, dispatcher falls
// back to inline processing (calls set_stale itself + acks). This
// preserves liveness even if a worker is descheduled.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "cxl_inval_ring.h"

namespace fusee {

constexpr int kInvalShardCount = 8;
constexpr int kInvalShardQueueDepth = 1024;

// Per-entry: pointer to InvalEntry on CXL ring + src host (for ack
// flushing) + req_op_id (so worker doesn't need to re-read).
struct InvalShardJob {
  InvalEntry *entry;
  uint64_t key;
  uint64_t req_op_id;
  int src;
};

// MPSC ring (single producer = dispatcher; single consumer = worker).
// Power-of-two depth + atomic head/tail for lock-free push/pop.
struct alignas(64) InvalShardQueue {
  alignas(64) std::atomic<uint64_t> tail;  // producer cursor
  alignas(64) std::atomic<uint64_t> head;  // consumer cursor
  alignas(64) InvalShardJob jobs[kInvalShardQueueDepth];
};
static_assert((kInvalShardQueueDepth & (kInvalShardQueueDepth - 1)) == 0,
              "kInvalShardQueueDepth must be power of two");

inline bool inval_shard_push(InvalShardQueue *q, const InvalShardJob &job) {
  uint64_t t = q->tail.load(std::memory_order_relaxed);
  uint64_t h = q->head.load(std::memory_order_acquire);
  if (t - h >= (uint64_t)kInvalShardQueueDepth) return false;  // full
  q->jobs[t & (kInvalShardQueueDepth - 1)] = job;
  q->tail.store(t + 1, std::memory_order_release);
  return true;
}

inline bool inval_shard_pop(InvalShardQueue *q, InvalShardJob *out) {
  uint64_t h = q->head.load(std::memory_order_relaxed);
  uint64_t t = q->tail.load(std::memory_order_acquire);
  if (h == t) return false;  // empty
  *out = q->jobs[h & (kInvalShardQueueDepth - 1)];
  q->head.store(h + 1, std::memory_order_release);
  return true;
}

}  // namespace fusee

#endif  // FUSEE_CXL_INVAL_SHARD_H_
