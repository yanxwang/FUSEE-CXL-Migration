#include "cxl_kv_ops_A.h"

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
    // Zero the pending rings.
    std::memset(rings_, 0, pending_ring_matrix_bytes());
    flush_region(rings_, pending_ring_matrix_bytes());
    store_fence();
  }

  stop_.store(false, std::memory_order_relaxed);
  if (!read_only_) {
    // Only non-read-only attaches spawn the replicator. Read-only clients
    // do not produce or consume ring traffic; no need for a replicator.
    replicator_ = std::thread(&CxlKvStoreA::replicator_loop, this);
  }
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
  if (recovery_mode_) {
    (void)b_idx; (void)s_idx; (void)value_word;
    return 0;
  }
  // op_id = host_id in high bits + monotonic nanoseconds (unique per caller).
  uint64_t op_id =
      ((uint64_t)(host_id_ + 1) << 56) | (now_ns() & 0x00FFFFFFFFFFFFFFULL);

  // Phase 5: hierarchical replication. FUSEE_A_GROUPS=K (default 1 = old
  // all-sync behavior) splits the total_workers into K groups; writer
  // still pushes to every peer (broadcast) but only waits for ACKs from
  // same-group peers. Reads inter-group are eventually consistent. Read
  // from env once per process at first use.
  static const int a_groups = [&]() {
    const char *e = getenv("FUSEE_A_GROUPS");
    if (!e || e[0] == '\0') return 1;
    int v = atoi(e);
    return (v < 1) ? 1 : v;
  }();
  const int my_group = host_id_ % a_groups;

  PendingRingEntry *my_entries[kMaxHosts] = {nullptr, nullptr, nullptr, nullptr};

  for (int dst = 0; dst < num_hosts_; dst++) {
    if (dst == host_id_) continue;
    PendingRing *ring = &rings_->rings[host_id_][dst];
    uint64_t t = local_tail_[dst]++;
    uint32_t slot = t % kPendingRingEntries;
    PendingRingEntry *e = &ring->entries[slot];

    // Wait for this ring slot to be free (op_id == 0). flush_line first
    // so we don't spin on a stale local copy.
    const int kRingWaitBudgetUs = 2000000; // 2 s sanity ceiling
    uint64_t start = now_ns();
    for (;;) {
      flush_line((void *)&e->prod);
      full_fence();
      if (e->prod.op_id == 0) break;
      if ((now_ns() - start) / 1000 > (uint64_t)kRingWaitBudgetUs) {
        return -3; // ring full / replicator stuck
      }
      __builtin_ia32_pause();
    }

    // Clear consumer ACK line (separate cacheline).
    STORE_CACHELINE(&e->cons.processed_op_id, 0ULL);

    // Fill producer payload. All four fields live on one cacheline;
    // one flush+sfence at the end publishes them atomically (64 B store
    // is atomic wrt the CXL memory server). op_id written LAST so any
    // consumer that races with the store sees a coherent line.
    e->prod.bucket_idx = (uint64_t)b_idx;
    e->prod.slot_idx   = (uint64_t)s_idx;
    e->prod.new_value  = value_word;
    compiler_barrier();
    e->prod.op_id      = op_id;
    compiler_barrier();
    flush_line((void *)&e->prod);
    store_fence();

    // Publish the tail update so the consumer knows something new landed.
    CACHELINE_STORE(&ring->tail, (uint64_t)(t + 1));

    my_entries[dst] = e;
  }

  // Spin on processed_op_id == op_id only for SAME-GROUP peers. Under
  // FUSEE_A_GROUPS=1 (default), this is every peer (original all-sync
  // semantics). With K>1, writer returns after N/K - 1 ACKs instead of
  // N-1.
  const int kAckWaitBudgetUs = 200000; // 200 ms per dst
  bool timed_out = false;
  for (int dst = 0; dst < num_hosts_; dst++) {
    if (dst == host_id_ || !my_entries[dst]) continue;
    if ((dst % a_groups) != my_group) continue;   // cross-group = eager, no ACK wait
    PendingRingEntry *e = my_entries[dst];
    uint64_t start = now_ns();
    for (;;) {
      if (LOAD_CACHELINE(&e->cons.processed_op_id) == op_id) break;
      if ((now_ns() - start) / 1000 > (uint64_t)kAckWaitBudgetUs) {
        timed_out = true;
        ack_timeouts_[dst].fetch_add(1, std::memory_order_relaxed);
        break;
      }
      __builtin_ia32_pause();
    }
  }

  // Always clear op_id on every dst so the slot can be reused. Payload
  // cacheline has op_id as last u64; flush_line + sfence publishes the
  // zero to the memory server.
  for (int dst = 0; dst < num_hosts_; dst++) {
    if (dst == host_id_ || !my_entries[dst]) continue;
    my_entries[dst]->prod.op_id = 0;
    compiler_barrier();
    flush_line((void *)&my_entries[dst]->prod);
    store_fence();
  }

  return timed_out ? -4 : 0;
}

int CxlKvStoreA::insert(uint64_t key, uint64_t value) {
  if (read_only_) {
    fprintf(stderr, "CxlKvStoreA::insert called on read-only attach\n");
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

  uint64_t log_idx = 0;
  bool logged = false;
  if (oplog_) {
    log_idx = oplog_->begin(OpLogKind::Insert, key, idx, (uint64_t)empty_idx, 0, value);
    logged = true;
  }

  publish_slot(&b->slots[empty_idx], key, value);

  int rc = dispatch_and_wait(idx, (uint32_t)empty_idx, value);
  // Bump the reader-visible epoch regardless so seqlock readers pick it up
  // even if replication ACK timed out (they still see the authoritative slot).
  bump_epoch(lock_table_.entry(idx));
  if (logged) oplog_->commit(log_idx);
  if (cache_enabled_) {
    cache_epoch_[idx].store(std::numeric_limits<uint64_t>::max(),
                            std::memory_order_release);
  }
  lock_table_.unlock(idx, host_id_);
  return rc;
}

int CxlKvStoreA::update(uint64_t key, uint64_t value) {
  if (read_only_) {
    fprintf(stderr, "CxlKvStoreA::update called on read-only attach\n");
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

  int rc = dispatch_and_wait(idx, (uint32_t)match, value);
  bump_epoch(lock_table_.entry(idx));
  if (logged) oplog_->commit(log_idx);
  if (cache_enabled_) {
    cache_epoch_[idx].store(std::numeric_limits<uint64_t>::max(),
                            std::memory_order_release);
  }
  lock_table_.unlock(idx, host_id_);
  return rc;
}

int CxlKvStoreA::remove(uint64_t key) {
  if (read_only_) {
    fprintf(stderr, "CxlKvStoreA::remove called on read-only attach\n");
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

  int rc = dispatch_and_wait(idx, (uint32_t)match, 0ULL);
  bump_epoch(lock_table_.entry(idx));
  if (logged) oplog_->commit(log_idx);
  if (cache_enabled_) {
    cache_epoch_[idx].store(std::numeric_limits<uint64_t>::max(),
                            std::memory_order_release);
  }
  lock_table_.unlock(idx, host_id_);
  return rc;
}

int CxlKvStoreA::search(uint64_t key, uint64_t *out) const {
  if (key == kEmptyKey) return -1;
  uint32_t idx = bucket_idx(key);
  BucketLockEntry *le = const_cast<BucketLockTable &>(lock_table_).entry(idx);
  CxlKvBucket *b = &buckets_[idx];

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

uint64_t CxlKvStoreA::recover_from_oplog() {
  if (!oplog_) return 0;
  OpLog *saved = oplog_;
  oplog_ = nullptr;
  recovery_mode_ = true;

  auto redo = +[](const OpLogEntry *e, void *user) -> int {
    auto *self = reinterpret_cast<CxlKvStoreA *>(user);
    int rc = 0;
    switch (e->kind) {
      case OpLogKind::Insert:
        rc = self->insert(e->key, e->new_value);
        if (rc == -2) rc = 0; // duplicate treated as already-applied
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
        if (rc == -1) rc = 0; // already gone
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

void CxlKvStoreA::enable_dram_cache(bool on) {
  // Race fix 2026-04-22: replicator thread reads cache_enabled_ without
  // locking, then touches cache_epoch_[idx]. Must allocate the backing
  // storage BEFORE setting cache_enabled_ = on so the replicator either
  // sees (false, anything) or (true, allocated).
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
        // Load the producer payload cacheline atomically. One
        // flush+fence pulls all 4 fields at once.
        flush_line((void *)&e->prod);
        full_fence();
        uint64_t op_id = e->prod.op_id;
        if (op_id == 0) break; // not yet published
        uint64_t bucket_hit = e->prod.bucket_idx;
        (void)e->prod.new_value;  // simulated pull: field already in register

        // A's "synchronous invalidation" promise: invalidate the DRAM cache
        // for the bucket BEFORE publishing processed_op_id.
        if (cache_enabled_ && bucket_hit < num_buckets_) {
          cache_epoch_[bucket_hit].store(
              std::numeric_limits<uint64_t>::max(),
              std::memory_order_release);
        }

        STORE_CACHELINE(&e->cons.processed_op_id, op_id);
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
