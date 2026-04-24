#include "cxl_batch_ring.h"

#include <cassert>
#include <cstring>
#include <sched.h>
#include <time.h>

namespace fusee {

namespace {
inline size_t align_up(size_t n, size_t a) {
  return (n + a - 1) & ~(a - 1);
}
}  // namespace

std::size_t MicroBatchRing::bytes_for(uint32_t num_buckets, uint32_t K) {
  std::size_t cursors_sz = align_up(
      static_cast<std::size_t>(num_buckets) * sizeof(BucketRingCursors), 64);
  std::size_t ring_sz = align_up(
      static_cast<std::size_t>(num_buckets) * K * sizeof(RingEntry), 64);
  std::size_t hdr_sz = align_up(sizeof(BatchRingHeader), 64);
  return cursors_sz + ring_sz + hdr_sz;
}

int MicroBatchRing::attach(void *base, std::size_t bytes, uint32_t num_buckets,
                           uint32_t K, uint32_t T_flush_us, bool init_region) {
  if (!base || num_buckets == 0 || K == 0) return -1;
  std::size_t need = bytes_for(num_buckets, K);
  if (bytes < need) return -1;

  num_buckets_ = num_buckets;
  K_ = K;
  T_flush_us_ = T_flush_us;

  char *p = reinterpret_cast<char *>(base);
  cursors_ = reinterpret_cast<BucketRingCursors *>(p);
  p += align_up(static_cast<std::size_t>(num_buckets) * sizeof(BucketRingCursors), 64);
  ring_ = reinterpret_cast<RingEntry *>(p);
  p += align_up(static_cast<std::size_t>(num_buckets) * K * sizeof(RingEntry), 64);
  hdr_ = reinterpret_cast<BatchRingHeader *>(p);

  if (init_region) {
    std::memset(cursors_, 0,
                static_cast<std::size_t>(num_buckets) * sizeof(BucketRingCursors));
    std::memset(ring_, 0,
                static_cast<std::size_t>(num_buckets) * K * sizeof(RingEntry));
    std::memset(hdr_, 0, sizeof(*hdr_));
    __atomic_thread_fence(__ATOMIC_RELEASE);
    hdr_->init_done.store(1, std::memory_order_release);
  } else {
    while (hdr_->init_done.load(std::memory_order_acquire) == 0) {
      sched_yield();
    }
  }
  return 0;
}

void MicroBatchRing::append(uint32_t bucket_idx, uint16_t slot_idx,
                            uint64_t value, uint64_t *ring_full_waits_out) {
  BucketRingCursors *c = &cursors_[bucket_idx];
  uint64_t pos = c->append_cursor.fetch_add(1, std::memory_order_acq_rel);

  // Spin-yield while our slot would still overlap the flush window. At
  // steady state with K large enough and the flusher alive, this loop is
  // single-digit iterations per append.
  uint64_t waits = 0;
  while (pos - __atomic_load_n(&c->flush_cursor, __ATOMIC_ACQUIRE) >= K_) {
    waits++;
    if ((waits & 0xFF) == 0) {
      // Every 256 tight spins, yield to the scheduler. Cheap safety net
      // when the flusher is CPU-starved.
      sched_yield();
    }
  }
  if (ring_full_waits_out) *ring_full_waits_out += waits;

  RingEntry *e = &ring_[static_cast<std::size_t>(bucket_idx) * K_ + (pos % K_)];
  e->slot_idx = slot_idx;
  e->seq = static_cast<uint32_t>(pos);
  e->new_value = value;
  __atomic_store_n(&e->flags, static_cast<uint16_t>(1),
                   __ATOMIC_RELEASE);  // publish: ready bit

  // Dedup: only push to dirty queue if we are the first writer after the
  // last drain. CAS 0->1 on the per-bucket `queued` flag; loser writers
  // skip the push because the winner already enqueued.
  uint8_t expected = 0;
  if (c->queued.compare_exchange_strong(expected, 1,
                                         std::memory_order_acq_rel,
                                         std::memory_order_relaxed)) {
    uint64_t dq_pos = hdr_->dq_tail.fetch_add(1, std::memory_order_acq_rel);
    uint64_t head = hdr_->dq_head.load(std::memory_order_acquire);
    if (dq_pos - head < kDirtyQueueCapacity) {
      // Two-phase publish so flusher never reads a half-committed slot.
      __atomic_store_n(&hdr_->dq_slots[dq_pos % kDirtyQueueCapacity],
                       bucket_idx, __ATOMIC_RELAXED);
      hdr_->dq_ready[dq_pos % kDirtyQueueCapacity].store(
          1, std::memory_order_release);
    } else {
      // Queue overflow: release our claim so a subsequent writer can try
      // to enqueue again once the queue drains. Correctness still holds;
      // the flusher's idle-scan picks up any pending work eventually.
      c->queued.store(0, std::memory_order_release);
    }
  }
}

bool MicroBatchRing::has_pending(uint32_t bucket_idx) const {
  BucketRingCursors *c = &cursors_[bucket_idx];
  uint64_t a = c->append_cursor.load(std::memory_order_acquire);
  uint64_t f = __atomic_load_n(&c->flush_cursor, __ATOMIC_ACQUIRE);
  return a > f;
}

}  // namespace fusee
