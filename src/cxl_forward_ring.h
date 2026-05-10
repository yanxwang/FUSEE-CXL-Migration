#ifndef FUSEE_CXL_FORWARD_RING_H_
#define FUSEE_CXL_FORWARD_RING_H_

// Protocol A v2 — minimal cross-host write-forward ring (Phase 8).
//
// Spec: docs/design_goals.md §I11 (cross-host write via N:1:1:N
// forward, not LFM), Scenario 7 in §VI-B.
//
// Design simplifications vs iter-3A's full N:1:1:N infrastructure:
//   - One per-(src,dst) SPSC ring living in CXL.
//   - Single producer per (src,dst) per writer worker — guarded by
//     a host-local atomic fetch_add on tail to serialize multiple
//     writers on the same source host.
//   - Single consumer thread per host (the "forward responder"
//     thread) that polls all incoming rings, executes the requested
//     write_local, and writes the response back into a per-slot
//     response cacheline.
//   - Response is in-band: reuses the same SPSC slot's `status` byte
//     and `payload_cxl_off` field to carry success / value pointer.
//
// This is intentionally NOT the full directory-coherence protocol
// (no register/invalidate plumbing). For Phase 8 correctness test
// (cross-host hash-diff after writes complete and barrier sync),
// this is sufficient. iter-5A will replace this minimal ring with
// the full N:1:1:N + sender/receiver threading from iter-3A.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fusee {

constexpr int kForwardRingDepth = 256;
constexpr int kForwardMaxHosts  = 4;

// iter-9A Phase 1 (TEMPORARY): inline value-bytes payload up to
// kForwardEntryPayloadBytes. Phase 2 replaces this with separate
// ForwardStaging[H] arena + control-only WriteEntry/ReadEntry. Until
// then, each ring entry is 1 metadata cacheline + 1024B payload =
// 1088 bytes. Smallest power-of-2 ring depth fitting ≤ 1.5MB total
// for [4][4] matrix is 256.
constexpr int kForwardEntryPayloadBytes = 1024;

struct alignas(64) ForwardEntry {
  // Producer side fields (writer fills + flushes):
  std::atomic<uint64_t> req_op_id;   // 0 = empty; non-zero = pending request
  uint64_t key;
  uint32_t value_len;                // iter-9A: actual payload bytes
  uint8_t  op_kind;                  // 0=update, 1=insert, 2=delete, 4=cache_register
  uint8_t  _pad[3];
  uint64_t resp_value_len_;          // iter-9A: register response payload size
  // Consumer side fields (responder fills + flushes):
  std::atomic<uint64_t> resp_op_id;  // matches req_op_id when ready
  int32_t  status;                   // 0=ok; <0 error
  uint8_t  _pad2[20];
  // iter-9A Phase 1 inline payload — TEMPORARY, removed in Phase 2.
  uint8_t  payload[kForwardEntryPayloadBytes];
};
static_assert(sizeof(ForwardEntry) == 64 + kForwardEntryPayloadBytes,
              "ForwardEntry layout drift");

struct alignas(64) ForwardRing {
  std::atomic<uint64_t> tail;       // producer cursor (writer fetch_add)
  char _pad_tail[64 - sizeof(std::atomic<uint64_t>)];
  uint64_t head;                    // consumer cursor (responder plain)
  char _pad_head[64 - sizeof(uint64_t)];
  ForwardEntry entries[kForwardRingDepth];
};

// (src_host, dst_host) -> ring. Allocated in CXL region.
struct ForwardRingMatrix {
  ForwardRing rings[kForwardMaxHosts][kForwardMaxHosts];
};

inline std::size_t forward_ring_matrix_bytes() {
  return sizeof(ForwardRingMatrix);
}

}  // namespace fusee

#endif  // FUSEE_CXL_FORWARD_RING_H_
