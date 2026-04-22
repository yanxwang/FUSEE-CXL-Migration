#ifndef FUSEE_CXL_PENDING_RING_H_
#define FUSEE_CXL_PENDING_RING_H_

// SPSC ring used by Option A sync replication. One ring per (src, dst) host
// pair. The writer on src publishes op metadata; the replicator on dst
// consumes and marks processed. Design mirrors the A-v2 layout from
// cxl_shm_profiling/bench/ycsb_abc_bench.c — see
// docs/option_a_side_track.md §A-v2 for the rationale.

#include <stdint.h>

extern "C" {
#include "common.h"  // cacheline_u64, CACHELINE_STORE / CACHELINE_LOAD
}

namespace fusee {

// Phase 4 + ring-depth sweep: kPendingRingEntries is a build-time knob.
// Default 256; override with -DFUSEE_RING_ENTRIES=N in CMake to sweep
// {4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048} for the 2f experiment.
#ifdef FUSEE_RING_ENTRIES
constexpr int kPendingRingEntries = FUSEE_RING_ENTRIES;
#else
constexpr int kPendingRingEntries = 256;
#endif
constexpr int kMaxWorkers = 200;
// Legacy name kept for existing code paths that use cross-host id to index
// the ring. New code should use kMaxWorkers.
constexpr int kMaxHosts = kMaxWorkers;

// Packed layout (2026-04-22, Phase-4 followup):
//   Cacheline 0: producer payload (bucket_idx, slot_idx, new_value, op_id).
//   Cacheline 1: consumer ACK.
//
// Writer fills all producer fields, then flush_line(&prod) + sfence ONCE.
// Because a single cacheline store is atomic wrt to the CXL memory server
// (64 B transaction unit), the consumer either observes all four fields
// with the new op_id, or observes the previous cacheline state — never a
// torn publish. This replaces the earlier 6-cacheline layout where each
// field was CACHELINE_STORE'd separately with its own flush+sfence.
//
// The ACK cacheline is separate to avoid the producer/consumer
// false-sharing ping-pong that broke an earlier 1-cacheline packing
// attempt (see docs/option_a_side_track.md 2026-04-20 note).
struct alignas(64) PendingRingEntry {
  struct alignas(64) Payload {
    uint64_t bucket_idx;
    uint64_t slot_idx;
    uint64_t new_value;
    uint64_t _pad[4];
    uint64_t op_id;          // written last; 0 = free slot
  } prod;
  struct alignas(64) Ack {
    uint64_t processed_op_id;
    uint64_t _pad[7];
  } cons;
};
static_assert(sizeof(PendingRingEntry::Payload) == 64,
              "producer payload must fit one cacheline");
static_assert(sizeof(PendingRingEntry::Ack) == 64,
              "consumer ack must occupy its own cacheline");
static_assert(sizeof(PendingRingEntry) == 128,
              "PendingRingEntry is 2 cachelines packed");

struct PendingRing {
  cacheline_u64 tail;   // producer cursor (src)
  cacheline_u64 head;   // consumer cursor (dst)
  PendingRingEntry entries[kPendingRingEntries];
};

// [src][dst] lattice; allocate size-of() for the whole block inside the CXL
// region. Only rings where src != dst are ever used — but we keep the full
// square for addressing simplicity.
struct PendingRingMatrix {
  PendingRing rings[kMaxHosts][kMaxHosts];
};

inline size_t pending_ring_matrix_bytes() {
  return sizeof(PendingRingMatrix);
}

} // namespace fusee

#endif // FUSEE_CXL_PENDING_RING_H_
