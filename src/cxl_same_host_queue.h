#ifndef FUSEE_CXL_SAME_HOST_QUEUE_H_
#define FUSEE_CXL_SAME_HOST_QUEUE_H_

// Same-host DRAM bypass queue (2a).
//
// Reason: Phase 4's per-client CXL PendingRing makes every writer broadcast
// to N-1 peers on every op. For N = H × T clients, that's O(N²) CXL fabric
// traffic per op. When the dst peer is on the SAME physical host as the
// writer, we don't need CXL semantics at all — same host = same cache-coherent
// domain = a plain std::atomic in a MAP_SHARED anonymous mmap is enough.
//
// Each (src_client_in_host, dst_client_in_host) pair gets its own SPSC
// queue in DRAM. Queues live in a mmap'd region allocated by the parent
// process before fork, so all children inherit identical virtual addresses.
//
// Entry semantics: one "invalidation message" = (bucket_idx, op_id). The
// writer doesn't need to send the value — same-host peers share the CXL
// bucket through their own mmap, so they can re-read it directly if they
// care to. Option B consumer only needs to set cache_epoch[bucket] = MAX.
// Option A consumer same, plus the writer waits for processed_op_id.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <time.h>

namespace fusee {

constexpr int kSameHostMaxClients = 128;
constexpr int kSameHostQueueDepth = 64;  // generous; DRAM is cheap

struct alignas(64) DramInvalEntry {
  std::atomic<uint64_t> op_id;           // 0 = free; writer publishes last
  uint64_t              bucket_idx;
  std::atomic<uint64_t> processed_op_id; // consumer ACKs here (A only)
  uint64_t              _pad[5];
};
static_assert(sizeof(DramInvalEntry) == 64,
              "DramInvalEntry must fit one cacheline");

struct alignas(64) DramInvalQueue {
  std::atomic<uint64_t> tail;              // producer cursor (src)
  std::atomic<uint64_t> head;              // consumer cursor (dst)
  uint64_t              _pad[6];
  DramInvalEntry        entries[kSameHostQueueDepth];
};
static_assert(sizeof(DramInvalQueue) ==
                  64 + sizeof(DramInvalEntry) * kSameHostQueueDepth,
              "unexpected DramInvalQueue layout");

// Matrix of [src_cid][dst_cid] queues for one host. (Only src != dst are used.)
struct DramInvalMatrix {
  DramInvalQueue rings[kSameHostMaxClients][kSameHostMaxClients];
};

inline size_t dram_inval_matrix_bytes() {
  return sizeof(DramInvalMatrix);
}

// ----- writer-side -----

// Push an invalidation message to peer `dst_cid` in this host. Non-blocking
// on free slots (same-host consumer drains quickly). Returns 0 on success.
inline int dram_push(DramInvalMatrix *mat, int src_cid, int dst_cid,
                     uint64_t bucket_idx, uint64_t op_id) {
  DramInvalQueue *q = &mat->rings[src_cid][dst_cid];
  uint64_t t = q->tail.load(std::memory_order_relaxed);
  DramInvalEntry *e = &q->entries[t % kSameHostQueueDepth];
  // Wait for free slot (op_id == 0). Same-host consumer should drain
  // within ~100 ns so spinning is fine.
  while (e->op_id.load(std::memory_order_acquire) != 0) {
    __builtin_ia32_pause();
  }
  // Fill payload, publish op_id last.
  e->bucket_idx = bucket_idx;
  e->processed_op_id.store(0, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  e->op_id.store(op_id, std::memory_order_release);
  q->tail.store(t + 1, std::memory_order_release);
  return 0;
}

// Writer (Option A) spin-waits for consumer ACK on a queue slot. Returns
// true if ACK arrived in budget, false on timeout.
inline bool dram_wait_ack(DramInvalMatrix *mat, int src_cid, int dst_cid,
                          uint64_t op_id, uint64_t produced_t,
                          uint64_t budget_us) {
  DramInvalQueue *q = &mat->rings[src_cid][dst_cid];
  DramInvalEntry *e = &q->entries[produced_t % kSameHostQueueDepth];
  // Minimal spin — same-host ACK is sub-microsecond in practice.
  struct timespec ts0;
  clock_gettime(CLOCK_MONOTONIC, &ts0);
  uint64_t t0_us = (uint64_t)ts0.tv_sec * 1000000ULL + (uint64_t)ts0.tv_nsec / 1000;
  for (;;) {
    if (e->processed_op_id.load(std::memory_order_acquire) == op_id) return true;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_us = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
    if (now_us - t0_us > budget_us) return false;
    __builtin_ia32_pause();
  }
}

// Clear the slot so the consumer's head can advance. Called after A's
// ACK wait, or unconditionally by the producer for B.
inline void dram_release(DramInvalMatrix *mat, int src_cid, int dst_cid,
                         uint64_t produced_t) {
  DramInvalQueue *q = &mat->rings[src_cid][dst_cid];
  DramInvalEntry *e = &q->entries[produced_t % kSameHostQueueDepth];
  e->op_id.store(0, std::memory_order_release);
}

// ----- consumer-side -----

// Consume one entry from peer `src_cid` into dst_cid's queue. Returns a
// non-zero op_id if an entry was consumed (and fills out bucket_idx);
// returns 0 if nothing new to consume.
inline uint64_t dram_try_consume(DramInvalMatrix *mat, int src_cid, int dst_cid,
                                 uint64_t *bucket_out) {
  DramInvalQueue *q = &mat->rings[src_cid][dst_cid];
  uint64_t head = q->head.load(std::memory_order_relaxed);
  uint64_t tail = q->tail.load(std::memory_order_acquire);
  if (head >= tail) return 0;
  DramInvalEntry *e = &q->entries[head % kSameHostQueueDepth];
  uint64_t op_id = e->op_id.load(std::memory_order_acquire);
  if (op_id == 0) return 0;
  *bucket_out = e->bucket_idx;
  q->head.store(head + 1, std::memory_order_release);
  return op_id;
}

// Mark the consumed entry ACK'd (Option A).
inline void dram_ack(DramInvalMatrix *mat, int src_cid, int dst_cid,
                     uint64_t consumed_head, uint64_t op_id) {
  DramInvalQueue *q = &mat->rings[src_cid][dst_cid];
  DramInvalEntry *e = &q->entries[(consumed_head - 1) % kSameHostQueueDepth];
  e->processed_op_id.store(op_id, std::memory_order_release);
  // Note: leaving op_id non-zero until the WRITER clears it in dram_release
  // would be one approach, but for B (no writer wait) we want the slot
  // reusable immediately. B's consumer clears op_id directly; A's writer
  // clears after ACK. The op-specific path calls dram_release for B.
}

} // namespace fusee

#endif // FUSEE_CXL_SAME_HOST_QUEUE_H_
