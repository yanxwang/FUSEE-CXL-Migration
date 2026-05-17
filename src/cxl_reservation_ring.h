#ifndef FUSEE_CXL_RESERVATION_RING_H_
#define FUSEE_CXL_RESERVATION_RING_H_

// iter-13A Phase 2 W3: per-worker batched block-reservation request ring.
//
// Layout: ReservationRingMatrix.rings[req_host][owner_host].entries[depth]
//
// Each request reserves K block offsets from owner. Owner alloc's K blocks
// from its OWN segment (NOT from peer-reserved region — that's W1's job).
// Worker maintains per-(thread, owner) DRAM queue and pops one blk_off per
// cross-host write; refills via this ring when low water mark hit.
//
// Unlike WriteRing, reservation is BLOCKING — worker must wait for owner
// to respond before issuing writes. Amortization: K writes per round trip.
// Owner's reservation handler runs on a dedicated thread (or piggybacks
// on existing receiver thread; we use a dedicated one for simplicity).

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "cxl_read_ring.h"  // for kReadMaxHosts

namespace fusee {

constexpr int kReservRingDepth   = 64;     // shallow; few outstanding reservations
constexpr int kReservMaxHosts    = kReadMaxHosts;
constexpr int kReservMaxBatchK   = 4096;   // matches QR4 sweep upper bound

struct alignas(64) ReservationEntry {
  // Producer (peer) writes cacheline 0.
  std::atomic<uint64_t> req_op_id;       // 0 = empty
  uint32_t batch_k;                       // requested K (1..kReservMaxBatchK)
  uint32_t _pad_a;
  uint8_t _pad_b[64 - 8 - 4 - 4];
  // Owner writes cachelines 1..K_max with blk_offs, then publishes resp_op_id.
  std::atomic<uint64_t> resp_op_id;
  int32_t  status;                        // 0 = ok; <0 = exhausted (partial fill)
  uint32_t filled_count;                  // owner returns # blk_offs actually filled
  uint8_t _pad_c[64 - 8 - 4 - 4];
  uint64_t blk_offs[kReservMaxBatchK];    // owner-filled response payload (~32 KB max)
};

struct ReservationRingMatrix {
  ReservationEntry rings[kReservMaxHosts][kReservMaxHosts][kReservRingDepth];
  // Per (req_host, owner_host) tail counter (peer-host bumps to reserve a slot).
  std::atomic<uint64_t> tails[kReservMaxHosts][kReservMaxHosts];
};

inline std::size_t reservation_ring_matrix_bytes() {
  return sizeof(ReservationRingMatrix);
}

inline ReservationEntry *reservation_entry(ReservationRingMatrix *rm,
                                           int req_host, int owner_host,
                                           int slot_idx) {
  return &rm->rings[req_host][owner_host][slot_idx];
}

}  // namespace fusee
#endif
