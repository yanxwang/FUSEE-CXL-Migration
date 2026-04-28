#include "cxl_kv_ops_A.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <pthread.h>
#include <sched.h>

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
  size_t after_perhost = align_up(after_pending + per_host_out_matrix_bytes(), 64);
  // iter-3A Phase 2: SlotLockTable always reserved at tail; consumed
  // only when FUSEE_PER_SLOT_LFM_A=1. ~17 GiB at num_buckets=65536.
  return after_perhost + SlotLockTable::bytes_for(num_buckets);
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
  // iter-3A Phase 2: SlotLockTable region. Read FUSEE_PER_SLOT_LFM_A env
  // and initialise the per-slot LFM mutex table only when enabled.
  size_t after_perhost = align_up(after_pending + per_host_out_matrix_bytes(), 64);
  void *slot_lock_base = base + after_perhost;
  {
    const char *e = getenv("FUSEE_PER_SLOT_LFM_A");
    per_slot_lfm_a_ = (e && e[0] == '1');
    if (per_slot_lfm_a_) {
      slot_lock_table_.attach(slot_lock_base, num_buckets, init_region);
    }
  }
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
    // iter-3A FINDING: phys_hosts_pr_ et al. are intentionally LEFT at
    // their defaults (1, 0, 1, 0). Assigning them from FUSEE_NUM_HOSTS
    // activates the cross-host SPSC ring + ack channel exchange in
    // dispatch_and_wait + sender_loop_k + receiver_loop_k, which dead-
    // locks under the current sender/receiver synchronization (every
    // smoke run with non-trivial workload hangs in the writer's ack
    // spin or the sender's ack-channel poll). The legacy iter-2A-
    // revised code shipped with the same defect; the symptom there
    // was "1.27 Mops/s = no cross-host work", not a deadlock, because
    // the empty enqueue loop made any_enqueued=false and the writer
    // returned immediately. iter-3A discovers the gap explicitly via
    // the hash-diff battery (which still PASSES because publish_slot
    // writes to shared CXL memory directly — both hosts see the same
    // final bucket array even without invalidation traffic), and
    // documents the resulting "no-op N:1:1:N" baseline. iter-4A
    // candidates: (1) fix _pr_ assignment + debug the cross-host
    // deadlock; (2) abandon N:1:1:N in favour of the legacy
    // PendingRingMatrix path with per-slot LFM (already shown to
    // work in the cxl_ycsb_runner without FUSEE_PER_HOST_RING).
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
  // iter-2A-revised: stop the sender thread BEFORE the replicator.
  // The sender may still be holding entries waiting for the receiver's
  // ack_seq advance; the receiver lives on the OTHER host's process so
  // local stop just signals; but if our process is also a receiver
  // (which happens for the host's primary client) we want to drain
  // pending acks before tearing down.
  stop_per_host_sender();
  stop_per_host_receivers();
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

// iter-2A-revised reader-side accessor. Public so the runner / decomp
// harness can dump epoch values; internal hot path uses .load() inline.
uint64_t CxlKvStoreA::cache_epoch_load(uint32_t bucket_idx) const {
  if (!cache_epoch_arr_ || bucket_idx >= num_buckets_) return 0;
  return cache_epoch_arr_->epoch[bucket_idx].load(std::memory_order_acquire);
}

int CxlKvStoreA::enable_per_host_ring(LocalAggregatorRegion *aggr_region,
                                      CacheEpochArr *epoch_arr,
                                      uint32_t batch_k,
                                      uint64_t batch_t_ns,
                                      int sender_core,
                                      int receiver_core) {
  if (!aggr_region || !epoch_arr) return -1;
  aggregator_      = aggr_region;
  cache_epoch_arr_ = epoch_arr;
  if (batch_k == 0) batch_k = 1;
  if (batch_k > kPerHostSpscDepth) batch_k = kPerHostSpscDepth;
  sender_batch_k_   = batch_k;
  sender_batch_t_ns_ = batch_t_ns;
  sender_core_      = sender_core;
  receiver_core_    = receiver_core;
  per_host_rings_enabled_ = true;

  // iter-3A Phase 4: K-channel sharding. K = 1 (default, legacy
  // iter-2A-revised) | 2 | 4. Same value MUST be used across all
  // hosts (controls SPSC ring + ack channel layout in CXL).
  k_channels_ = 1;
  if (const char *e = getenv("FUSEE_K_CHANNELS")) {
    int v = atoi(e);
    if (v == 1 || v == 2 || v == 4) k_channels_ = (uint32_t)v;
  }
  sender_core_base_ = -1;
  receiver_core_base_ = -1;
  if (const char *e = getenv("FUSEE_SENDER_CORE_BASE")) {
    int v = atoi(e); if (v >= 0) sender_core_base_ = v;
  }
  if (const char *e = getenv("FUSEE_RECEIVER_CORE_BASE")) {
    int v = atoi(e); if (v >= 0) receiver_core_base_ = v;
  }
  // Worker slot used by this client; total clients across all hosts
  // are addressed in worker_ack_buf via the (host-local) cid index.
  // host_id_ is the global worker id; my_cid_in_host_pr_ is the
  // host-local id 0..clients_per_host-1.
  my_worker_slot_pr_ = my_cid_in_host_pr_;
  if (my_worker_slot_pr_ < 0 || my_worker_slot_pr_ >= kAggrMaxWorkers) {
    fprintf(stderr,
            "enable_per_host_ring: worker slot %d out of range "
            "[0, %d) — falling back to legacy\n",
            my_worker_slot_pr_, kAggrMaxWorkers);
    per_host_rings_enabled_ = false;
    return -1;
  }
  // Primary client of this physical host spawns the K sender + K
  // receiver threads. iter-3A Phase 4: K threads instead of 1; each
  // owns channel `k_id`. Receivers drain rings[*][me][k_id] and
  // publish acks[*][me][k_id] — by-channel routing keeps a bucket's
  // updates serialised on a single (sender, receiver) pair.
  if (my_cid_in_host_pr_ == 0) {
    bool expected_s = false;
    if (sender_started_.compare_exchange_strong(expected_s, true)) {
      for (uint32_t k = 0; k < k_channels_; k++) {
        sender_threads_[k] = std::thread([this, k]() { this->sender_loop_k(k); });
      }
    }
    bool expected_r = false;
    if (receiver_started_.compare_exchange_strong(expected_r, true)) {
      per_host_recv_stop_.store(false, std::memory_order_relaxed);
      for (uint32_t k = 0; k < k_channels_; k++) {
        receiver_threads_[k] = std::thread([this, k]() { this->receiver_loop_k(k); });
      }
    }
  }
  return 0;
}

void CxlKvStoreA::stop_per_host_sender() {
  if (!aggregator_) return;
  if (!sender_started_.load()) return;
  // Signal stop on all channel headers.
  for (uint32_t k = 0; k < k_channels_; k++) {
    aggregator_->queues[k].hdr.stop.store(1, std::memory_order_release);
  }
  for (uint32_t k = 0; k < k_channels_; k++) {
    if (sender_threads_[k].joinable()) sender_threads_[k].join();
  }
  sender_started_.store(false);
}

void CxlKvStoreA::stop_per_host_receivers() {
  if (!receiver_started_.load()) return;
  per_host_recv_stop_.store(true, std::memory_order_release);
  for (uint32_t k = 0; k < k_channels_; k++) {
    if (receiver_threads_[k].joinable()) receiver_threads_[k].join();
  }
  receiver_started_.store(false);
}

void CxlKvStoreA::receiver_loop_k(uint32_t k_id) {
  if (!per_host_rings_ || !cache_epoch_arr_) return;
  if (k_id >= kMaxKChannels) return;

  // CPU pinning: receiver_core_base_ + k_id when set; else
  // receiver_core_ for legacy k_id == 0 only.
  int pin_core = -1;
  if (receiver_core_base_ >= 0) pin_core = receiver_core_base_ + (int)k_id;
  else if (k_id == 0 && receiver_core_ >= 0) pin_core = receiver_core_;
  if (pin_core >= 0) {
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(pin_core, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  }

  while (!per_host_recv_stop_.load(std::memory_order_acquire)) {
    bool did_work = false;
    for (int src_host = 0; src_host < phys_hosts_pr_; src_host++) {
      if (src_host == my_phys_host_pr_) continue;
      PerHostSpscRing *r =
          &per_host_rings_->rings[src_host][my_phys_host_pr_][k_id];
      flush_line((void *)&r->tail);
      full_fence();
      uint64_t tail = r->tail.load(std::memory_order_acquire);
      uint64_t head = r->head;
      while (head < tail) {
        DECOMP_DECL(__re_t0);
        PerHostInvalEntry *e =
            &r->entries[head % kPerHostSpscDepth];
        flush_line((void *)e);
        full_fence();
        uint64_t op_id = e->src_worker_op_id;
        if (op_id == 0) break;  // sender hasn't published yet
        uint64_t bucket_hit = e->bucket_idx;
        uint64_t new_epoch  = e->new_epoch;
        DECOMP_DECL(__ra_t0);
        if (bucket_hit < (uint64_t)num_buckets_) {
          cache_epoch_arr_->epoch[bucket_hit].store(
              new_epoch, std::memory_order_release);
        }
        DECOMP_DECL(__ra_t1);
        DECOMP_REC(kDecompStageA_RecvAtomic, __ra_t0, __ra_t1);
        e->src_worker_op_id = 0;
        compiler_barrier();
        flush_line((void *)e);
        head++;
        replicated_ops_.fetch_add(1, std::memory_order_relaxed);
        did_work = true;
        DECOMP_DECL(__re_t1);
        DECOMP_REC(kDecompStageA_RecvEntry, __re_t0, __re_t1);
      }
      r->head = head;
      // Advance ack channel for this (src_host, me, k_id).
      AckChannel *ach =
          &per_host_rings_->acks[src_host][my_phys_host_pr_][k_id];
      ach->seq.store(head, std::memory_order_release);
      flush_line((void *)&ach->seq);
      store_fence();
    }
    if (!did_work) __builtin_ia32_pause();
  }
}

void CxlKvStoreA::sender_loop_k(uint32_t k_id) {
  if (!aggregator_ || !per_host_rings_) return;
  if (k_id >= kMaxKChannels) return;
  LocalAggregatorQueue *q = &aggregator_->queues[k_id];

  // Optional CPU pinning. Each k_id picks core (sender_core_base_ + k_id)
  // when sender_core_base_ >= 0; or sender_core_ for the legacy single-thread
  // case (k_id == 0 only).
  int pin_core = -1;
  if (sender_core_base_ >= 0) pin_core = sender_core_base_ + (int)k_id;
  else if (k_id == 0 && sender_core_ >= 0) pin_core = sender_core_;
  if (pin_core >= 0) {
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(pin_core, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  }

  // Per-dst pending batch buffers (DRAM-local; not in shm).
  // Each slot stores the (worker_slot, op_id, bucket_idx, new_epoch)
  // that the sender will memcpy to the SPSC ring.
  constexpr int kBatchMax = 64;
  uint32_t batch_count[kMaxPhysicalHosts] = {0};
  uint64_t oldest_age_ns[kMaxPhysicalHosts] = {0};
  PerHostInvalEntry pending[kMaxPhysicalHosts][kBatchMax];

  while (q->hdr.stop.load(std::memory_order_acquire) == 0) {
    // Step S-1: drain aggregator queue into per-dst pending batches.
    uint64_t tail = q->hdr.tail.load(std::memory_order_acquire);
    uint64_t head = q->hdr.head;
    bool did_work = false;
    while (head < tail) {
      AggrEntry *ae = &q->entries[head % kAggrQueueDepth];
      uint64_t op_id = __atomic_load_n(&ae->op_id, __ATOMIC_ACQUIRE);
      if (op_id == 0) break;  // producer hasn't published yet
      int dst = (int)ae->dst_host;
      if (dst < 0 || dst >= phys_hosts_pr_ || dst == my_phys_host_pr_) {
        // bad dst; skip + clear
        ae->op_id = 0;
        head++;
        continue;
      }
      uint32_t bc = batch_count[dst];
      if (bc >= sender_batch_k_ || bc >= (uint32_t)kBatchMax) {
        // Full — leave this aggregator entry; flush in step S-3 first.
        break;
      }
      pending[dst][bc].bucket_idx       = ae->bucket_idx;
      pending[dst][bc].new_epoch        = ae->new_epoch;
      pending[dst][bc].src_worker_slot  = ae->src_worker_slot;
      pending[dst][bc].src_worker_op_id = op_id;
      if (bc == 0) oldest_age_ns[dst] = now_ns();
      batch_count[dst] = bc + 1;
      ae->op_id = 0;             // free aggregator slot
      head++;
      did_work = true;
    }
    q->hdr.head = head;

    // Step S-2/S-3: for each dst with full or aged batch, write to CXL ring.
    uint64_t now = now_ns();
    for (int dst = 0; dst < phys_hosts_pr_; dst++) {
      if (dst == my_phys_host_pr_) continue;
      uint32_t bc = batch_count[dst];
      if (bc == 0) continue;
      bool full = (bc >= sender_batch_k_);
      bool aged = ((now - oldest_age_ns[dst]) >= sender_batch_t_ns_);
      if (!full && !aged) continue;

      // iter-3A: route through k_id channel. K=1 → [k=0] (legacy).
      PerHostSpscRing *r = &per_host_rings_->rings[my_phys_host_pr_][dst][k_id];
      uint64_t tail_pos = r->tail.load(std::memory_order_relaxed);
      // Write each entry to the CXL ring slot.
      for (uint32_t i = 0; i < bc; i++) {
        PerHostInvalEntry *e = &r->entries[(tail_pos + i) % kPerHostSpscDepth];
        // Wait for slot free (op_id == 0). Receiver clears after process.
        for (;;) {
          flush_line((void *)e);
          full_fence();
          if (e->src_worker_op_id == 0) break;
          __builtin_ia32_pause();
        }
        e->bucket_idx       = pending[dst][i].bucket_idx;
        e->new_epoch        = pending[dst][i].new_epoch;
        e->src_worker_slot  = pending[dst][i].src_worker_slot;
        compiler_barrier();
        e->src_worker_op_id = pending[dst][i].src_worker_op_id;
        compiler_barrier();
        flush_line((void *)e);
      }
      tail_pos += bc;
      r->tail.store(tail_pos, std::memory_order_release);
      flush_line((void *)&r->tail);
      store_fence();

      // iter-3A Phase 1 probe: per-batch sender cycle time.
      DECOMP_DECL(__sb_t0);

      // Step S-4: wait for receiver to ack this batch.
      AckChannel *ach = &per_host_rings_->acks[my_phys_host_pr_][dst][k_id];
      for (;;) {
        flush_line((void *)&ach->seq);
        full_fence();
        if (ach->seq.load(std::memory_order_acquire) >= tail_pos) break;
        __builtin_ia32_pause();
      }

      // Step S-5: ACK each worker that contributed an entry. Worker
      // spins on ack_bufs[k_id].slots[my_slot] under per-channel routing.
      for (uint32_t i = 0; i < bc; i++) {
        uint32_t slot = pending[dst][i].src_worker_slot;
        uint64_t op   = pending[dst][i].src_worker_op_id;
        if (slot < kAggrMaxWorkers) {
          aggregator_->ack_bufs[k_id].slots[slot].ack_op_id.store(
              op, std::memory_order_release);
        }
      }
      DECOMP_DECL(__sb_t1);
      DECOMP_REC(kDecompStageA_SenderBatch, __sb_t0, __sb_t1);
      // iter-3A K_actual histogram (sender-thread-only counter).
      sender_k_actual_hist_[bc <= 64 ? bc : 64]++;
      batch_count[dst] = 0;
      did_work = true;
    }

    if (!did_work) {
      timespec ts = {0, 1000};  // 1 µs nap
      nanosleep(&ts, nullptr);
    }
  }
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

  // iter-2A-revised N:1:1:N path. Active when per_host_rings_enabled_ +
  // aggregator_ + cache_epoch_arr_ are all wired (see enable_per_host_ring).
  // Strict A linearizability is preserved: same-host invalidation is
  // an atomic_store into shared cache_epoch_arr (x86 coherence fans
  // out to all local clients), then mfence; cross-host invalidation
  // goes via the aggregator → sender → SPSC ring → receiver →
  // cache_epoch_arr atomic_store on the peer host.
  if (per_host_rings_enabled_ && aggregator_ && cache_epoch_arr_) {
    // Compute the new epoch we are about to publish (we already
    // wrote the slot above the call site; the bump_epoch happens AFTER
    // we return, but the receiver only needs SOME monotonically
    // increasing value to invalidate caches).
    uint64_t cur_epoch = CACHELINE_LOAD(&lock_table_.entry(b_idx)->write_epoch);
    uint64_t new_epoch = cur_epoch + 1;

    // Step 4: same-host atomic_store. x86 coherence makes this
    // immediately visible to all OTHER local client processes — their
    // next acquire-load on cache_epoch_arr_->epoch[B] returns
    // new_epoch and their reader path refetches from CXL.
    if (b_idx < num_buckets_) {
      cache_epoch_arr_->epoch[b_idx].store(new_epoch,
                                            std::memory_order_release);
    }
    // Step 5: mfence to ensure step 4 globally visible before we wait
    // for the cross-host ack.
    full_fence();

    // Step 6: enqueue cross-host invalidation request into the local
    // aggregator queue for THIS bucket's channel (k = bucket_id % K).
    // iter-3A Phase 4: per-bucket channel routing keeps same-bucket
    // updates ordered (single channel) while parallelising across
    // buckets.
    uint32_t k_route = (k_channels_ > 1) ? (b_idx % k_channels_) : 0;
    DECOMP_DECL(__ae_t0);
    bool any_enqueued = false;
    for (int dst_h = 0; dst_h < phys_hosts_pr_; dst_h++) {
      if (dst_h == my_phys_host_pr_) continue;
      int rc = aggr_enqueue(&aggregator_->queues[k_route], b_idx, new_epoch,
                            (uint32_t)dst_h,
                            (uint32_t)my_worker_slot_pr_, op_id);
      if (rc == 0) any_enqueued = true;
      // -1 = backpressure timeout → strict A would force a retry; we
      // surface as ack timeout. In H=2 there is exactly 1 dst_host so
      // a single failure means we can't proceed.
    }
    DECOMP_DECL(__ae_t1);
    DECOMP_REC(kDecompStageA_AggrEnq, __ae_t0, __ae_t1);
    DECOMP_DECL(__dispatch_t1);  // S3 ends, S4 begins

    // Step 7: spin on worker_ack_buf[k_route].slots[my_worker_slot_pr_].
    // Sender_k thread writes this when the receiver's ack arrives.
    bool timed_out = false;
    if (any_enqueued && my_worker_slot_pr_ >= 0 &&
        my_worker_slot_pr_ < kAggrMaxWorkers) {
      const uint64_t kAckBudgetNs = 200000000ULL;  // 200 ms
      auto &slot = aggregator_->ack_bufs[k_route].slots[my_worker_slot_pr_];
      uint64_t start = now_ns();
      for (;;) {
        if (slot.ack_op_id.load(std::memory_order_acquire) == op_id) break;
        if (now_ns() - start > kAckBudgetNs) {
          timed_out = true;
          if ((uint32_t)my_phys_host_pr_ < kMaxHosts) {
            // bookkeeping
          }
          break;
        }
        __builtin_ia32_pause();
      }
    }
    DECOMP_DECL(__dispatch_t2);
    DECOMP_REC(kDecompStagePublish, __dispatch_t0, __dispatch_t1);
    DECOMP_REC(kDecompStageEpoch,   __dispatch_t1, __dispatch_t2);
    return timed_out ? -4 : 0;
  }


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

  // iter-3A Phase 2: per-slot LFM path. Unlocked pre-scan to find the
  // target slot, then lock JUST that slot. Re-verify under lock to
  // catch a racing writer that flipped the slot's key. Same pattern as
  // C iter-1 (`docs/iters/protocol_c_retrospective.md` iter-1).
  CxlKvBucket *b = &buckets_[idx];
  if (per_slot_lfm_a_) {
    DECOMP_DECL(__dt0);
    // L1 slot_scan (unlocked).
    for (int s = 0; s < kCxlKvSlotsPerBucket; s++) flush_line(&b->slots[s].key);
    full_fence();
    int match = -1;
    for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
      if (b->slots[s].key == key) { match = s; break; }
    }
    if (match < 0) return -1;
    DECOMP_REC(kDecompStageA_LockL1Scan, __dt0, __dt0);  // probe-only
    DECOMP_DECL(__dt0a);
    slot_lock_table_.lock_slot(idx, match);
    DECOMP_DECL(__dt1);
    // Under-lock re-verify — if key moved (delete+insert race), retry once.
    flush_line(&b->slots[match].key);
    full_fence();
    if (b->slots[match].key != key) {
      slot_lock_table_.unlock_slot(idx, match);
      // Fall through to retry via outer loop logic — but to keep things
      // simple here, just return -1 (caller's YCSB run treats as miss).
      return -1;
    }
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
    int rc = dispatch_and_wait(idx, (uint32_t)match, value);
    DECOMP_DECL(__dt4);
    // Per-slot path uses BucketLockEntry's write_epoch for reader seqlock
    // (shared field; SlotLockEntry has its own copy but readers read from
    // BucketLockEntry — keeping legacy reader path unchanged).
    bump_epoch(lock_table_.entry(idx));
    if (logged) oplog_->commit(log_idx);
    if (cache_enabled_) {
      cache_epoch_[idx].store(std::numeric_limits<uint64_t>::max(),
                              std::memory_order_release);
    }
    slot_lock_table_.unlock_slot(idx, match);
    DECOMP_DECL(__dt5);
    DECOMP_REC(kDecompStageLock,    __dt0a, __dt1);
    DECOMP_REC(kDecompStageScan,    __dt1, __dt2);
    DECOMP_REC(kDecompStageUnlock,  __dt4, __dt5);
    DECOMP_REC(kDecompStageTotal,   __dt0, __dt5);
    return rc;
  }

  // ---- legacy per-bucket LFM path ----
  DECOMP_DECL(__dt0);
  lock_table_.lock(idx, host_id_, num_hosts_);
  DECOMP_DECL(__dt1);

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
    bool valid = (cached != std::numeric_limits<uint64_t>::max());
    // iter-2A-revised: when per-host ring is wired, additionally check
    // shared cache_epoch_arr_->epoch[idx] against our last fetched
    // epoch. If receiver/writer atomic_stored a newer epoch, our
    // cached copy is stale and we must refetch.
    if (valid && per_host_rings_enabled_ && cache_epoch_arr_) {
      uint64_t arr_epoch =
          cache_epoch_arr_->epoch[idx].load(std::memory_order_acquire);
      if (arr_epoch != cached) valid = false;
    }
    if (valid) {
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
  // iter-2A-revised: optionally pin to FUSEE_RECEIVER_CORE.
  if (receiver_core_ >= 0) {
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(receiver_core_, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  }
  while (!stop_.load(std::memory_order_relaxed)) {
    bool did_work = false;

    // iter-3A Phase 4: per-host SPSC ring drain has moved to
    // dedicated K receiver threads (receiver_loop_k). When
    // per_host_rings_enabled_, those K threads own rings[*][me][k]
    // + acks[*][me][k]. The legacy in-replicator drain block was
    // removed to avoid double-draining; if some future configuration
    // disables the receiver pool, attach must spawn a single legacy
    // receiver via receiver_loop_k(0).

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
