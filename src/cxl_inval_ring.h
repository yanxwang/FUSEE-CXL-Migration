#ifndef FUSEE_CXL_INVAL_RING_H_
#define FUSEE_CXL_INVAL_RING_H_

// iter-5A Phase 4: separate invalidate channel for §I9 strict-A.
//
// The iter-4A-redo deadlock arose because writer-side OP_INVALIDATE
// broadcasts (via ForwardRingMatrix) require ACK from peer responder,
// while peer responder is itself executing forward ops that may
// trigger their own invalidates back at us. Circular wait.
//
// Fix: a SEPARATE channel (this file) carries only OP_INVALIDATE
// messages, drained by a dedicated `cache_dispatcher_loop` thread per
// host. The dispatcher does NOT call execute_write_local (no
// directory lock acquired); it only flips the cache_pool stale flag
// and ACKs.
//
// This is the standard "separate invalidation network" pattern from
// MESI directory protocols (Hill, Wood et al.): merging request +
// response on one virtual channel and putting invalidate on a
// disjoint channel breaks the cyclic dependency.
//
// Per spec §VI-A.bis MESSAGE PAYLOAD POLICY: no inline value bytes.
// The InvalEntry carries only key + req/resp op_id + status (32 B
// useful, padded to 64 B for cacheline isolation).
//
// Layout: InvalRingMatrix has [H][H] rings; rings[src][dst] is
// drained by dst's cache_dispatcher.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "cxl_write_ring.h"  // kRingShardsMax (iter-17A scaling)

namespace fusee {

constexpr int kInvalMaxHosts = 4;
constexpr int kInvalRingDepth = 256;

// iter-6A Phase 5/6: 128 B (2 cachelines) — req on first, resp on
// second. Producer never writes line 2; consumer never writes line 1.
// Fixes producer-consumer cacheline ping-pong over CXL Type 3 (no
// cross-host coherence) which iter-5A's packed 1-line layout
// suffered: 200ms tail timeouts on producer's spin_wait observing ACK.
//
// See docs/iters/iter6A_phase5_rap.md for the RAP justifying this
// layout. Same lever as PerHostSpscRing's tail/head separation.
struct alignas(64) InvalEntry {
  // First cacheline — producer-owned (host that calls send_invalidate).
  std::atomic<uint64_t> req_op_id;   // 0 = empty; non-zero = pending
  uint64_t key;                      // target key to mark stale
  uint8_t  _pad_p[64 - 8 - 8];
  // Second cacheline — consumer-owned (cache_dispatcher_loop).
  std::atomic<uint64_t> resp_op_id;  // matches req_op_id when ACKed
  int32_t  status;                   // 0 = ack OK
  uint8_t  _pad_c[64 - 8 - 4];
};
static_assert(sizeof(InvalEntry) == 128,
              "InvalEntry must be exactly two cachelines (iter-6A layout)");

struct alignas(64) InvalRing {
  // iter-9A redo Phase 2 fix: tail and head MUST be on separate
  // cachelines. The original iter-5A InvalRing put them on the same
  // 64 B header, which works on cache-coherent hardware but on
  // non-coherent CXL Type 3 causes false-sharing — forwarder writes
  // tail, receiver writes head, last-writer-wins clobbers one or the
  // other. The bug was latent in iter-9A original (less invalidate
  // traffic than write/read forwards) but surfaced as a 60%-timeout
  // cascade once iter-9A redo Phase 2.A built WriteRing/ReadRing
  // with the same flawed layout. See cxl_write_ring.h header for the
  // full RAP.
  std::atomic<uint64_t> tail;        // producer cursor
  char _pad_tail[64 - sizeof(std::atomic<uint64_t>)];
  uint64_t head;                     // consumer cursor (dst-local)
  char _pad_head[64 - sizeof(uint64_t)];
  InvalEntry entries[kInvalRingDepth];
};

// iter-17A: 3D layout (src, dst, ring_shard) — see cxl_write_ring.h.
struct InvalRingMatrix {
  InvalRing rings[kInvalMaxHosts][kInvalMaxHosts][kRingShardsMax];
};

inline std::size_t inval_ring_matrix_bytes() {
  return sizeof(InvalRingMatrix);
}

inline InvalRing *inval_ring_shard(InvalRingMatrix *m, int src, int dst,
                                   int shard) {
  return &m->rings[src][dst][shard];
}

}  // namespace fusee

#endif  // FUSEE_CXL_INVAL_RING_H_
