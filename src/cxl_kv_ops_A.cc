#include "cxl_kv_ops_A.h"

#include <cassert>
#include <cstring>
#include <ctime>

extern "C" {
#include "common.h"
}

namespace fusee {

constexpr size_t kHeaderBytes = 4096;

static inline size_t align_up(size_t n, size_t a) {
  return (n + a - 1) & ~(a - 1);
}

size_t CxlKvStoreA::bytes_for(uint32_t num_buckets) {
  size_t locks = BucketLockTable::bytes_for(num_buckets);
  size_t after_locks = align_up(kHeaderBytes + locks, 64);
  size_t buckets = sizeof(CxlKvBucket) * num_buckets;
  size_t after_buckets = align_up(after_locks + buckets, 64);
  return after_buckets + pending_ring_matrix_bytes();
}

static inline uint64_t now_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int CxlKvStoreA::attach(void *region_base, size_t region_bytes,
                        uint32_t num_buckets, int host_id, int num_hosts,
                        bool init_region) {
  if (!region_base || num_buckets == 0) return -1;
  if (num_hosts > kMaxHosts) return -1;
  if (region_bytes < bytes_for(num_buckets)) return -1;

  num_buckets_ = num_buckets;
  host_id_ = host_id;
  num_hosts_ = num_hosts;

  auto *base = reinterpret_cast<char *>(region_base);
  void *locks_base = base + kHeaderBytes;
  lock_table_.attach(locks_base, num_buckets, init_region);

  size_t locks = BucketLockTable::bytes_for(num_buckets);
  size_t after_locks = align_up(kHeaderBytes + locks, 64);
  buckets_ = reinterpret_cast<CxlKvBucket *>(base + after_locks);

  size_t after_buckets =
      align_up(after_locks + sizeof(CxlKvBucket) * num_buckets, 64);
  rings_ = reinterpret_cast<PendingRingMatrix *>(base + after_buckets);

  if (init_region) {
    for (uint32_t b = 0; b < num_buckets_; b++) {
      for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
        buckets_[b].slots[s].key = kEmptyKey;
        buckets_[b].slots[s].value = 0;
      }
      flush_region(&buckets_[b], sizeof(CxlKvBucket));
    }
    // Zero the pending rings.
    std::memset(rings_, 0, pending_ring_matrix_bytes());
    flush_region(rings_, pending_ring_matrix_bytes());
    store_fence();
  }

  stop_.store(false, std::memory_order_relaxed);
  replicator_ = std::thread(&CxlKvStoreA::replicator_loop, this);
  return 0;
}

void CxlKvStoreA::stop() {
  if (replicator_.joinable()) {
    stop_.store(true, std::memory_order_relaxed);
    replicator_.join();
  }
}

static inline void publish_slot(CxlKvSlot *slot, uint64_t key, uint64_t value) {
  slot->value = value;
  flush_line(&slot->value);
  store_fence();
  slot->key = key;
  flush_line(&slot->key);
  store_fence();
}

static inline void bump_epoch(BucketLockEntry *e) {
  uint64_t cur = CACHELINE_LOAD(&e->write_epoch);
  CACHELINE_STORE(&e->write_epoch, cur + 1);
}

int CxlKvStoreA::dispatch_and_wait(uint32_t b_idx, uint32_t s_idx,
                                   uint64_t value_word) {
  // op_id = host_id in high bits + monotonic nanoseconds (unique per caller).
  uint64_t op_id =
      ((uint64_t)(host_id_ + 1) << 56) | (now_ns() & 0x00FFFFFFFFFFFFFFULL);

  PendingRingEntry *my_entries[kMaxHosts] = {nullptr, nullptr, nullptr, nullptr};

  for (int dst = 0; dst < num_hosts_; dst++) {
    if (dst == host_id_) continue;
    PendingRing *ring = &rings_->rings[host_id_][dst];
    uint64_t t = local_tail_[dst]++;
    uint32_t slot = t % kPendingRingEntries;
    PendingRingEntry *e = &ring->entries[slot];

    // Wait for this ring slot to be free (op_id == 0).
    const int kRingWaitBudgetUs = 2000000; // 2 s sanity ceiling
    uint64_t start = now_ns();
    for (;;) {
      uint64_t cur = CACHELINE_LOAD(&e->op_id);
      if (cur == 0) break;
      if ((now_ns() - start) / 1000 > (uint64_t)kRingWaitBudgetUs) {
        return -3; // ring full / replicator stuck
      }
      __builtin_ia32_pause();
    }

    // Fill entry fields before publishing op_id last.
    CACHELINE_STORE(&e->bucket_idx, (uint64_t)b_idx);
    CACHELINE_STORE(&e->slot_idx,   (uint64_t)s_idx);
    CACHELINE_STORE(&e->new_value_lo, value_word);
    CACHELINE_STORE(&e->new_value_hi, 0ULL);
    CACHELINE_STORE(&e->processed_op_id, 0ULL);
    store_fence();
    CACHELINE_STORE(&e->op_id, op_id);

    // Publish the tail update so the consumer knows something new landed.
    CACHELINE_STORE(&ring->tail, (uint64_t)(t + 1));

    my_entries[dst] = e;
  }

  // Spin on processed_op_id == op_id on every dst we enqueued to.
  const int kAckWaitBudgetUs = 2000000;
  uint64_t start = now_ns();
  for (int dst = 0; dst < num_hosts_; dst++) {
    if (dst == host_id_ || !my_entries[dst]) continue;
    PendingRingEntry *e = my_entries[dst];
    for (;;) {
      if (CACHELINE_LOAD(&e->processed_op_id) == op_id) break;
      if ((now_ns() - start) / 1000 > (uint64_t)kAckWaitBudgetUs) return -4;
      __builtin_ia32_pause();
    }
  }

  // Clear op_id on every dst so the slot can be reused.
  for (int dst = 0; dst < num_hosts_; dst++) {
    if (dst == host_id_ || !my_entries[dst]) continue;
    CACHELINE_STORE(&my_entries[dst]->op_id, 0ULL);
  }

  return 0;
}

int CxlKvStoreA::insert(uint64_t key, uint64_t value) {
  if (key == kEmptyKey) return -1;
  uint32_t idx = bucket_idx(key);
  lock_table_.lock(idx, host_id_, num_hosts_);

  CxlKvBucket *b = &buckets_[idx];
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) flush_line(&b->slots[s].key);
  full_fence();

  int empty_idx = -1;
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
    uint64_t k = b->slots[s].key;
    if (k == key) {
      lock_table_.unlock(idx, host_id_);
      return -2; // duplicate
    }
    if (empty_idx < 0 && k == kEmptyKey) empty_idx = s;
  }
  if (empty_idx < 0) {
    lock_table_.unlock(idx, host_id_);
    return -1;
  }

  publish_slot(&b->slots[empty_idx], key, value);

  int rc = dispatch_and_wait(idx, (uint32_t)empty_idx, value);
  // Bump the reader-visible epoch regardless so seqlock readers pick it up
  // even if replication ACK timed out (they still see the authoritative slot).
  bump_epoch(lock_table_.entry(idx));
  lock_table_.unlock(idx, host_id_);
  return rc;
}

int CxlKvStoreA::update(uint64_t key, uint64_t value) {
  if (key == kEmptyKey) return -1;
  uint32_t idx = bucket_idx(key);
  lock_table_.lock(idx, host_id_, num_hosts_);

  CxlKvBucket *b = &buckets_[idx];
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) flush_line(&b->slots[s].key);
  full_fence();

  int match = -1;
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
    if (b->slots[s].key == key) { match = s; break; }
  }
  if (match < 0) { lock_table_.unlock(idx, host_id_); return -1; }

  b->slots[match].value = value;
  flush_line(&b->slots[match].value);
  store_fence();

  int rc = dispatch_and_wait(idx, (uint32_t)match, value);
  bump_epoch(lock_table_.entry(idx));
  lock_table_.unlock(idx, host_id_);
  return rc;
}

int CxlKvStoreA::remove(uint64_t key) {
  if (key == kEmptyKey) return -1;
  uint32_t idx = bucket_idx(key);
  lock_table_.lock(idx, host_id_, num_hosts_);

  CxlKvBucket *b = &buckets_[idx];
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) flush_line(&b->slots[s].key);
  full_fence();

  int match = -1;
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
    if (b->slots[s].key == key) { match = s; break; }
  }
  if (match < 0) { lock_table_.unlock(idx, host_id_); return -1; }

  b->slots[match].key = kEmptyKey;
  flush_line(&b->slots[match].key);
  store_fence();

  int rc = dispatch_and_wait(idx, (uint32_t)match, 0ULL);
  bump_epoch(lock_table_.entry(idx));
  lock_table_.unlock(idx, host_id_);
  return rc;
}

int CxlKvStoreA::search(uint64_t key, uint64_t *out) const {
  if (key == kEmptyKey) return -1;
  uint32_t idx = bucket_idx(key);
  BucketLockEntry *le = const_cast<BucketLockTable &>(lock_table_).entry(idx);
  CxlKvBucket *b = &buckets_[idx];

  for (int attempt = 0; attempt < 8; attempt++) {
    uint64_t e1 = CACHELINE_LOAD(&le->write_epoch);

    bool found = false;
    uint64_t captured = 0;
    for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
      flush_line(&b->slots[s].key);
      flush_line(&b->slots[s].value);
    }
    full_fence();
    for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
      if (b->slots[s].key == key) {
        captured = b->slots[s].value;
        found = true;
        break;
      }
    }

    uint64_t e2 = CACHELINE_LOAD(&le->write_epoch);
    if (e1 == e2) {
      if (found) { if (out) *out = captured; return 0; }
      return -1;
    }
  }
  return -1;
}

void CxlKvStoreA::replicator_loop() {
  while (!stop_.load(std::memory_order_relaxed)) {
    bool did_work = false;
    for (int src = 0; src < num_hosts_; src++) {
      if (src == host_id_) continue;
      PendingRing *ring = &rings_->rings[src][host_id_];
      uint64_t tail = CACHELINE_LOAD(&ring->tail);
      uint64_t head = CACHELINE_LOAD(&ring->head);
      while (head < tail) {
        uint32_t slot = head % kPendingRingEntries;
        PendingRingEntry *e = &ring->entries[slot];
        uint64_t op_id = CACHELINE_LOAD(&e->op_id);
        if (op_id == 0) break; // not yet published

        // Simulated "pull" — touch the CXL side of the source entry so the
        // replicator pays a comparable cost to a real fetch.
        (void)CACHELINE_LOAD(&e->new_value_lo);

        CACHELINE_STORE(&e->processed_op_id, op_id);
        head++;
        replicated_ops_.fetch_add(1, std::memory_order_relaxed);
        did_work = true;
      }
      CACHELINE_STORE(&ring->head, head);
    }
    if (!did_work) __builtin_ia32_pause();
  }
}

} // namespace fusee
