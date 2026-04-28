#ifndef FUSEE_CXL_A_LOCAL_AGGREGATOR_H_
#define FUSEE_CXL_A_LOCAL_AGGREGATOR_H_

// iter-2A-revised — per-host MPSC aggregator (DRAM) + worker ACK buffer.
//
// All N writer threads on a host enqueue here; exactly one sender
// thread (per host) drains, batches K entries, and ships them
// across CXL via PerHostSpscRing. After the receiver acks, the sender
// flips the writer's worker_ack_buf slot to ACK_DONE so the writer
// (which has been spinning on its slot) can return.
//
// Living in DRAM (sysv shm or pre-fork mmap MAP_SHARED|MAP_ANONYMOUS)
// because every client process must enqueue here. Single sender per
// host (process fork inheritance + sysv attach) reads. Cache coherence
// is x86 default — no clflushopt needed for inter-host visibility.

#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace fusee {

// Per-host bound. 256 in-flight ops cover N=86 workers each having a
// few outstanding without producer backpressure.
constexpr int kAggrQueueDepth = 256;
// Hard upper bound on workers per host. Worker_ack_buf is sized by
// it; same as kSameHostMaxClients (== 86 in practice).
constexpr int kAggrMaxWorkers = 128;

// One queued cross-host invalidation request from a writer.
//   bucket_idx: which CXL bucket the destination host should refresh
//   new_epoch: the post-write epoch the receiver should publish
//   dst_host: physical host id of the destination
//   src_worker_slot: 0..kAggrMaxWorkers-1 - the sender uses this to
//              flip worker_ack_buf[src_worker_slot] once the receiver
//              acks this entry
//   op_id: writer-supplied unique id; 0 = slot free (sentinel for the
//          ring's MPSC tail/head bookkeeping). Written LAST by the
//          producer; the consumer waits on it.
struct alignas(64) AggrEntry {
  uint64_t bucket_idx;
  uint64_t new_epoch;
  uint32_t dst_host;
  uint32_t src_worker_slot;
  uint64_t op_id;          // 0 = free; written last (release)
  uint64_t _pad[4];
};
static_assert(sizeof(AggrEntry) == 64,
              "AggrEntry must occupy one full 64-B cacheline");

// Ring header. Tail (atomic, multi-producer fetch_add) on its own
// cacheline; head (single-consumer plain) on its own cacheline.
struct alignas(64) LocalAggregatorQueueHeader {
  std::atomic<uint64_t> tail;     // multi-producer fetch_add
  char _pad_tail[64 - sizeof(std::atomic<uint64_t>)];
  uint64_t head;                  // single-consumer plain
  char _pad_head[64 - sizeof(uint64_t)];
  std::atomic<uint64_t> stop;     // sender thread shutdown signal
  char _pad_stop[64 - sizeof(std::atomic<uint64_t>)];
  std::atomic<uint64_t> init_done;
  char _pad_init[64 - sizeof(std::atomic<uint64_t>)];
};

struct LocalAggregatorQueue {
  LocalAggregatorQueueHeader hdr;
  AggrEntry entries[kAggrQueueDepth];
};

// Worker ACK buffer slot. Sender writes ACK_DONE (the matched op_id)
// once the cross-host receiver acks; the worker spins on its slot.
// Each writer owns one slot indexed by its src_worker_slot.
struct alignas(64) WorkerAckSlot {
  std::atomic<uint64_t> ack_op_id;  // matches the worker's last issued op_id when done
  char _pad[64 - sizeof(std::atomic<uint64_t>)];
};

struct WorkerAckBuf {
  WorkerAckSlot slots[kAggrMaxWorkers];
};

// iter-3A Phase 4: K-channel sharding. The runner allocates K queues +
// K ack buffers per host (pre-fork mmap). The writer hashes
// `bucket_id % K` to pick a channel. Each channel has its own sender
// thread (drains queues[k]) + receiver counterpart on the peer host.
// Memory is sized for kMaxKChannelsAggr always; only [0..K-1] used.
constexpr int kMaxKChannelsAggr = 4;

// Per-host wrapper. K queues + K worker_ack_bufs. K=1 reduces to
// iter-2A-revised behavior (only [0] used). Code that doesn't yet
// understand K addresses .queues[0] / .ack_bufs[0] explicitly.
struct LocalAggregatorRegion {
  LocalAggregatorQueue queues[kMaxKChannelsAggr];
  WorkerAckBuf         ack_bufs[kMaxKChannelsAggr];
};

inline size_t local_aggregator_region_bytes() {
  return sizeof(LocalAggregatorRegion);
}

// Producer-side enqueue. Spins on slot-free up to a 5 ms wall clock
// budget; returns 0 on success, -1 on timeout (writer must retry).
// Strict A semantics require option-1 backpressure (do not drop): a
// dropped invalidation breaks linearizability.
//   q: aggregator queue (in DRAM)
//   bucket_idx, new_epoch, dst_host, src_worker_slot, op_id: payload
// Out parameter: my_pos is the position the entry was written at; not
// needed by the worker — the worker spins on the ack_buf slot.
int aggr_enqueue(LocalAggregatorQueue *q, uint64_t bucket_idx,
                 uint64_t new_epoch, uint32_t dst_host,
                 uint32_t src_worker_slot, uint64_t op_id);

// Initializer (zero region + publish init_done). Called by the host's
// primary client process before fork. Other clients spin on init_done
// during their attach.
int aggr_region_init(LocalAggregatorRegion *r);

// Spin until init_done is published.
int aggr_region_attach(LocalAggregatorRegion *r);

} // namespace fusee

#endif // FUSEE_CXL_A_LOCAL_AGGREGATOR_H_
