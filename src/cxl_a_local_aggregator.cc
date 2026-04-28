#include "cxl_a_local_aggregator.h"

#include <cstring>
#include <ctime>
#include <sched.h>

namespace fusee {

namespace {
inline uint64_t now_ns_aggr() {
  timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
}  // namespace

int aggr_region_init(LocalAggregatorRegion *r) {
  if (!r) return -1;
  std::memset(r, 0, sizeof(*r));
  std::atomic_thread_fence(std::memory_order_release);
  // Publish init_done on every channel's header.
  for (int k = 0; k < kMaxKChannelsAggr; k++) {
    r->queues[k].hdr.init_done.store(1, std::memory_order_release);
  }
  return 0;
}

int aggr_region_attach(LocalAggregatorRegion *r) {
  if (!r) return -1;
  while (r->queues[0].hdr.init_done.load(std::memory_order_acquire) == 0) {
    sched_yield();
  }
  return 0;
}

int aggr_enqueue(LocalAggregatorQueue *q, uint64_t bucket_idx,
                 uint64_t new_epoch, uint32_t dst_host,
                 uint32_t src_worker_slot, uint64_t op_id) {
  if (!q || op_id == 0) return -1;

  uint64_t pos = q->hdr.tail.fetch_add(1, std::memory_order_acq_rel);
  AggrEntry *e = &q->entries[pos % kAggrQueueDepth];

  // Spin on slot-free with 5 ms timeout. Strict A forbids dropping the
  // invalidation; a -1 return tells the writer to retry under the
  // outer lock release/reacquire protocol.
  const uint64_t kBudgetNs = 5000000ULL;  // 5 ms
  uint64_t start = now_ns_aggr();
  while (e->op_id != 0) {
    if (now_ns_aggr() - start > kBudgetNs) return -1;
    __builtin_ia32_pause();
  }

  // Publish payload, then op_id last (release).
  e->bucket_idx = bucket_idx;
  e->new_epoch  = new_epoch;
  e->dst_host   = dst_host;
  e->src_worker_slot = src_worker_slot;
  std::atomic_thread_fence(std::memory_order_release);
  e->op_id = op_id;
  std::atomic_thread_fence(std::memory_order_release);
  return 0;
}

}  // namespace fusee
