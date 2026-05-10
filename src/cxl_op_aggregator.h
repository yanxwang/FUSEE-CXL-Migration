#ifndef FUSEE_CXL_OP_AGGREGATOR_H_
#define FUSEE_CXL_OP_AGGREGATOR_H_

// Protocol A v2 — per-worker DRAM aggregator slots + 3 sender threads
// (Phase 2.C of task_plan_iter9A.md).
//
// Without this layer, every worker thread does its own
// CXL atomic fetch_add(ring->tail) directly. At T=64 workers per host,
// that's 64-way contention on a single CXL atomic — measured 1.4 µs
// uncontested but degrading sharply under contention (iter-8A µbench
// shows ~25 ms tail at T=64 same-host atomic hammering). The
// aggregator breaks the contention: each worker has its own DRAM slot
// per ring kind that only IT writes; one sender thread per ring
// (Write/Read/Inval) drains all worker slots and is the SOLE producer
// on the CXL ring's tail.
//
// Each Protocol A worker is single-threaded and synchronous (issues
// one op, waits for completion before the next), so a single in-flight
// op per (worker, ring_kind) is sufficient — no ring/queue per worker,
// just a slot. This keeps the layer small and reasoning trivial.
//
// Memory layout (per host, MAP_SHARED|MAP_ANONYMOUS pre-fork):
//
//   AggregatorRegion {
//     slots      [kAggrRingKinds][kMaxAggrWorkers] — control + state
//     value_bufs [kAggrRingKinds][kMaxAggrWorkers] — value bytes (1 KiB each)
//   }
//
// ring_kind: 0 = Write, 1 = Read, 2 = Inval (matches the 3 sender threads).
//
// Per-host footprint (kMaxAggrWorkers=64, kAggrSlotBytes=1024):
//   slots:      3 × 64 × 64       =      12 KiB
//   value_bufs: 3 × 64 × 1024     =     192 KiB
//   total:                        ≈     204 KiB DRAM (no CXL).

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "cxl_forward_staging.h"
#include "cxl_inval_ring.h"
#include "cxl_read_ring.h"
#include "cxl_write_ring.h"

namespace fusee {

constexpr int kAggrRingKinds      = 3;     // Write=0, Read=1, Inval=2
constexpr int kAggrRingKindWrite  = 0;
constexpr int kAggrRingKindRead   = 1;
constexpr int kAggrRingKindInval  = 2;

constexpr int kMaxAggrWorkers     = 64;    // matches T_max grid

// Slot state machine:
//   kAggrEmpty   - slot is free, worker can write
//   kAggrPending - worker filled fields; awaiting sender to forward
//   kAggrDone    - sender has ACK from receiver and stored result_status
//
// Worker flow:
//   1. wait until state == kAggrEmpty (initially zero so first call is fine)
//   2. memcpy value bytes into value_bufs[k][w]
//   3. fill slot fields (key, op_kind, value_len, dst_host, worker_op_id)
//   4. state.store(kAggrPending, release)
//   5. spin: while state.load(acquire) != kAggrDone
//   6. read result_status; state.store(kAggrEmpty, release)
//
// Sender flow (per ring kind k):
//   1. poll slots[k][0..T-1] for state == kAggrPending
//   2. for each pending: do CXL forward (ring tail fetch_add, staging
//      memcpy, write entry, spin on resp_op_id)
//   3. on resp received: result_status = status; state.store(kAggrDone, release)
constexpr uint64_t kAggrEmpty   = 0;
constexpr uint64_t kAggrPending = 1;
constexpr uint64_t kAggrDone    = 2;

struct alignas(64) AggrSlot {
  // Worker writes (in this order):
  uint64_t key;                  //  0
  uint8_t  op_kind;              //  8
  uint8_t  _pad_a[3];            //  9
  uint32_t value_len;            // 12
  uint32_t dst_host;             // 16
  uint32_t _rsv;                 // 20
  uint64_t worker_op_id;         // 24
  // Sender writes:
  int32_t  result_status;        // 32
  uint32_t _pad_c;               // 36
  // Shared state (workers/sender both touch):
  std::atomic<uint64_t> state;   // 40, 8B
  uint64_t _pad_d[2];            // 48..63
};
static_assert(sizeof(AggrSlot) == 64,
              "AggrSlot must be exactly one cacheline");

constexpr uint32_t kAggrSlotValueBytes = kForwardStagingSlotBytes;

struct AggregatorRegion {
  AggrSlot slots[kAggrRingKinds][kMaxAggrWorkers];
  uint8_t  value_bufs[kAggrRingKinds][kMaxAggrWorkers][kAggrSlotValueBytes];
};

inline std::size_t aggregator_region_bytes() {
  return sizeof(AggregatorRegion);
}

inline AggrSlot *aggregator_slot(AggregatorRegion *r, int kind, int worker) {
  return &r->slots[kind][worker];
}

inline uint8_t *aggregator_value_buf(AggregatorRegion *r, int kind,
                                     int worker) {
  return r->value_bufs[kind][worker];
}

}  // namespace fusee

#endif  // FUSEE_CXL_OP_AGGREGATOR_H_
