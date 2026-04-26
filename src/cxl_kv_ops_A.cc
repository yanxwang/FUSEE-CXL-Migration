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

#include "cxl_latency_decomp_probe.h"

// iter-1A: per-stage decomp for protocol A's write path. Reuses the C
// enum slots (no enum widening), with this protocol-specific mapping:
//   kDecompStageLock     S1 lock_acquire    BucketLockTable::lock returns
//   kDecompStageScan     S2 local_apply     7-slot scan + slot write + flush
//   kDecompStagePublish  S3 broadcast       enqueue to N-1 peers + sfence
//   kDecompStageEpoch    S4 ack_wait        spin on N-1 ACKs
//   kDecompStageUnlock   S5 epoch+release   bump_epoch + ring slot clear + unlock
//   kDecompStageTotal    end-to-end
// When -DFUSEE_LATENCY_DECOMP=0 (default), all macros expand to ((void)0).
#if defined(FUSEE_LATENCY_DECOMP) && FUSEE_LATENCY_DECOMP
#include <time.h>
namespace {
inline uint64_t decomp_now_ns_a() {
  timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
} // namespace
#define DECOMP_DECL(name) uint64_t name = decomp_now_ns_a()
#define DECOMP_REC(stage, a, b) ::fusee::decomp_record(::fusee::stage, (b) - (a))
#else
#define DECOMP_DECL(name) ((void)0)
#define DECOMP_REC(stage, a, b) ((void)0)
#endif

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
  // iter-1A: always reserve PerHostOutMatrix region tail; the structure
  // is small (~4 MB) and only consumed when FUSEE_PER_HOST_RING=1.
  size_t after_pending = align_up(after_buckets + pending_ring_matrix_bytes(), 64);
  return after_pending + per_host_out_matrix_bytes();
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

  // iter-1A Solution 1 data structure (opt-in via FUSEE_PER_HOST_RING=1).
  // The matrix is allocated unconditionally so a future build can flip the
  // env without re-attach; producer/consumer integration is NOT in this
  // iter — see iter1A summary §"Phase 5 status" for what is + is not wired.
  size_t after_pending =
      align_up(after_buckets + pending_ring_matrix_bytes(), 64);
  per_host_rings_ = reinterpret_cast<PerHostOutMatrix *>(base + after_pending);
  {
    const char *e = getenv("FUSEE_PER_HOST_RING");
    per_host_rings_enabled_ = (e && e[0] == '1');
    // CxlKvStoreA::attach receives `num_hosts` = total_workers (per-client
    // ring topology); the *physical* host count comes from the runner's
    // FUSEE_NUM_HOSTS env. The PerHostOutMatrix is sized by physical hosts.
    int phys_hosts = 1;
    if (const char *nh = getenv("FUSEE_NUM_HOSTS")) {
      int v = atoi(nh);
      if (v >= 1) phys_hosts = v;
    }
    if (per_host_rings_enabled_ && phys_hosts > kMaxPhysicalHosts) {
      fprintf(stderr,
              "FUSEE_PER_HOST_RING=1 requires FUSEE_NUM_HOSTS (%d) <= "
              "kMaxPhysicalHosts (%d); falling back to legacy path\n",
              phys_hosts, kMaxPhysicalHosts);
      per_host_rings_enabled_ = false;
    }
  }

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
    // Zero the per-host out matrix.
    std::memset(per_host_rings_, 0, per_host_out_matrix_bytes());
    flush_region(per_host_rings_, per_host_out_matrix_bytes());
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

void CxlKvStoreA::enable_same_host_bypass(DramInvalMatrix *mat,
                                          int num_clients_per_host) {
  if (!mat || num_clients_per_host <= 1) {
    dram_mat_ = nullptr;
    num_clients_per_host_ = 0;
    return;
  }
  if (num_clients_per_host > kSameHostMaxClients) {
    fprintf(stderr, "enable_same_host_bypass: num_clients_per_host=%d > kSameHostMaxClients=%d\n",
            num_clients_per_host, kSameHostMaxClients);
    std::abort();
  }
  dram_mat_ = mat;
  num_clients_per_host_ = num_clients_per_host;
  my_host_ = host_id_ / num_clients_per_host;
  my_cid_in_host_ = host_id_ % num_clients_per_host;
  physical_hosts_ = num_hosts_ / num_clients_per_host;
  for (int i = 0; i < kSameHostMaxClients; i++) dram_local_tail_[i] = 0;
}

void CxlKvStoreA::stop() {
  if (replicator_.joinable()) {
    stop_.store(true, std::memory_order_relaxed);
    replicator_.join();
  }
}

static inline void publish_slot(CxlKvSlot *slot, uint64_t key, uint64_t value) {
  // 2g: slot is 16 B in a 64 B cacheline — one clflushopt publishes the pair
  // atomically. x86 TSO orders the two stores; clflushopt is ordered with
  // prior stores to the same line. The single sfence after the dispatch
  // loop (for A) and bump_epoch (for both) is enough.
  slot->value = value;
  slot->key = key;
  flush_line(slot);
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
  DECOMP_DECL(__dispatch_t0);  // S3 begins (broadcast)
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
  // Track per-dst same-host queue tails we pushed into (for DRAM ACK wait).
  uint64_t dram_my_t[kSameHostMaxClients];
  bool     dram_pushed[kSameHostMaxClients];
  for (int i = 0; i < kSameHostMaxClients; i++) {
    dram_my_t[i] = 0; dram_pushed[i] = false;
  }

  for (int dst = 0; dst < num_hosts_; dst++) {
    if (dst == host_id_) continue;

    // Same-host bypass: peer lives on our physical host → push via DRAM.
    if (dram_mat_ && num_clients_per_host_ > 1 &&
        (dst / num_clients_per_host_) == my_host_) {
      int dst_cid = dst % num_clients_per_host_;
      uint64_t t = dram_local_tail_[dst_cid]++;
      DramInvalQueue *q =
          &dram_mat_->rings[my_cid_in_host_][dst_cid];
      DramInvalEntry *e = &q->entries[t % kSameHostQueueDepth];
      while (e->op_id.load(std::memory_order_acquire) != 0) {
        __builtin_ia32_pause();
      }
      e->bucket_idx = (uint64_t)b_idx;
      e->processed_op_id.store(0, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_release);
      e->op_id.store(op_id, std::memory_order_release);
      q->tail.store(t + 1, std::memory_order_release);
      dram_my_t[dst_cid] = t;
      dram_pushed[dst_cid] = true;
      continue;
    }

    // Cross-host: CXL ring.
    PendingRing *ring = &rings_->rings[host_id_][dst];
    uint64_t t = local_tail_[dst]++;
    uint32_t slot = t % kPendingRingEntries;
    PendingRingEntry *e = &ring->entries[slot];

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

    // 2b: no processed_op_id reset — op_ids are globally unique (host-ns
    // prefix), so a stale processed_op_id cannot match our current op_id.
    // No per-peer sfence on the payload/tail below; the single sfence
    // after the dispatch loop drains before the ACK wait begins.
    e->prod.bucket_idx = (uint64_t)b_idx;
    e->prod.slot_idx   = (uint64_t)s_idx;
    e->prod.new_value  = value_word;
    compiler_barrier();
    e->prod.op_id      = op_id;
    compiler_barrier();
    flush_line((void *)&e->prod);
    ring->tail.value = (uint64_t)(t + 1);
    flush_line((void *)&ring->tail.value);

    my_entries[dst] = e;
  }
  // 2b: one sfence after the dispatch loop drains all per-peer clflushopts
  // before we spin on ACK. Peers that poll tail first simply see op_id==0
  // and retry; this sfence bounds the window.
  store_fence();
  DECOMP_DECL(__dispatch_t1);  // S3 ends, S4 begins (ack_wait)

  // Spin on processed_op_id == op_id only for SAME-GROUP peers. Under
  // FUSEE_A_GROUPS=1 (default), this is every peer (original all-sync
  // semantics). With K>1, writer returns after N/K - 1 ACKs instead of
  // N-1.
  const int kAckWaitBudgetUs = 200000; // 200 ms per dst
  bool timed_out = false;

  // CXL peers' ACKs.
  for (int dst = 0; dst < num_hosts_; dst++) {
    if (dst == host_id_ || !my_entries[dst]) continue;
    if ((dst % a_groups) != my_group) continue;
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

  // Same-host DRAM peers' ACKs.
  if (dram_mat_ && num_clients_per_host_ > 1) {
    for (int dst_cid = 0; dst_cid < num_clients_per_host_; dst_cid++) {
      if (!dram_pushed[dst_cid]) continue;
      // Group-sync: only wait for same-group same-host peers.
      int peer_gid = my_host_ * num_clients_per_host_ + dst_cid;
      if ((peer_gid % a_groups) != my_group) continue;
      DramInvalQueue *q = &dram_mat_->rings[my_cid_in_host_][dst_cid];
      DramInvalEntry *e = &q->entries[dram_my_t[dst_cid] % kSameHostQueueDepth];
      uint64_t start = now_ns();
      for (;;) {
        if (e->processed_op_id.load(std::memory_order_acquire) == op_id) break;
        if ((now_ns() - start) / 1000 > (uint64_t)kAckWaitBudgetUs) {
          timed_out = true;
          break;
        }
        __builtin_ia32_pause();
      }
    }
  }

  // Clear CXL ring slots we published (so the slot can be reused).
  // 2b: one sfence covers all per-peer slot clears. bump_epoch's
  // CACHELINE_STORE in the caller provides a second global sfence for
  // readers — both are adequate to order these clears.
  bool any_cleared = false;
  for (int dst = 0; dst < num_hosts_; dst++) {
    if (dst == host_id_ || !my_entries[dst]) continue;
    my_entries[dst]->prod.op_id = 0;
    compiler_barrier();
    flush_line((void *)&my_entries[dst]->prod);
    any_cleared = true;
  }
  if (any_cleared) store_fence();
  // DRAM slots: consumer clears op_id after processing; no writer-side
  // release needed.

  DECOMP_DECL(__dispatch_t2);  // S4 ends (slot clear is part of S4)
  DECOMP_REC(kDecompStagePublish, __dispatch_t0, __dispatch_t1);  // S3 broadcast
  DECOMP_REC(kDecompStageEpoch,   __dispatch_t1, __dispatch_t2);  // S4 ack_wait
  return timed_out ? -4 : 0;
}

int CxlKvStoreA::insert(uint64_t key, uint64_t value) {
  if (read_only_) {
    fprintf(stderr, "CxlKvStoreA::insert called on read-only attach\n");
    std::abort();
  }
  if (key == kEmptyKey) return -1;
  uint32_t idx = bucket_idx(key);
  DECOMP_DECL(__dt0);
  lock_table_.lock(idx, host_id_, num_hosts_);
  DECOMP_DECL(__dt1);

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
  DECOMP_DECL(__dt2);

  int rc = dispatch_and_wait(idx, (uint32_t)empty_idx, value);
  DECOMP_DECL(__dt4);
  // Bump the reader-visible epoch regardless so seqlock readers pick it up
  // even if replication ACK timed out (they still see the authoritative slot).
  bump_epoch(lock_table_.entry(idx));
  if (logged) oplog_->commit(log_idx);
  if (cache_enabled_) {
    cache_epoch_[idx].store(std::numeric_limits<uint64_t>::max(),
                            std::memory_order_release);
  }
  lock_table_.unlock(idx, host_id_);
  DECOMP_DECL(__dt5);
  DECOMP_REC(kDecompStageLock,    __dt0, __dt1);
  DECOMP_REC(kDecompStageScan,    __dt1, __dt2);
  DECOMP_REC(kDecompStageUnlock,  __dt4, __dt5);
  DECOMP_REC(kDecompStageTotal,   __dt0, __dt5);
  return rc;
}

int CxlKvStoreA::update(uint64_t key, uint64_t value) {
  if (read_only_) {
    fprintf(stderr, "CxlKvStoreA::update called on read-only attach\n");
    std::abort();
  }
  if (key == kEmptyKey) return -1;
  uint32_t idx = bucket_idx(key);
  DECOMP_DECL(__dt0);
  lock_table_.lock(idx, host_id_, num_hosts_);
  DECOMP_DECL(__dt1);

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
  DECOMP_DECL(__dt2);
  // 2b: drop intermediate sfence. dispatch_and_wait starts with a
  // full_fence'd slot-free spin and ends with one sfence before the ACK wait.

  int rc = dispatch_and_wait(idx, (uint32_t)match, value);
  DECOMP_DECL(__dt4);  // dispatch_and_wait covers S3+S4 internally; we split
                       // in the dispatch helper via __dt2..__dt4.
  bump_epoch(lock_table_.entry(idx));
  if (logged) oplog_->commit(log_idx);
  if (cache_enabled_) {
    cache_epoch_[idx].store(std::numeric_limits<uint64_t>::max(),
                            std::memory_order_release);
  }
  lock_table_.unlock(idx, host_id_);
  DECOMP_DECL(__dt5);
  DECOMP_REC(kDecompStageLock,    __dt0, __dt1);  // S1 lock_acquire
  DECOMP_REC(kDecompStageScan,    __dt1, __dt2);  // S2 local_apply
  // S3 broadcast and S4 ack_wait are recorded inside dispatch_and_wait
  // (split point passed via this call's elapsed time below).
  DECOMP_REC(kDecompStageUnlock,  __dt4, __dt5);  // S5 epoch+release+unlock
  DECOMP_REC(kDecompStageTotal,   __dt0, __dt5);  // end-to-end
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
  // 2b: drop intermediate sfence (same as update).

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

    // CXL rings: consume from cross-host peers only. With bypass off we
    // consume from all peers (original Phase 4 behavior).
    for (int src = 0; src < num_hosts_; src++) {
      if (src == host_id_) continue;
      if (dram_mat_ && num_clients_per_host_ > 1 &&
          (src / num_clients_per_host_) == my_host_) {
        continue;  // same-host peer uses DRAM queue
      }
      PendingRing *ring = &rings_->rings[src][host_id_];
      uint64_t tail = CACHELINE_LOAD(&ring->tail);
      uint64_t head = CACHELINE_LOAD(&ring->head);
      while (head < tail) {
        uint32_t slot = head % kPendingRingEntries;
        PendingRingEntry *e = &ring->entries[slot];
        flush_line((void *)&e->prod);
        full_fence();
        uint64_t op_id = e->prod.op_id;
        if (op_id == 0) break;
        uint64_t bucket_hit = e->prod.bucket_idx;
        (void)e->prod.new_value;
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

    // DRAM queues: consume from same-host peers only.
    if (dram_mat_ && num_clients_per_host_ > 1) {
      for (int src_cid = 0; src_cid < num_clients_per_host_; src_cid++) {
        if (src_cid == my_cid_in_host_) continue;
        DramInvalQueue *q = &dram_mat_->rings[src_cid][my_cid_in_host_];
        uint64_t head = q->head.load(std::memory_order_relaxed);
        uint64_t tail = q->tail.load(std::memory_order_acquire);
        while (head < tail) {
          DramInvalEntry *e = &q->entries[head % kSameHostQueueDepth];
          uint64_t op_id = e->op_id.load(std::memory_order_acquire);
          if (op_id == 0) break;
          uint64_t bucket_hit = e->bucket_idx;
          if (cache_enabled_ && bucket_hit < num_buckets_) {
            cache_epoch_[bucket_hit].store(
                std::numeric_limits<uint64_t>::max(),
                std::memory_order_release);
          }
          e->processed_op_id.store(op_id, std::memory_order_release);
          // Free slot so producer can reuse (DRAM equivalent of op_id=0 clear).
          e->op_id.store(0, std::memory_order_release);
          head++;
          replicated_ops_.fetch_add(1, std::memory_order_relaxed);
          did_work = true;
        }
        q->head.store(head, std::memory_order_release);
      }
    }

    if (!did_work) __builtin_ia32_pause();
  }
}

} // namespace fusee
