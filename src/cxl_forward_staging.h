#ifndef FUSEE_CXL_FORWARD_STAGING_H_
#define FUSEE_CXL_FORWARD_STAGING_H_

// Protocol A v2 — write-path staging arena (Phase 2.B).
//
// The C2 constraint (no value bytes on the message ring) means the
// WriteEntry control message can only carry a *pointer* to the value
// bytes; the bytes themselves live in a CXL staging arena that the
// forwarder writes once and the owner LD-CXL-reads once.
//
// iter-9A original used the 1088 B inline payload as a "bridge" —
// that bridge is what this file replaces.
//
// Lifetime model is intentionally trivial:
//
//   - Each (src_host, dst_host) pair has kWriteRingDepth fixed-size
//     staging slots, mirroring WriteRing[src][dst].entries 1:1 by
//     index. WriteRing slot_idx → ForwardStaging slot_idx (same idx).
//   - Allocation = the ring's natural slot allocation
//     (fetch_add(tail) % depth + spin until req_op_id == 0). No
//     separate alloc/free path is needed — when the ring slot is
//     free, the staging slot is free.
//   - Free = automatic when forwarder bumps past the slot on next
//     fetch_add; circular reuse is identical to ring depth.
//
// This is WAY simpler than a generic arena allocator and has the
// nice property that the staging arena cannot leak.
//
// Per-host CXL footprint (defaults: kMaxHosts=4, kRingDepth=256,
// kSlotBytes=1024):
//
//   ForwardStagingMatrix = 4 * 4 * 256 * 1024  =  4 MiB total in CXL
//
// Forwarder (src) on slot_idx:
//   1. memcpy value bytes into fsm->slots[src][dst][slot_idx].bytes
//   2. flush_region the value_len bytes; sfence
//   3. enqueue WriteEntry { staging_off=slot_idx, value_len, ... }
//   4. spin on resp_op_id
//
// Owner (dst) on slot_idx:
//   1. drain WriteEntry; let src = (req_op_id >> 56) - 1
//   2. flush_region fsm->slots[src][me][slot_idx].bytes (value_len)
//   3. mfence
//   4. memcpy bytes into owner's pool block
//   5. ACK via resp_op_id (frees both ring slot and staging slot)
//
// staging_off is intentionally kept as a slot index (not a byte
// offset) in WriteEntry — it's a sanity check + room for future
// per-host arena rebasing. Today it should equal the WriteRing
// slot_idx; receiver asserts this.

#include <cstddef>
#include <cstdint>

#include "cxl_write_ring.h"

namespace fusee {

constexpr int kForwardStagingMaxHosts = kWriteMaxHosts;
constexpr int kForwardStagingDepth    = kWriteRingDepth;
// iter-16A V-sweep: configurable so V-matched builds use staging slot
// sized to actual KV value (saves memory + matches CXL line footprint).
// Default 1024 keeps V=1024 canonical build unchanged.
#ifndef FUSEE_FWD_STAGING_SLOT_BYTES
#define FUSEE_FWD_STAGING_SLOT_BYTES 1024
#endif
constexpr uint32_t kForwardStagingSlotBytes = FUSEE_FWD_STAGING_SLOT_BYTES;

// iter-16A V-sweep: when slot_bytes >= 64, keep alignas(64) for false-sharing
// prevention; when slot_bytes < 64 (e.g., V=8), drop alignas so static_assert
// "sizeof == slot_bytes" still holds (false-sharing acceptable for decomp
// study at small V — controlled experiment, not perf-tuned).
#if FUSEE_FWD_STAGING_SLOT_BYTES >= 64
struct alignas(64) ForwardStagingSlot {
  uint8_t bytes[kForwardStagingSlotBytes];
};
#else
struct ForwardStagingSlot {
  uint8_t bytes[kForwardStagingSlotBytes];
};
#endif
static_assert(sizeof(ForwardStagingSlot) == kForwardStagingSlotBytes,
              "ForwardStagingSlot must be exactly slot_bytes (no header)");

struct ForwardStagingMatrix {
  // slots[src_host][dst_host][slot_idx] = the value-bytes payload for
  // WriteRing[src_host][dst_host].entries[slot_idx].
  ForwardStagingSlot slots[kForwardStagingMaxHosts]
                          [kForwardStagingMaxHosts]
                          [kForwardStagingDepth];
};

inline std::size_t forward_staging_matrix_bytes() {
  return sizeof(ForwardStagingMatrix);
}

inline uint8_t *forward_staging_bytes(ForwardStagingMatrix *fsm,
                                       int src_host, int dst_host,
                                       int slot_idx) {
  return fsm->slots[src_host][dst_host][slot_idx].bytes;
}

}  // namespace fusee

#endif  // FUSEE_CXL_FORWARD_STAGING_H_
