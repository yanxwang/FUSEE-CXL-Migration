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

namespace fusee {

constexpr int kInvalMaxHosts = 4;
constexpr int kInvalRingDepth = 256;

// 64-byte cacheline. One full line per slot to avoid producer/consumer
// false sharing (same lesson as ForwardEntry / PerHostInvalEntry).
struct alignas(64) InvalEntry {
  std::atomic<uint64_t> req_op_id;   // 0 = empty; non-zero = pending
  uint64_t key;                      // target key to mark stale
  std::atomic<uint64_t> resp_op_id;  // matches req_op_id when ACKed
  int32_t status;                    // 0 = ack OK
  uint8_t  _pad[64 - 8 - 8 - 8 - 4];
};
static_assert(sizeof(InvalEntry) == 64,
              "InvalEntry must be exactly one cacheline");

struct alignas(64) InvalRing {
  std::atomic<uint64_t> tail;        // producer cursor (CXL-flushed)
  uint64_t head;                     // consumer cursor (plain, dst-local)
  uint8_t  _pad[64 - 16];
  InvalEntry entries[kInvalRingDepth];
};

struct InvalRingMatrix {
  InvalRing rings[kInvalMaxHosts][kInvalMaxHosts];
};

inline std::size_t inval_ring_matrix_bytes() {
  return sizeof(InvalRingMatrix);
}

}  // namespace fusee

#endif  // FUSEE_CXL_INVAL_RING_H_
