#include "cxl_kv_ops_B.h"

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

size_t CxlKvStoreB::bytes_for(uint32_t num_buckets) {
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

int CxlKvStoreB::attach(void *region_base, size_t region_bytes,
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
    std::memset(rings_, 0, pending_ring_matrix_bytes());
    flush_region(rings_, pending_ring_matrix_bytes());
    store_fence();
  }

  stop_.store(false, std::memory_order_relaxed);
  replicator_ = std::thread(&CxlKvStoreB::replicator_loop, this);
  return 0;
}

void CxlKvStoreB::stop() {
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

int CxlKvStoreB::dispatch_nowait(uint32_t b_idx, uint32_t s_idx,
                                 uint64_t value_word) {
  uint64_t op_id =
      ((uint64_t)(host_id_ + 1) << 56) | (now_ns() & 0x00FFFFFFFFFFFFFFULL);
  for (int dst = 0; dst < num_hosts_; dst++) {
    if (dst == host_id_) continue;
    PendingRing *ring = &rings_->rings[host_id_][dst];
    uint64_t t = local_tail_[dst]++;
    uint32_t slot = t % kPendingRingEntries;
    PendingRingEntry *e = &ring->entries[slot];

    // Wait for slot to be free (replicator clears op_id after processing).
    const int kRingWaitBudgetUs = 2000000;
    uint64_t start = now_ns();
    for (;;) {
      if (CACHELINE_LOAD(&e->op_id) == 0) break;
      if ((now_ns() - start) / 1000 > (uint64_t)kRingWaitBudgetUs) return -3;
      __builtin_ia32_pause();
    }

    CACHELINE_STORE(&e->bucket_idx, (uint64_t)b_idx);
    CACHELINE_STORE(&e->slot_idx,   (uint64_t)s_idx);
    CACHELINE_STORE(&e->new_value_lo, value_word);
    CACHELINE_STORE(&e->new_value_hi, 0ULL);
    CACHELINE_STORE(&e->processed_op_id, 0ULL);
    store_fence();
    CACHELINE_STORE(&e->op_id, op_id);
    CACHELINE_STORE(&ring->tail, (uint64_t)(t + 1));
  }
  return 0;
}

int CxlKvStoreB::insert(uint64_t key, uint64_t value) {
  if (key == kEmptyKey) return -1;
  uint32_t idx = bucket_idx(key);
  lock_table_.lock(idx, host_id_, num_hosts_);

  CxlKvBucket *b = &buckets_[idx];
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) flush_line(&b->slots[s].key);
  full_fence();

  int empty_idx = -1;
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
    uint64_t k = b->slots[s].key;
    if (k == key) { lock_table_.unlock(idx, host_id_); return -2; }
    if (empty_idx < 0 && k == kEmptyKey) empty_idx = s;
  }
  if (empty_idx < 0) { lock_table_.unlock(idx, host_id_); return -1; }

  publish_slot(&b->slots[empty_idx], key, value);
  int rc = dispatch_nowait(idx, (uint32_t)empty_idx, value);
  bump_epoch(lock_table_.entry(idx));
  lock_table_.unlock(idx, host_id_);
  return rc;
}

int CxlKvStoreB::update(uint64_t key, uint64_t value) {
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

  int rc = dispatch_nowait(idx, (uint32_t)match, value);
  bump_epoch(lock_table_.entry(idx));
  lock_table_.unlock(idx, host_id_);
  return rc;
}

int CxlKvStoreB::remove(uint64_t key) {
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
  int rc = dispatch_nowait(idx, (uint32_t)match, 0ULL);
  bump_epoch(lock_table_.entry(idx));
  lock_table_.unlock(idx, host_id_);
  return rc;
}

int CxlKvStoreB::search(uint64_t key, uint64_t *out) const {
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

void CxlKvStoreB::replicator_loop() {
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
        if (op_id == 0) break;

        // Simulated apply — touch the source-side entry.
        (void)CACHELINE_LOAD(&e->new_value_lo);

        // B: no writer waits on processed_op_id, so we clear op_id directly.
        CACHELINE_STORE(&e->op_id, 0ULL);
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
