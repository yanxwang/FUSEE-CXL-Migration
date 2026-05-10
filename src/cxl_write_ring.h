#ifndef FUSEE_CXL_WRITE_RING_H_
#define FUSEE_CXL_WRITE_RING_H_

// Protocol A v2 — write channel (Phase 2.A).
//
// iter-9A redo replaces the iter-5A unified `cxl_forward_ring.h`
// (which carried op_kind ∈ {UPDATE, INSERT, DELETE, CACHE_REGISTER}
// in a single ring with an inline 1024 B payload — that violated C2)
// with three disjoint per-channel rings:
//
//   WriteRing  (this file)  — op 1/2/3 UPDATE/INSERT/DELETE
//   ReadRing   (cxl_read_ring.h) — op 4 CACHE_REGISTER
//   InvalRing  (cxl_inval_ring.h, unchanged from iter-5A) — op 5
//
// Hard constraints honoured by this layout:
//
//   C2  — message ring entries MUST NOT inline value bytes. WriteEntry
//         carries only (key, op_kind, value_len, staging_off,
//         staging_gen) on cacheline 1 and (status,) on cacheline 2.
//         The 1024 B payload is gone. `static_assert(sizeof(WriteEntry)
//         == 128, ...)` and the lack of any `payload` member fail loud
//         if anyone tries to bring it back.
//
//   iter-6A 2-cacheline split — req on cacheline 1 (producer-owned),
//         resp on cacheline 2 (consumer-owned). Avoids cross-host
//         cacheline ping-pong on the spin-wait line. Same lever as
//         InvalEntry / PerHostSpscRing.
//
// Data plane for op 1/2 (UPDATE/INSERT):
//   1. Forwarder allocates `staging_off = ForwardStaging[me].alloc(value_len)`
//      (CXL bump-cursor, see cxl_forward_staging.h).
//   2. Forwarder memcpy + flush_region value bytes into staging arena.
//   3. Forwarder enqueues WriteEntry { key, op_kind, value_len,
//      staging_off, staging_gen } on WriteRing[me][owner].
//   4. Owner's WriteReceiver thread drains, reads value bytes from
//      staging[forwarder].base + staging_off (LD-CXL after flush),
//      copies into owner's pool block, executes execute_write_local,
//      ACKs via resp_op_id.
//   5. After ACK, forwarder frees the staging slot.
//
// Data plane for op 3 (DELETE):
//   No value bytes; staging_off and value_len are both 0.
//
// staging_gen is reserved for a future cross-host pool generation
// scheme (iter-10A backlog #6) — for iter-9A redo we always set it to
// 0 and the receiver ignores it.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fusee {

constexpr int kWriteRingDepth = 256;
constexpr int kWriteMaxHosts  = 4;

struct alignas(64) WriteEntry {
  // Cacheline 1 — producer-owned (the forwarder).
  std::atomic<uint64_t> req_op_id;   // 0 = empty; non-zero = pending
  uint64_t key;                      // target key
  uint8_t  op_kind;                  // 0=update, 1=insert, 2=delete
  uint8_t  _pad_a[3];
  uint32_t value_len;                // bytes in staging arena
  uint64_t staging_off;              // forwarder's ForwardStaging arena offset
  uint64_t staging_gen;              // reserved (iter-10A pool-gen)
  uint8_t  _pad_b[64 - 8 - 8 - 4 - 4 - 8 - 8];

  // Cacheline 2 — consumer-owned (owner's WriteReceiver).
  std::atomic<uint64_t> resp_op_id;  // matches req_op_id when ACKed
  int32_t  status;                   // 0 = ok; <0 error
  uint8_t  _pad_c[64 - 8 - 4];
};
static_assert(sizeof(WriteEntry) == 128,
              "WriteEntry must be exactly two cachelines (iter-6A layout)");

// C2 enforcement: no `payload` field in WriteEntry. If a future patch
// re-adds inline value bytes, change one of these constants and the
// build will catch it; we deliberately tie the assertion to the
// member-by-member size budget so a stealth `uint8_t payload[N]` push
// breaks compilation here, not silently at runtime.
static_assert(sizeof(WriteEntry::key) +
              sizeof(WriteEntry::op_kind) +
              sizeof(WriteEntry::_pad_a) +
              sizeof(WriteEntry::value_len) +
              sizeof(WriteEntry::staging_off) +
              sizeof(WriteEntry::staging_gen) +
              sizeof(WriteEntry::req_op_id) +
              sizeof(WriteEntry::_pad_b) ==
              64,
              "WriteEntry cacheline 1 budget exhausted (C2)");

struct alignas(64) WriteRing {
  // CRITICAL: tail and head MUST live on separate cachelines.
  // Forwarder writes tail (CXL atomic fetch_add + flush); receiver
  // writes head (plain store, then implicitly flushed when the line
  // is re-loaded for tail polling). On non-coherent CXL Type 3,
  // sharing a cacheline causes last-writer-wins on the whole line —
  // either tail's increment or head's advance is silently lost,
  // producing the iter-9A redo Phase 2 cascade where ~60% of
  // forwards time out.  Original cxl_forward_ring.h split them
  // correctly; the iter-6A 2-cacheline-entry pattern was
  // mis-applied at the ring header level when this file was first
  // sketched.
  std::atomic<uint64_t> tail;        // producer cursor
  char _pad_tail[64 - sizeof(std::atomic<uint64_t>)];
  uint64_t head;                     // consumer cursor (dst-local)
  char _pad_head[64 - sizeof(uint64_t)];
  WriteEntry entries[kWriteRingDepth];
};

// (src_host, dst_host) -> ring. Allocated in CXL region.
struct WriteRingMatrix {
  WriteRing rings[kWriteMaxHosts][kWriteMaxHosts];
};

inline std::size_t write_ring_matrix_bytes() {
  return sizeof(WriteRingMatrix);
}

}  // namespace fusee

#endif  // FUSEE_CXL_WRITE_RING_H_
