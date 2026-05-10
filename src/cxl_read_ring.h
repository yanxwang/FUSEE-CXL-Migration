#ifndef FUSEE_CXL_READ_RING_H_
#define FUSEE_CXL_READ_RING_H_

// Protocol A v2 — read channel (Phase 2.A, op 4 CACHE_REGISTER).
//
// Per task_plan_iter9A.md §2.A, the read channel is the place where
// iter-9A's strict "value bytes never on the message ring" (C2) pays
// off the most: §I9 register-then-fill becomes register-only on the
// ring + reader-direct LD-CXL on the data plane.
//
// Producer (reader) side:
//   1. Reader miss in local cache → forward_cache_register(owner, key).
//   2. Allocate a slot on ReadRing[me][owner], fill ReadEntry.req:
//        { key, } and the req_op_id = (host_id << 56) | seq.
//   3. Spin on resp_op_id.
//
// Consumer (owner's ReadReceiver) side:
//   1. Drain ReadRing[*][me]; for each entry:
//        a. Look up (key) in own bucket → encoded slot value.
//        b. Decode size_class + blk_off (in OWNER's CxlKvBlockPool segment).
//        c. Acquire SlotDirectory lock for that slot, set sharer_bitmap
//           bit for the requester, release.
//        d. Write resp into ReadEntry.resp:
//             { status=0, value_len, owner_blk_off }
//        e. resp_op_id = req_op_id (release).
//   2. Reader observes resp_op_id, calls
//        pool->read(owner_blk_off, out_buf, value_len)
//      which already routes the LD-CXL through the right host segment.
//
// IMPORTANT: ReadEntry intentionally does NOT carry value bytes —
// that was the iter-9A original failure mode (1088 B inline payload).
// The reader pulls bytes directly from the owner's CxlKvBlockPool via
// pool->read, which is one round-trip on CXL but byte-precise and
// fence-clean. C2 is enforced by static_asserting away any payload
// member.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fusee {

constexpr int kReadRingDepth = 256;
constexpr int kReadMaxHosts  = 4;

struct alignas(64) ReadEntry {
  // Cacheline 1 — producer-owned (the reader).
  std::atomic<uint64_t> req_op_id;   // 0 = empty; non-zero = pending
  uint64_t key;                      // target key to register-then-fill
  uint8_t  _pad_a[64 - 8 - 8];

  // Cacheline 2 — consumer-owned (owner's ReadReceiver).
  std::atomic<uint64_t> resp_op_id;  // matches req_op_id when filled
  int32_t  status;                   // 0 = ok; -1 = key-not-found
  uint32_t resp_value_len;           // bytes the reader should LD-CXL
  uint64_t resp_blk_off;             // absolute pool offset in owner's segment
  uint8_t  _pad_c[64 - 8 - 4 - 4 - 8];
};
static_assert(sizeof(ReadEntry) == 128,
              "ReadEntry must be exactly two cachelines (iter-6A layout)");

// C2 enforcement: cacheline 1 must not contain anything beyond
// {req_op_id, key, padding}. Any future field — much less an inline
// value buffer — will fail compilation here.
static_assert(sizeof(ReadEntry::req_op_id) +
              sizeof(ReadEntry::key) +
              sizeof(ReadEntry::_pad_a) ==
              64,
              "ReadEntry cacheline 1 budget exhausted (C2)");

struct alignas(64) ReadRing {
  // See cxl_write_ring.h for why tail and head must occupy separate
  // cachelines on non-coherent CXL Type 3.
  std::atomic<uint64_t> tail;
  char _pad_tail[64 - sizeof(std::atomic<uint64_t>)];
  uint64_t head;
  char _pad_head[64 - sizeof(uint64_t)];
  ReadEntry entries[kReadRingDepth];
};

struct ReadRingMatrix {
  ReadRing rings[kReadMaxHosts][kReadMaxHosts];
};

inline std::size_t read_ring_matrix_bytes() {
  return sizeof(ReadRingMatrix);
}

}  // namespace fusee

#endif  // FUSEE_CXL_READ_RING_H_
