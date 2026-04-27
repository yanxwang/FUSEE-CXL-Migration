#ifndef FUSEE_CXL_PER_HOST_RING_H_
#define FUSEE_CXL_PER_HOST_RING_H_

// iter-2A-revised — N:1:1:N aggregation primitives on CXL.
//
// The single CXL hop in the writer commit path crosses one of these
// rings: the local aggregator (DRAM, MPSC) gathers writer enqueues
// behind exactly one sender thread per host; the sender batches K
// entries, single-flushes them across CXL into the per-(src_host,
// dst_host) ring; the receiver thread on the destination drains
// sequentially and atomic_stores invalidations into the local
// cache_epoch_arr (no DramInvalQueue fan-out, x86 coherence does it).
// AckChannel is a per-(src,dst) seq counter the receiver advances; the
// sender spins on it; the worker spins on the in-DRAM worker_ack_buf
// the sender flips when its op_id finally clears.
//
// Both ring and ack channel live in shared CXL memory (the host that
// owns the writer side flushes; the receiver host clflushopts to
// pull). They are SPSC end to end: 1 sender produces, 1 receiver
// consumes. Sub-cacheline sharing is forbidden — every entry +
// every counter occupies its own 64-B line. Same lesson as
// PendingRingEntry's 2-cacheline split (cxl_pending_ring.h note from
// 2026-04-22) and the iter-2A wire failure (entry 32-B repacked
// after producers on adjacent slots false-shared).

#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace fusee {

constexpr int kMaxPhysicalHosts = 4;
constexpr int kPerHostSpscDepth = 256;  // power of two, per-(src,dst) ring

// 64-B per entry, full cacheline owned by exactly one ring slot.
//   bucket_idx: which CXL bucket the receiver should refresh
//   new_epoch: the bucket's new write_epoch (receiver atomic_stores
//              this into cache_epoch_arr[bucket_idx])
//   src_worker_slot: index into the source host's worker_ack_buf so
//              the source-side sender knows which worker to ACK once
//              the receiver acknowledges this entry
//   src_worker_op_id: the source worker's per-op id (uniqueness +
//              double-ACK guard); also doubles as the ring-slot
//              "ready" sentinel. 0 = slot free.
//   _pad rounds the entry up to one full cacheline.
struct alignas(64) PerHostInvalEntry {
  uint64_t bucket_idx;
  uint64_t new_epoch;
  uint32_t src_worker_slot;
  uint32_t _pad32;
  uint64_t src_worker_op_id;   // 0 = slot free; written last (release)
  uint64_t _pad[4];
};
static_assert(sizeof(PerHostInvalEntry) == 64,
              "PerHostInvalEntry must occupy one full 64-B cacheline");

// SPSC ring: 1 src-host sender writes; 1 dst-host receiver reads. head
// and tail live in separate cachelines so producer / consumer cacheline
// updates don't ping-pong each other.
struct alignas(64) PerHostSpscRing {
  // Producer (sender) cursor. clflushopt'd by sender after each batch
  // publish; receiver clflushopt+load to pull.
  std::atomic<uint64_t> tail;
  char _pad_tail[64 - sizeof(std::atomic<uint64_t>)];
  // Consumer (receiver) cursor. Plain uint64_t; only the receiver
  // writes it. Sender doesn't read head.
  uint64_t head;
  char _pad_head[64 - sizeof(uint64_t)];
  PerHostInvalEntry entries[kPerHostSpscDepth];
};

// Per-(src_host, dst_host) ACK channel. The DST receiver bumps `seq`
// every time it processes an entry; the SRC sender clflushopt+loads
// `seq` to know when its batch is acknowledged. Then the sender
// flips the per-worker worker_ack_buf slot in DRAM so the original
// writer can return.
struct alignas(64) AckChannel {
  std::atomic<uint64_t> seq;
  char _pad[64 - sizeof(std::atomic<uint64_t>)];
};

// CXL-resident matrix: every (src, dst) pair has a ring + ack channel.
// At H = 2 only off-diagonal cells are used.
struct PerHostOutMatrix {
  PerHostSpscRing rings[kMaxPhysicalHosts][kMaxPhysicalHosts];
  AckChannel      acks[kMaxPhysicalHosts][kMaxPhysicalHosts];
};

inline size_t per_host_out_matrix_bytes() {
  return sizeof(PerHostOutMatrix);
}

// Backwards-compat type aliases — code that referenced the iter-2A
// dead-code names still compiles. New code should use the names above.
using PerHostOutEntry = PerHostInvalEntry;
using PerHostOutRing  = PerHostSpscRing;
constexpr int kPerHostRingDepth = kPerHostSpscDepth;

} // namespace fusee

#endif // FUSEE_CXL_PER_HOST_RING_H_
