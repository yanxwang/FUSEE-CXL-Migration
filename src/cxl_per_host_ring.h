#ifndef FUSEE_CXL_PER_HOST_RING_H_
#define FUSEE_CXL_PER_HOST_RING_H_

// iter-1A Solution 1 — per-host MPSC ring for cross-host PendingRingEntry
// aggregation. Models methodology §9.1 "Aggregate-before-CXL":
//
//   Legacy [kMaxHosts][kMaxHosts] = [200][200] = 40 000 SPSC rings, with
//   per-(src_worker, dst_worker) ring traffic. At T=64 each UPDATE writes
//   N-1 = 127 cachelines to CXL, easily exceeding the 25 GB/s aggregate
//   ceiling above T~16.
//
//   New [kMaxPhysicalHosts][kMaxPhysicalHosts] = [4][4] = 16 MPSC rings.
//   N producer clients on each src_host enqueue to a single outgoing
//   ring per dst_host (atomic fetch_add tail; same correctness pattern
//   as iter-5 V2 DirtyQueueShard). Each UPDATE puts at most 1 entry on
//   each peer-host outgoing ring; the receiving host's replicator
//   processes entries and dispatches local invalidations via Phase-4
//   DramInvalQueue.
//
// Layout (designed to fit comfortably inside the existing CXL region):
//   [PerHostOutRing rings[H][H]]      — H × H matrix; only off-diagonal used
//   each PerHostOutRing:
//     [tail (atomic, multi-producer)] — 1 cacheline
//     [head (single-consumer)]         — 1 cacheline
//     [entries[kPerHostRingDepth]]     — circular buffer (32 B/entry)
//
// Sizing: with H=4 and kPerHostRingDepth=8192 the matrix totals
// 4×4×(2*64 + 8192*32) = ~4.2 MB, ~330× smaller than the legacy 1.28 GB
// PendingRingMatrix.
//
// Opt-in: gated by env `FUSEE_PER_HOST_RING=1` at attach time. Default
// 0 preserves legacy SPSC code path byte-for-byte.

#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace fusee {

constexpr int kMaxPhysicalHosts = 4;
constexpr int kPerHostRingDepth = 8192;  // power-of-two

// 64-B per entry (one full cacheline). Sub-cacheline sharing across
// hosts on coherence-less CXL produces false-sharing torn-write
// scenarios — same lesson as PendingRingEntry's 2-cacheline split
// (see iter-5 cxl_pending_ring.h note from 2026-04-22). Each entry
// owns its own cacheline so producer's clflushopt and receiver's
// clflushopt can proceed independently. Solution-2 byte-compression
// (plan §2.1) targets the *payload* sized inside this 64 B cell, not
// the cacheline alignment.
struct alignas(64) PerHostOutEntry {
  uint32_t bucket_idx;   // u32 enough for num_buckets ≤ 2^32
  uint16_t slot_idx;
  uint16_t src_worker;   // for ACK routing back to the originating client
  uint64_t new_value;
  uint64_t op_id;        // 0 = free; written last (release fence)
  uint64_t _pad[5];      // pad to 64 B (full cacheline)
};
static_assert(sizeof(PerHostOutEntry) == 64,
              "PerHostOutEntry must occupy one full cacheline");

// Per-(src_host, dst_host) MPSC ring. Producers fetch_add(tail) atomically;
// consumer (replicator on dst_host) reads sequentially via head.
struct alignas(64) PerHostOutRing {
  std::atomic<uint64_t> tail;            // multi-producer fetch_add
  char _pad_tail[64 - sizeof(std::atomic<uint64_t>)];
  uint64_t              head;            // single-consumer plain
  char _pad_head[64 - sizeof(uint64_t)];
  std::atomic<uint64_t> ack_seq;         // replicator publishes per-host ACK
  char _pad_ack[64 - sizeof(std::atomic<uint64_t>)];
  PerHostOutEntry       entries[kPerHostRingDepth];
};

struct PerHostOutMatrix {
  PerHostOutRing rings[kMaxPhysicalHosts][kMaxPhysicalHosts];
};

inline size_t per_host_out_matrix_bytes() {
  return sizeof(PerHostOutMatrix);
}

} // namespace fusee

#endif // FUSEE_CXL_PER_HOST_RING_H_
