#include "cxl_kv_ops_B.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>

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
                        bool init_region, bool read_only) {
  if (!region_base || num_buckets == 0) return -1;
  if (num_hosts > kMaxHosts) return -1;
  if (region_bytes < bytes_for(num_buckets)) return -1;

  num_buckets_ = num_buckets;
  host_id_ = host_id;
  num_hosts_ = num_hosts;
  read_only_ = read_only;

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
  if (!read_only_) {
    replicator_ = std::thread(&CxlKvStoreB::replicator_loop, this);
  }
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
  if (recovery_mode_) {
    (void)b_idx; (void)s_idx; (void)value_word;
    return 0;
  }
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
  if (read_only_) {
    fprintf(stderr, "CxlKvStoreB::insert called on read-only attach\n");
    std::abort();
  }
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

  uint64_t log_idx = 0;
  bool logged = false;
  if (oplog_) {
    log_idx = oplog_->begin(OpLogKind::Insert, key, idx, (uint64_t)empty_idx, 0, value);
    logged = true;
  }

  publish_slot(&b->slots[empty_idx], key, value);
  int rc = dispatch_nowait(idx, (uint32_t)empty_idx, value);
  bump_epoch(lock_table_.entry(idx));
  if (logged) oplog_->commit(log_idx);
  if (cache_enabled_) {
    cache_epoch_[idx].store(std::numeric_limits<uint64_t>::max(),
                            std::memory_order_release);
  }
  lock_table_.unlock(idx, host_id_);
  return rc;
}

int CxlKvStoreB::update(uint64_t key, uint64_t value) {
  if (read_only_) {
    fprintf(stderr, "CxlKvStoreB::update called on read-only attach\n");
    std::abort();
  }
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

  uint64_t log_idx = 0;
  bool logged = false;
  if (oplog_) {
    log_idx = oplog_->begin(OpLogKind::Update, key, idx, (uint64_t)match,
                            b->slots[match].value, value);
    logged = true;
  }

  b->slots[match].value = value;
  flush_line(&b->slots[match].value);
  store_fence();

  int rc = dispatch_nowait(idx, (uint32_t)match, value);
  bump_epoch(lock_table_.entry(idx));
  if (logged) oplog_->commit(log_idx);
  if (cache_enabled_) {
    cache_epoch_[idx].store(std::numeric_limits<uint64_t>::max(),
                            std::memory_order_release);
  }
  lock_table_.unlock(idx, host_id_);
  return rc;
}

int CxlKvStoreB::remove(uint64_t key) {
  if (read_only_) {
    fprintf(stderr, "CxlKvStoreB::remove called on read-only attach\n");
    std::abort();
  }
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

  uint64_t log_idx = 0;
  bool logged = false;
  if (oplog_) {
    log_idx = oplog_->begin(OpLogKind::Delete, key, idx, (uint64_t)match,
                            b->slots[match].value, 0);
    logged = true;
  }

  b->slots[match].key = kEmptyKey;
  flush_line(&b->slots[match].key);
  store_fence();
  int rc = dispatch_nowait(idx, (uint32_t)match, 0ULL);
  bump_epoch(lock_table_.entry(idx));
  if (logged) oplog_->commit(log_idx);
  if (cache_enabled_) {
    cache_epoch_[idx].store(std::numeric_limits<uint64_t>::max(),
                            std::memory_order_release);
  }
  lock_table_.unlock(idx, host_id_);
  return rc;
}

int CxlKvStoreB::search(uint64_t key, uint64_t *out) const {
  if (key == kEmptyKey) return -1;
  uint32_t idx = bucket_idx(key);
  BucketLockEntry *le = const_cast<BucketLockTable &>(lock_table_).entry(idx);
  CxlKvBucket *b = &buckets_[idx];

  // Fast path: DRAM cache, no CXL load. Replicator invalidates cache_epoch_
  // to UINT64_MAX when a peer writer pushes through the ring.
  if (cache_enabled_) {
    uint64_t cached = cache_epoch_[idx].load(std::memory_order_acquire);
    if (cached != std::numeric_limits<uint64_t>::max()) {
      const CxlKvBucket &cb = cache_buckets_[idx];
      for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
        if (cb.slots[s].key == key) {
          if (out) *out = cb.slots[s].value;
          return 0;
        }
      }
      return -1;
    }
  }

  for (int attempt = 0; attempt < 8; attempt++) {
    uint64_t e1 = CACHELINE_LOAD(&le->write_epoch);

    bool found = false;
    uint64_t captured = 0;
    for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
      flush_line(&b->slots[s].key);
      flush_line(&b->slots[s].value);
    }
    full_fence();
    if (cache_enabled_) {
      cache_buckets_[idx] = *b;
    }
    for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
      if (b->slots[s].key == key) {
        captured = b->slots[s].value;
        found = true;
        break;
      }
    }

    uint64_t e2 = CACHELINE_LOAD(&le->write_epoch);
    if (e1 == e2) {
      if (cache_enabled_) cache_epoch_[idx].store(e1, std::memory_order_release);
      if (found) { if (out) *out = captured; return 0; }
      return -1;
    }
  }
  return -1;
}

uint64_t CxlKvStoreB::recover_from_oplog() {
  if (!oplog_) return 0;
  OpLog *saved = oplog_;
  oplog_ = nullptr;
  recovery_mode_ = true;

  auto redo = +[](const OpLogEntry *e, void *user) -> int {
    auto *self = reinterpret_cast<CxlKvStoreB *>(user);
    int rc = 0;
    switch (e->kind) {
      case OpLogKind::Insert:
        rc = self->insert(e->key, e->new_value);
        if (rc == -2) rc = 0;
        break;
      case OpLogKind::Update:
        rc = self->update(e->key, e->new_value);
        if (rc == -1) {
          rc = self->insert(e->key, e->new_value);
          if (rc == -2) rc = 0;
        }
        break;
      case OpLogKind::Delete:
        rc = self->remove(e->key);
        if (rc == -1) rc = 0;
        break;
      default:
        rc = -1;
    }
    return rc;
  };

  uint64_t acted = saved->recover_redo(redo, this);

  recovery_mode_ = false;
  oplog_ = saved;
  return acted;
}

void CxlKvStoreB::enable_dram_cache(bool on) {
  // Race fix 2026-04-22 (same as CxlKvStoreA): allocate cache storage
  // before flipping cache_enabled_.
  if (on) {
    cache_buckets_.assign(num_buckets_, CxlKvBucket{});
    std::vector<std::atomic<uint64_t>> tmp(num_buckets_);
    for (auto &a : tmp) a.store(std::numeric_limits<uint64_t>::max(),
                                std::memory_order_relaxed);
    cache_epoch_ = std::move(tmp);
    std::atomic_thread_fence(std::memory_order_release);
    cache_enabled_ = on;
  } else {
    cache_enabled_ = on;
    std::atomic_thread_fence(std::memory_order_release);
    cache_buckets_.clear();
    cache_epoch_.clear();
  }
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

        // Read the bucket_idx so we can invalidate our DRAM cache.
        uint64_t bucket_hit = CACHELINE_LOAD(&e->bucket_idx);
        if (cache_enabled_ && bucket_hit < num_buckets_) {
          cache_epoch_[bucket_hit].store(
              std::numeric_limits<uint64_t>::max(),
              std::memory_order_release);
        }

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
