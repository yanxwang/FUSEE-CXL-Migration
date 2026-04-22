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

// Phase 4 (2026-04-22): ring capacity shrunk from 4096 → 256 because each
// (src_gid, dst_gid) pair now has at most one outstanding op at a time
// under normal steady-state operation (writer waits for ACK before next op).
// kMaxWorkers sized for 2 hosts × 86 clients = 172 + margin; keep symmetric
// with cxl_shm_profiling's MAX_HOST_NUM (200) so LFM id and ring index can
// share the same numbering space.
constexpr int kPendingRingEntries = 256;
constexpr int kMaxWorkers = 200;
// Legacy name kept for existing code paths that use cross-host id to index
// the ring. New code should use kMaxWorkers.
constexpr int kMaxHosts = kMaxWorkers;

struct PendingRingEntry {
  cacheline_u64 op_id;            // 0 = free; writer publishes this last.
  cacheline_u64 bucket_idx;
  cacheline_u64 slot_idx;
  cacheline_u64 new_value_hi;
  cacheline_u64 new_value_lo;
  cacheline_u64 processed_op_id;  // replicator writes op_id here when done.
};

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
