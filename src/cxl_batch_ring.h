#ifndef FUSEE_CXL_BATCH_RING_H_
#define FUSEE_CXL_BATCH_RING_H_

// Per-host DRAM pending-write ring for protocol C UPDATE micro-batching.
//
// Only one host's clients ever write into its own ring; the peer host never
// touches it. Therefore the ring lives in host-local MAP_SHARED|MAP_ANONYMOUS
// memory (pre-fork allocation by the primary client, children inherit the
// address), NOT in CXL. This saves the ~3 µs CXL roundtrip per append that
// batching is supposed to eliminate.
//
// Layout (one contiguous MAP_SHARED region, carved by bytes_for / attach):
//
//   [cursors[num_buckets]]         - per-bucket {append_cursor, flush_cursor}
//   [ring_entries[num_buckets][K]] - row-major ring payload
//   [dirty_queue hdr + slots]      - MPSC bounded queue of bucket_idx notifications
//   [batch_stop (atomic bool)]     - flusher stop flag
//   [batch_init (atomic u64)]      - 1 once primary has memset everything
//
// Sizing is a runtime function of K so we can sweep K without rebuilding.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace fusee {

struct alignas(64) BucketRingCursors {
  // Writers fetch_add(append_cursor, 1) to claim a unique ring position.
  std::atomic<uint64_t> append_cursor;
  // `queued`: 0 = bucket not in the flusher's dirty queue. Writer that
  // successfully flips 0->1 via CAS is the one responsible for pushing
  // onto the dirty queue; flusher clears to 0 at end of drain. Cap on
  // dq inflation: each hot bucket contributes at most one enqueue per
  // drain cycle.
  std::atomic<uint8_t> queued;
  // `draining`: mutual exclusion between concurrent flushers. Whichever
  // thread CAS-wins 0 -> 1 gets to run drain_bucket(); losers skip. Needed
  // once we spawn N > 1 flushers (multiple consumers on the dirty queue).
  std::atomic<uint8_t> draining;
  char _pad1[64 - sizeof(std::atomic<uint64_t>) - 2 * sizeof(std::atomic<uint8_t>)];
  // Flusher-only store; readers load relaxed (a slightly stale value just
  // means more spin-wait slack on the producer side).
  uint64_t flush_cursor;
  char _pad2[64 - sizeof(uint64_t)];
};
static_assert(sizeof(BucketRingCursors) == 128,
              "BucketRingCursors must be exactly 2 cachelines");

// 16 B per entry: slot_idx (which of the 7 slots to overwrite), flags
// (primarily bit 0 = "entry written" so a slow writer can publish its
// payload before flusher reads it; flusher spins on this bit before drain),
// seq (== the append_cursor value at claim time, lets the flusher detect
// lapped-around writes), and the new value itself.
struct RingEntry {
  uint16_t slot_idx;
  uint16_t flags;   // bit 0 = ready; bits 1..15 reserved
  uint32_t seq;
  uint64_t new_value;
};
static_assert(sizeof(RingEntry) == 16, "RingEntry must be exactly 16 B");

constexpr uint32_t kDirtyQueueCapacity = 8192;

struct BatchRingHeader {
  // Flusher stop flag; primary sets on store::stop().
  std::atomic<bool> stop;
  char _pad_stop[64 - sizeof(std::atomic<bool>)];
  // Init-done flag so non-primary children spin until primary memsets the
  // region. Primary writes 1 after init.
  std::atomic<uint64_t> init_done;
  char _pad_init[64 - sizeof(std::atomic<uint64_t>)];
  // MPMC dirty-bucket queue. Writers fetch_add tail; if tail - head >=
  // kDirtyQueueCapacity, skip the push (flusher's T-timer will catch it).
  // Multiple flushers compete on dq_head via fetch_add too — needed once
  // N > 1 flushers share the queue.
  std::atomic<uint64_t> dq_tail;
  char _pad_tail[64 - sizeof(std::atomic<uint64_t>)];
  std::atomic<uint64_t> dq_head;
  char _pad_head[64 - sizeof(std::atomic<uint64_t>)];
  uint32_t dq_slots[kDirtyQueueCapacity];
};

class MicroBatchRing {
 public:
  // Sizing: how many bytes does the shared-memory blob need for
  // (num_buckets, K)?
  static std::size_t bytes_for(uint32_t num_buckets, uint32_t K);

  // Attach a pre-mapped MAP_SHARED|MAP_ANONYMOUS blob. `init_region` is set
  // on exactly one process per host (the primary client). Non-primary
  // processes spin until init_done flips.
  int attach(void *base, std::size_t bytes, uint32_t num_buckets, uint32_t K,
             uint32_t T_flush_us, bool init_region);

  // Writer path. Returns when the entry is in the ring and visible to the
  // flusher. Blocks (spin-yield) when the ring is full for this bucket.
  // `ring_full_waits_out` lets the caller count backpressure stalls.
  void append(uint32_t bucket_idx, uint16_t slot_idx, uint64_t value,
              uint64_t *ring_full_waits_out = nullptr);

  bool has_pending(uint32_t bucket_idx) const;
  uint32_t K() const { return K_; }
  uint32_t T_flush_us() const { return T_flush_us_; }
  uint32_t num_buckets() const { return num_buckets_; }
  bool valid() const { return cursors_ != nullptr; }

  // Raw accessors for the flusher to read/write ring state under its own
  // synchronisation.
  BucketRingCursors *cursors() { return cursors_; }
  RingEntry         *ring()    { return ring_; }
  BatchRingHeader   *header()  { return hdr_; }

 private:
  uint32_t num_buckets_ = 0;
  uint32_t K_ = 0;
  uint32_t T_flush_us_ = 0;
  BucketRingCursors *cursors_ = nullptr;
  RingEntry *ring_ = nullptr;
  BatchRingHeader *hdr_ = nullptr;
};

// Convenience for the writer's ring_full_waits counter (thread-local,
// accumulated across one client's lifetime).
}  // namespace fusee

#endif  // FUSEE_CXL_BATCH_RING_H_
