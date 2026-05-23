#define _GNU_SOURCE
#include <sched.h>
#include <pthread.h>
#include <unistd.h>
#include <mutex>
#include <vector>

#include "cxl_kv_ops_A.h"
#include "cxl_probe.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

extern "C" {
#include "common.h"  // CACHELINE_LOAD, CACHELINE_STORE, flush_line, store_fence, full_fence
}

namespace fusee {

// iter-9A Phase 2.C — per-worker thread-local aggregator-routing id.
// iter-10A Phase 1.C — per-worker thread-local TlsCache pointer.
// Both declared at file scope (TU-level) so execute_write_local() and
// search() can read them; setters set_worker_id / set_thread_tls_cache
// live further down. -1 / nullptr means "feature off for this thread".
namespace {
thread_local int       g_aggr_worker_id = -1;
thread_local TlsCache *g_thread_tls     = nullptr;

// iter-17A multi-ring scaling: process-wide ring sharding config.
// Set once via configure_ring_sharding() at attach time.
int g_num_workers          = 1;   // T
int g_ring_shards_factor   = 1;   // N
int g_actual_ring_shards   = 1;   // ceil(T/N), capped at kRingShardsMax
int g_ring_block_size      = 1;   // ceil(T/actual_shards)
// iter-17A Plan A vs B routing selector. 0 = worker_id (Plan A,
// default). 1 = key_hash (Plan B, opt-in via FUSEE_RING_ROUTING=key_hash).
int g_ring_routing_mode    = 0;

inline int worker_ring_idx_helper() {
  // Plan A: static modulo on worker_id (default).
  int wid = g_aggr_worker_id;
  if (wid < 0) return 0;
  int idx = wid / g_ring_block_size;
  if (idx >= g_actual_ring_shards) idx = g_actual_ring_shards - 1;
  return idx;
}

inline int worker_ring_idx_for_key(uint64_t key) {
  // Plan B: ring_idx = fnv1a(key) % actual_shards. Per key-affinity:
  // same key always routes to same receiver, improving bucket cacheline
  // locality at receiver but losing worker-side per-ring locality.
  if (g_actual_ring_shards <= 1) return 0;
  return (int)(fnv1a_u64(key) % (uint64_t)g_actual_ring_shards);
}

inline int compute_ring_idx(uint64_t key) {
  return (g_ring_routing_mode == 1)
       ? worker_ring_idx_for_key(key)
       : worker_ring_idx_helper();
}
}  // namespace

// ============================================================
// iter-15A Layer A — per-thread path-counter instrumentation.
// Verifies that local_read/xhost_read/local_write/xhost_write
// trace scenarios trigger ONLY the expected code paths.
// Build-time off by default; enable with -DFUSEE_PATH_COUNTERS=1.
// ============================================================
#ifndef FUSEE_PATH_COUNTERS
#define FUSEE_PATH_COUNTERS 0
#endif

// ============================================================
// iter-15A Tier 1+2 — 2-tier cache ablation flags.
// FUSEE_DISABLE_TLS         : skip TLS L1 layer (default = 1, OFF)
// FUSEE_DISABLE_CACHE_POOL  : skip shared cache_pool L2 layer (default = 0, ON)
//
// iter-15A Tier 2 ruling (perf c2c): TLS layer does not reduce MESI
// HITM as iter-10A designed; default-disabled to simplify. Re-enable
// for ablation experiments only via -DFUSEE_DISABLE_TLS=0.
// ============================================================
#ifndef FUSEE_DISABLE_TLS
#define FUSEE_DISABLE_TLS 1
#endif
#ifndef FUSEE_DISABLE_CACHE_POOL
#define FUSEE_DISABLE_CACHE_POOL 0
#endif

#if FUSEE_PATH_COUNTERS
namespace {
struct PathCounters {
  // Read path
  uint64_t n_tls_hit                       = 0;  // R0_tls_hit
  uint64_t n_r2hit                         = 0;  // shared cache_pool hit
  uint64_t n_r2miss_local                  = 0;  // cache miss + key owned by self
  uint64_t n_r3                            = 0;  // cache miss + key peer-owned -> forward_read
  uint64_t n_cache_pool_insert_from_read   = 0;  // cache_pool_insert after R3
  // Write path (worker side)
  uint64_t n_local_write_worker            = 0;  // execute_write_local from worker (own-key)
  uint64_t n_forward_write                 = 0;  // forward_write_direct (peer-key)
  // Write path (receiver side — re-entries from forwarded writes)
  uint64_t n_local_write_with_blk_forwarded = 0; // W1 RESERVED receiver path
  uint64_t n_local_write_staging_forwarded  = 0; // STAGING receiver path
  uint64_t n_cache_pool_insert_from_write   = 0; // cache_pool_insert after W10 (both worker + receiver)
  // iter-15A microbench plan C.3: cache_pool counters
  uint64_t n_cache_pool_evict              = 0;  // cache_pool_evict() events (explicit physical delete)
  uint64_t n_cache_pool_set_stale          = 0;  // cache_pool_set_stale() events (lazy invalidate)
  uint64_t n_cache_pool_lru_evict          = 0;  // cache_pool_insert chose LRU slot (implicit eviction)
  // iter-15A microbench HR-2: receiver-side READ counter — symmetric to
  // n_local_write_with_blk_forwarded on the write path. Single-threaded
  // (receiver in primary), so survives fork — accurate cluster anchor for
  // gate verification.
  uint64_t n_read_handler_served           = 0;
};

thread_local PathCounters g_path_counters;
thread_local bool         g_path_counters_registered = false;

std::mutex g_path_registry_mu;
std::vector<PathCounters*> g_path_registry;
std::vector<uint64_t>      g_path_registry_tids;

void register_path_counters_once() {
  if (g_path_counters_registered) return;
  g_path_counters_registered = true;
  std::lock_guard<std::mutex> lk(g_path_registry_mu);
  g_path_registry.push_back(&g_path_counters);
  // pthread_self() returns thread id (opaque, but stable per-thread)
  g_path_registry_tids.push_back((uint64_t)pthread_self());
}
}  // anon ns

void fusee_path_counters_dump(FILE *fp, int host_id, const char *label) {
  std::lock_guard<std::mutex> lk(g_path_registry_mu);
  PathCounters agg{};
  const char *lbl = label ? label : "final";
  for (size_t i = 0; i < g_path_registry.size(); i++) {
    PathCounters *p = g_path_registry[i];
    fprintf(fp,
      "# PATH host=%d label=%s tid=%lu "
      "r0_tls=%lu r2hit=%lu r2miss_local=%lu r3=%lu cpool_ins_r=%lu "
      "local_w=%lu fwd_w=%lu local_w_blk_fwd=%lu local_w_stg_fwd=%lu cpool_ins_w=%lu "
      "cp_evict=%lu cp_set_stale=%lu cp_lru_evict=%lu rh_served=%lu\n",
      host_id, lbl, g_path_registry_tids[i],
      p->n_tls_hit, p->n_r2hit, p->n_r2miss_local, p->n_r3, p->n_cache_pool_insert_from_read,
      p->n_local_write_worker, p->n_forward_write,
      p->n_local_write_with_blk_forwarded, p->n_local_write_staging_forwarded,
      p->n_cache_pool_insert_from_write,
      p->n_cache_pool_evict, p->n_cache_pool_set_stale, p->n_cache_pool_lru_evict,
      p->n_read_handler_served);
    agg.n_tls_hit                        += p->n_tls_hit;
    agg.n_r2hit                          += p->n_r2hit;
    agg.n_r2miss_local                   += p->n_r2miss_local;
    agg.n_r3                             += p->n_r3;
    agg.n_cache_pool_insert_from_read    += p->n_cache_pool_insert_from_read;
    agg.n_local_write_worker             += p->n_local_write_worker;
    agg.n_forward_write                  += p->n_forward_write;
    agg.n_local_write_with_blk_forwarded += p->n_local_write_with_blk_forwarded;
    agg.n_local_write_staging_forwarded  += p->n_local_write_staging_forwarded;
    agg.n_cache_pool_insert_from_write   += p->n_cache_pool_insert_from_write;
    agg.n_cache_pool_evict               += p->n_cache_pool_evict;
    agg.n_cache_pool_set_stale           += p->n_cache_pool_set_stale;
    agg.n_cache_pool_lru_evict           += p->n_cache_pool_lru_evict;
    agg.n_read_handler_served            += p->n_read_handler_served;
  }
  fprintf(fp,
    "# PATH host=%d label=%s AGG "
    "r0_tls=%lu r2hit=%lu r2miss_local=%lu r3=%lu cpool_ins_r=%lu "
    "local_w=%lu fwd_w=%lu local_w_blk_fwd=%lu local_w_stg_fwd=%lu cpool_ins_w=%lu "
    "cp_evict=%lu cp_set_stale=%lu cp_lru_evict=%lu rh_served=%lu\n",
    host_id, lbl,
    agg.n_tls_hit, agg.n_r2hit, agg.n_r2miss_local, agg.n_r3, agg.n_cache_pool_insert_from_read,
    agg.n_local_write_worker, agg.n_forward_write,
    agg.n_local_write_with_blk_forwarded, agg.n_local_write_staging_forwarded,
    agg.n_cache_pool_insert_from_write,
    agg.n_cache_pool_evict, agg.n_cache_pool_set_stale, agg.n_cache_pool_lru_evict,
    agg.n_read_handler_served);
  fflush(fp);
}

#define PATH_CTR(field) do { \
  if (!g_path_counters_registered) register_path_counters_once(); \
  g_path_counters.field++; \
} while (0)

void fusee_path_ctr_cache_evict()     { PATH_CTR(n_cache_pool_evict); }
void fusee_path_ctr_cache_set_stale() { PATH_CTR(n_cache_pool_set_stale); }
void fusee_path_ctr_cache_lru_evict() { PATH_CTR(n_cache_pool_lru_evict); }

#else
#define PATH_CTR(field) do {} while (0)
void fusee_path_counters_dump(FILE *fp, int host_id, const char *label) {
  (void)fp; (void)host_id; (void)label;
}
void fusee_path_ctr_cache_evict()     {}
void fusee_path_ctr_cache_set_stale() {}
void fusee_path_ctr_cache_lru_evict() {}
#endif

// iter-16A receiver-NOOP study (per docs/microbench_xhost_spec.md §D).
// Env var FUSEE_RECEIVER_NOOP_LEVEL ∈ {0,1,2,3} switches how much of the
// receiver-side work to do. L0 = full real path; L1 skips invalidate
// broadcast; L2 only does CXL slot publish + ack; L3 only acks. Used to
// disentangle which receiver step dominates the 0.6 Mops/s xhost ceiling.
static int g_recv_noop_level = -1;
static inline int recv_noop_level() {
  int v = g_recv_noop_level;
  if (__builtin_expect(v < 0, 0)) {
    const char *e = std::getenv("FUSEE_RECEIVER_NOOP_LEVEL");
    v = (e && e[0]) ? std::atoi(e) : 0;
    if (v < 0 || v > 3) v = 0;
    g_recv_noop_level = v;
  }
  return v;
}

// iter-15A microbench HR (HARD REQUIREMENT 2): receiver-side read serving
// counter. Called from read_handler() — receiver thread is single-instance
// in primary process, so this counter is fork-safe and authoritative for
// gate verification (xhost_read scenario: total cluster ≈ trans_ops).
void CxlKvStoreA::wire_rings_for_child(WriteRingMatrix *wr,
                                       ForwardStagingMatrix *fs,
                                       ReadRingMatrix *rr,
                                       ReadStagingMatrix *rs,
                                       InvalRingMatrix *ir,
                                       ReservationRingMatrix *rsv,
                                       RcuDomain *rcu, HazardDomain *haz) {
  // Pure pointer assignment — no memset, no spawn, no fence. Children call
  // this in their post-fork init path to get the same ring view as primary
  // would have via enable_*_ring (which they don't call). Without this,
  // children's wr_/rr_/ir_/etc are nullptr → forward_*_direct fail with
  // -10 → high-T cross-host throughput is silently faked. See HR-2 in
  // docs/iter15A_microbench_plan/README.md.
  if (wr)  wr_  = wr;
  if (fs)  fs_  = fs;
  if (rr)  rr_  = rr;
  if (rs)  rs_  = rs;
  if (ir)  ir_  = ir;
  if (rsv) rsv_ = rsv;
  if (rcu) rcu_ = rcu;
  if (haz) haz_ = haz;
}

namespace {

// CoW slot publish: write value bytes to a freshly allocated CXL block
// (already done by caller before reaching here), then atomically update
// slot.{key,value}. The slot is 16 B = key(8) + value(8) on a single
// cacheline, so one flush_line + sfence makes both fields visible
// atomically to peer-host readers (CXL cacheline writes are 64 B atomic).
// Older two-phase publish (write value→flush→write key→flush) was
// inherited from spec §VI-A.bis but that pattern is for the case where
// value and key live on DIFFERENT cachelines — not this layout.
inline void publish_slot_cow(CxlKvSlot *slot, uint64_t key,
                             uint64_t encoded_value) {
  slot->value = encoded_value;
  slot->key = key;
  flush_line(slot);
  store_fence();
}

inline void retire_slot(CxlKvSlot *slot) {
  __atomic_store_n(&slot->key, kEmptyKey, __ATOMIC_RELEASE);
  flush_line(slot);
  store_fence();
}

inline uint8_t key_fingerprint(uint64_t key) {
  return (uint8_t)(fnv1a_u64(key) & 0xFF);
}

// iter-9A Phase 2: spin-wait helper templated over entry type.
// WriteEntry and ReadEntry both have {req_op_id, resp_op_id, status}
// at well-known offsets — both 2-cacheline (req on line 1, resp on
// line 2). Caller flushes resp's cacheline (line 2 of the entry)
// before each load so peer's CXL store reaches us.
//
// iter-8A Phase 5 timeout cap kept (5 ms): empirical 500× healthy p99
// budget — guarantees a runaway ring stall doesn't balloon trans_wall
// per-cell to tens of seconds.
//
// iter-12A bimodal fix: kBudgetUs kept at 5 ms. Empirical: bumping to
// 50 ms made the rare pathological-gap case 10× WORSE in wall time
// (84 s vs 8.5 s) without reducing the timeout-rate fraction. The
// fix that actually helps is the receiver-side gap-tolerance budget
// at cxl_kv_ops_A.cc:1340 / 1638 / 1675 (set to ~2.8 ms to match
// 5 ms worker timeout with headroom).
template <typename Entry>
inline int generic_spin_wait(Entry *e, uint64_t op_id, int *out_status) {
  const uint64_t kBudgetUs = 5000;
  uint64_t spin_start_ns = 0;
  // iter-17A Stage 5 micro-opt: lfence is sufficient to order clflushopt
  // before the subsequent load (Intel manual: clflushopt ordered w/r/t
  // following loads by LFENCE). mfence is overkill here — saves ~10-20 ns
  // per spin iter, compounded across the spin loop dominating 72-99% of
  // worker latency at high T (iter-16A finding).
  for (;;) {
    flush_line((void *)&e->resp_op_id);
    __builtin_ia32_lfence();
    uint64_t resp = e->resp_op_id.load(std::memory_order_acquire);
    if (resp == op_id) {
      if (out_status) *out_status = e->status;
      // Free slot: zero req_op_id (cacheline 1) so next forwarder
      // sees it free.
      e->req_op_id.store(0, std::memory_order_release);
      flush_line((void *)&e->req_op_id);
      store_fence();
      return 0;
    }
    if (spin_start_ns == 0) {
      timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
      spin_start_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    } else {
      timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
      uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
      if ((now_ns - spin_start_ns) / 1000 > kBudgetUs) {
        e->req_op_id.store(0, std::memory_order_release);
        flush_line((void *)&e->req_op_id);
        store_fence();
        return -11;
      }
    }
    __builtin_ia32_pause();
  }
}

// Encode a request op_id with src host in high 8 bits.
inline uint64_t encode_op_id(int host_id, uint64_t seq) {
  return ((uint64_t)(host_id + 1) << 56) | (seq & 0x00FFFFFFFFFFFFFFULL);
}

// Decode src host id from op_id (1-based in high byte → 0-based).
inline int decode_src_host(uint64_t op_id) {
  return (int)((op_id >> 56) & 0xFF) - 1;
}

}  // anonymous namespace

uint32_t CxlKvStoreA::bucket_idx(uint64_t key) const {
  return (uint32_t)(fnv1a_u64(key) % num_buckets_);
}

uint32_t CxlKvStoreA::owner_host(uint64_t key) const {
  return host_of(st_, key);
}

int CxlKvStoreA::attach(void *bucket_base, uint32_t num_buckets,
                        int host_id, int num_hosts, bool init_region,
                        ShardingTable *st, SlotDirectory *dir,
                        KvCachePool *cache, BlockFreeList *freelist,
                        CxlKvBlockPool *pool) {
  if (!bucket_base || num_buckets == 0 || !st || !dir || !cache ||
      !freelist || !pool) {
    return -1;
  }
  // H1 / AP13 trip wire.
  if ((int)st->num_hosts != num_hosts) {
    fprintf(stderr,
            "AP13 trip wire: ShardingTable.num_hosts=%u != attach.num_hosts=%d\n",
            st->num_hosts, num_hosts);
    std::abort();
  }
  buckets_ = static_cast<CxlKvBucket *>(bucket_base);
  num_buckets_ = num_buckets;
  host_id_ = host_id;
  num_hosts_ = num_hosts;
  st_ = st;
  dir_ = dir;
  cache_ = cache;
  freelist_ = freelist;
  pool_ = pool;

  if (init_region) {
    for (uint32_t b = 0; b < num_buckets_; b++) {
      for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
        buckets_[b].slots[s].key = kEmptyKey;
        buckets_[b].slots[s].value = 0;
      }
      flush_line(&buckets_[b]);
      flush_line((char *)&buckets_[b] + 64);
    }
    store_fence();
  }
  return 0;
}

// iter-13A Phase 2 W1: receiver fast-path for direct-pool-write. The
// worker (peer host) has already alloc'd a block in our reserved-for-it
// sub-segment and pool->write'd the value bytes. We just need to:
//   1. (mirror execute_write_local) lock the bucket, send invalidates
//      to sharers (UPDATE/DELETE), publish slot pointer (CoW).
//   2. Evict any cache_pool entry for this key (bumps bucket_epoch);
//      next reader will forward_read and pull fresh from pool.
//
// Skipped (vs execute_write_local):
//   - pool_->alloc()        (worker did it)
//   - pool_->write()        (worker did it)
//   - cache_pool_insert()   (would require pool_->read to fetch value;
//                            defeats the optimization. We evict instead;
//                            next read forwards to us, we read pool to
//                            respond — but pool->read fetches into owner
//                            DRAM ONCE per write event, vs the legacy
//                            path's 2 copies. Net win.)
//   - TLS cache update      (TLS is per-thread; receiver thread is not a
//                            worker thread; TLS irrelevant here.)
int CxlKvStoreA::execute_write_local_with_blk(uint64_t key, uint64_t blk_off,
                                              uint32_t value_len, int op_kind,
                                              int self_inval_src) {
  if (key == kEmptyKey) return -1;
  PATH_CTR(n_local_write_with_blk_forwarded);
  if (op_kind == kOpKindDelete) {
    // DELETE shouldn't reach this path — worker doesn't alloc on delete.
    // Fall back to regular delete.
    return execute_write_local(key, nullptr, 0, kOpKindDelete, self_inval_src);
  }
  if (blk_off == 0 || value_len == 0) return -1;
  if (pool_ && value_len + 4 > pool_->block_size()) return -5;
  uint32_t b = bucket_idx(key);
  CxlKvBucket *bucket = &buckets_[b];
  // iter-17A: removed bucket flush+mfence here (was 2×clflushopt+mfence).
  // Buckets are sharded by owner host; only owner writes its own buckets.
  // Within owner host, x86 MOESI keeps multi-core L1 coherent automatically,
  // and prior writes' clflushopt already invalidated owner L1 copies.
  // Empirical evidence: the matching flush in the post-lock path below only
  // flushed cacheline 1 of bucket, never touching slots 4-7's cacheline,
  // yet the code worked → flush was never load-bearing.

  int match = -1, empty = -1;
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
    if (bucket->slots[s].key == key) { match = s; break; }
    if (bucket->slots[s].key == kEmptyKey && empty < 0) empty = s;
  }

  int target_slot;
  if (op_kind == kOpKindInsert) {
    if (match >= 0) return -2;
    if (empty < 0) return -3;
    target_slot = empty;
  } else /* UPDATE */ {
    if (match < 0) return -1;
    target_slot = match;
  }

  SlotDirectoryEntry *de =
      slot_directory_entry(dir_, b, (uint32_t)target_slot);
  slot_directory_lock(de);
  // iter-17A: removed flush_line(bucket)+full_fence post-lock — see above.
  CxlKvSlot *slot = &bucket->slots[target_slot];

  // Sharer invalidate (same as execute_write_local Step 4).
  // iter-14A Phase 2: if `self_inval_src` is set, skip that host from
  // broadcast because the forwarding worker already self-invalidated
  // its local cache before sending the WriteEntry.
  // iter-16A Receiver-NOOP L1: skip broadcast entirely to measure
  // invalidate-broadcast cost.
  uint8_t bitmap = de->sharer_bitmap;
  if (recv_noop_level() < 1 && op_kind != kOpKindInsert && num_hosts_ > 1 && ir_) {
    for (int h = 0; h < num_hosts_; h++) {
      if (h == host_id_) continue;
#if FUSEE_XHOST_WRITE_SELF_INVAL
      if (h == self_inval_src) continue;
#endif
      if ((bitmap & (1u << h)) == 0) continue;
      send_invalidate((uint32_t)h, key);
    }
  }

  // CoW publish with worker's pre-allocated blk_off.
  uint8_t fp = key_fingerprint(key);
  uint8_t sc = kSizeClassBlock256;
  uint64_t encoded = cxl_slot_pack(blk_off, sc, fp);
  publish_slot_cow(slot, key, encoded);

  // Directory state.
  de->version++;
  de->state = kDirStateShared;
  de->sharer_bitmap = (uint8_t)(1u << (uint32_t)host_id_);
  slot_directory_unlock(de);

  // iter-13A W1: evict cache_pool entry (bumps bucket_epoch, forces next
  // reader to forward_read and pull fresh value from pool). We do NOT
  // call cache_pool_insert because we don't have value bytes here.
#if !FUSEE_DISABLE_CACHE_POOL
  cache_pool_evict(cache_, key);
#endif
  return 0;
}

// Owner-self write. Called by local insert/update/remove and by
// responder on behalf of a remote forwarder. Steps follow spec §V.
//
// iter-9A Phase 1: variable-length value bytes. value/value_len
// is the user payload. value=nullptr+value_len=0 is the DELETE
// convention. value_len > pool_->block_size() is rejected.
int CxlKvStoreA::execute_write_local(uint64_t key, const void *value,
                                     uint32_t value_len, int op_kind,
                                     int self_inval_src) {
  if (key == kEmptyKey) return -1;
  if (op_kind != kOpKindDelete) {
    if (!value || value_len == 0) return -1;
    if (pool_ && value_len + 4 > pool_->block_size()) return -5;
  }
  // iter-15A Layer A: count which caller — worker (self_inval_src < 0)
  // or receiver of forwarded write (self_inval_src >= 0, the src host id).
  if (self_inval_src < 0) PATH_CTR(n_local_write_worker);
  else                    PATH_CTR(n_local_write_staging_forwarded);
  PROBE_PATH("W1", key);
  uint32_t b = bucket_idx(key);
  CxlKvBucket *bucket = &buckets_[b];

  // iter-17A: removed bucket flush+mfence here. Owner-host-only writes
  // + x86 MOESI keeps owner's L1 coherent without explicit clflushopt.
  // (Same audit as execute_write_local_with_blk.)

  int match = -1, empty = -1;
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
    if (bucket->slots[s].key == key) { match = s; break; }
    if (bucket->slots[s].key == kEmptyKey && empty < 0) empty = s;
  }

  int target_slot;
  if (op_kind == kOpKindInsert) {
    if (match >= 0) return -2;
    if (empty < 0) return -3;
    target_slot = empty;
  } else if (op_kind == kOpKindUpdate) {
    if (match < 0) return -1;
    target_slot = match;
  } else /* DELETE */ {
    if (match < 0) return -1;
    target_slot = match;
  }

  SlotDirectoryEntry *de = slot_directory_entry(dir_, b, (uint32_t)target_slot);
  slot_directory_lock(de);
  PROBE_PATH("W2", key);

  // iter-17A: removed flush_line(bucket)+full_fence post-lock — see above.
  CxlKvSlot *slot = &bucket->slots[target_slot];

  // Step 4: invalidate sharers \ {self}. spec §I9 / I10.
  //
  // iter-5A Phase 4: broadcast OP_INVALIDATE on the SEPARATE
  // InvalRing channel and wait for ACK from each non-self sharer.
  // The InvalReceiver on each peer host drains InvalRing
  // independently of its WriteReceiver/ReadReceiver, so no circular
  // wait can form.
  // (See cxl_inval_ring.h header comment + RAP in iter5A_summary
  // for the deadlock-freedom argument.)
  //
  // For INSERT no prior sharers exist (we're the only host ever to
  // touch the slot), so skip. UPDATE/DELETE: scan bitmap.
  uint8_t bitmap = de->sharer_bitmap;
  PROBE_PATH("W3", key);
  if (op_kind != kOpKindInsert && num_hosts_ > 1 && ir_) {
    int n_sent = 0;
    for (int h = 0; h < num_hosts_; h++) {
      if (h == host_id_) continue;
#if FUSEE_XHOST_WRITE_SELF_INVAL
      // iter-14A Phase 2: skip the host that forwarded this write to
      // us — it self-invalidated its local cache before sending.
      if (h == self_inval_src) continue;
#endif
      if ((bitmap & (1u << h)) == 0) continue;
      if (n_sent == 0) PROBE_PATH("W4", key);
      send_invalidate((uint32_t)h, key);
      n_sent++;
    }
    if (n_sent > 0) PROBE_PATH("W6", key);
  }

  // Step 5: CoW publish.
  //
  // iter-5A Phase 1: blockpool path RESTORED with FUSEE_TRACE_BLOCKPOOL=1
  // instrumentation to diagnose the iter-4A-redo hang at workload-a 50k+.
  // Inline u64 path retained as runtime-fallback when pool_ == nullptr
  // (legacy tests that didn't supply pool, or ad-hoc benchmarks that
  // want the lower per-op cost).
  if (op_kind == kOpKindDelete) {
    retire_slot(slot);
    PROBE_PATH("W9", key);
  } else if (pool_ == nullptr) {
    // Inline u64 fallback (legacy / Protocol A degenerate mode).
    // Only valid when caller passes 8 bytes; truncated otherwise.
    uint64_t inline_v = 0;
    std::memcpy(&inline_v, value, value_len < 8 ? value_len : 8);
    publish_slot_cow(slot, key, inline_v);
    PROBE_PATH("W9", key);
  } else {
    // Full blockpool CoW path with real value_len bytes.
    uint64_t blk_off = pool_->alloc();
    PROBE_PATH("W7", key);
    static thread_local int trace = -1;
    if (trace == -1) {
      const char *e = getenv("FUSEE_TRACE_BLOCKPOOL");
      trace = (e && e[0] == '1') ? 1 : 0;
    }
    if (blk_off == 0) {
      if (trace) {
        fprintf(stderr, "[A:trace h%d] blockpool EXHAUSTED key=%lx op=%d\n",
                host_id_, key, op_kind);
      }
      slot_directory_unlock(de);
      return -4;
    }
    // iter-9A: store 4B length header, then value bytes. search()
    // owner-self-miss path reads the 4B header to know how much to copy
    // back. iter-17A audit: confirmed used at cxl_kv_ops_A.cc:2632-2634.
    // Removing would require encoding value_len in bucket slot — defer.
    uint8_t buf[4096];  // local stack scratch (max block 4 KB)
    uint32_t total = 4 + value_len;
    if (total > sizeof(buf)) {
      slot_directory_unlock(de);
      return -5;
    }
    std::memcpy(buf, &value_len, 4);
    std::memcpy(buf + 4, value, value_len);
    pool_->write(blk_off, buf, total);
    PROBE_PATH("W8", key);
    uint8_t fp = key_fingerprint(key);
    uint8_t sc = kSizeClassBlock256;
    // iter-9A: encode value_len in slot.value high bits via separate
    // field. For now keep slot encoding compatible (size_class hardcoded
    // to 256); reader uses pool->block_size() OR a separate value_len
    // field. We pack value_len into low bits of fp temporarily — TODO
    // expand slot to 24B in iter-10A. For Phase 1 we keep 16B slot and
    // store value_len alongside in directory entry (de->version repurposed
    // is bad; simpler: store value_len in cache_pool entry only and
    // owner reads it via slot.encoded high bits).
    //
    // Concrete iter-9A Phase 1 encoding: keep 16B slot; reader fetches
    // pool block at blk_off + reads "stored value_len" from a small
    // header at the start of each block (4B header + value bytes).
    // pool_->write writes header+value; pool_->read returns header.
    uint64_t encoded = cxl_slot_pack(blk_off, sc, fp);
    if (trace) {
      fprintf(stderr,
              "[A:trace h%d] write key=%lx op=%d blk_off=%lx len=%u\n",
              host_id_, key, op_kind, blk_off, value_len);
    }
    publish_slot_cow(slot, key, encoded);
    PROBE_PATH("W9", key);
  }

  // Step 6: directory state.
  de->version++;
  if (op_kind == kOpKindDelete) {
    de->state = kDirStateInvalid;
    de->sharer_bitmap = 0;
  } else {
    de->state = kDirStateShared;
    // Self only — peer hosts must re-register if they want to cache.
    de->sharer_bitmap = (uint8_t)(1u << (uint32_t)host_id_);
  }
  PROBE_PATH("W10", key);
  slot_directory_unlock(de);

  // Step 7: own cache.
  if (op_kind == kOpKindDelete) {
#if !FUSEE_DISABLE_CACHE_POOL
    cache_pool_evict(cache_, key);
#endif
    // iter-10A Phase 1.C: also evict from TLS so subsequent reads
    // don't see the deleted entry. (cache_pool_evict already bumped
    // bucket_epoch so any TLS reader without our explicit evict would
    // also detect stale on next access — this is belt + suspenders.)
#if !FUSEE_DISABLE_TLS
    if (g_thread_tls) tls_evict(g_thread_tls, key);
#endif
  } else {
#if !FUSEE_DISABLE_CACHE_POOL
    cache_pool_insert(cache_, key,
                      reinterpret_cast<const uint8_t *>(value),
                      value_len);
    PATH_CTR(n_cache_pool_insert_from_write);
#endif
    // iter-10A Phase 1.C: populate TLS L1 with the just-written value
    // and the post-bump epoch so this thread's next read hits TLS.
#if !FUSEE_DISABLE_TLS
    if (g_thread_tls) {
      uint64_t cur_epoch = cache_pool_bucket_epoch(cache_, key);
      tls_insert(g_thread_tls, key,
                 reinterpret_cast<const uint8_t *>(value),
                 value_len, cur_epoch);
    }
#endif
  }
  PROBE_PATH("W12", key);
  return 0;
}

int CxlKvStoreA::insert(uint64_t key, const void *value, uint32_t value_len) {
  if (key == kEmptyKey) return -1;
  uint32_t owner = owner_host(key);
  if (owner != (uint32_t)host_id_) {
    return forward_write(owner, key, value, value_len, kOpKindInsert);
  }
  return execute_write_local(key, value, value_len, kOpKindInsert);
}

int CxlKvStoreA::update(uint64_t key, const void *value, uint32_t value_len) {
  if (key == kEmptyKey) return -1;
  uint32_t owner = owner_host(key);
  if (owner != (uint32_t)host_id_) {
    return forward_write(owner, key, value, value_len, kOpKindUpdate);
  }
  return execute_write_local(key, value, value_len, kOpKindUpdate);
}

int CxlKvStoreA::remove(uint64_t key) {
  if (key == kEmptyKey) return -1;
  uint32_t owner = owner_host(key);
  if (owner != (uint32_t)host_id_) {
    return forward_write(owner, key, nullptr, 0, kOpKindDelete);
  }
  return execute_write_local(key, nullptr, 0, kOpKindDelete);
}

// ---- Cross-host forwarding (iter-9A Phase 2: 3-ring + staging) ----

// iter-17A Phase 3: receiver layout helper.
// Computes per-type thread plans = list of (cpu, ring_indices to drain).
// Allocates ceil-fair across the receiver CPU pool starting at
// start_cpu = num_workers (right after worker pinning). Write gets the
// remainder when (pool_size % 3) != 0.
namespace {
struct ReceiverPlan {
  int cpu;
  std::vector<int> ring_indices;
};
struct ReceiverLayout {
  std::vector<ReceiverPlan> write_plans;
  std::vector<ReceiverPlan> read_plans;
  std::vector<ReceiverPlan> inval_plans;
  int start_cpu = 0;
  int pool_size = 0;
};

ReceiverLayout compute_receiver_layout(int num_workers, int num_shards) {
  ReceiverLayout L;
  long nproc = sysconf(_SC_NPROCESSORS_ONLN);
  if (nproc <= 0) nproc = 86;
  // start_cpu = num_workers (紧跟 worker), with hard floor at 64
  // empirically: pinning receivers to CPU 8-10 at T=8 gave 5x slowdown
  // (likely L3 cache contention with worker cores 0-7; further audit
  // pending). Use max(num_workers, 64) until diagnosis available.
  int start_cpu = num_workers;
  if (start_cpu < 64) start_cpu = 64;
  if (start_cpu < 0) start_cpu = 0;
  if (start_cpu >= (int)nproc) start_cpu = (int)nproc - 1;
  int pool_size = (int)nproc - start_cpu;
  if (pool_size < 3) pool_size = 3;
  L.start_cpu = start_cpu;
  L.pool_size = pool_size;

  int budget_base  = pool_size / 3;
  int budget_extra = pool_size % 3;  // give to write
  int t_w = std::min(num_shards, budget_base + budget_extra);
  int t_r = std::min(num_shards, budget_base);
  int t_i = std::min(num_shards, budget_base);
  if (t_w < 1) t_w = 1;
  if (t_r < 1) t_r = 1;
  if (t_i < 1) t_i = 1;

  // iter-17A Exp 3: env override decouples thread-count from the default
  // pool_size/3 floor. Holds num_shards fixed, varies threads, so we can
  // isolate packing cost (each thread owning multiple rings) from
  // per-bucket lock contention. Allowed values [1, num_shards].
  const char *force_env = std::getenv("FUSEE_FORCE_THREADS_PER_TYPE");
  if (force_env && *force_env) {
    int v = std::atoi(force_env);
    if (v >= 1 && v <= num_shards) {
      t_w = v; t_r = v; t_i = v;
    }
  }

  auto build = [&](int n_threads, int cpu_offset,
                   std::vector<ReceiverPlan> &out) {
    for (int i = 0; i < n_threads; i++) {
      ReceiverPlan p;
      // Wrap CPU into [start_cpu, nproc) pool. With env override the total
      // (t_w + t_r + t_i) can exceed pool_size; modulo packs them into the
      // pool with intentional oversubscription on the wrap.
      p.cpu = start_cpu + ((cpu_offset + i) % pool_size);
      int ring_start = (int)((long)i * num_shards / n_threads);
      int ring_end   = (int)((long)(i + 1) * num_shards / n_threads);
      for (int r = ring_start; r < ring_end; r++) {
        p.ring_indices.push_back(r);
      }
      out.push_back(std::move(p));
    }
  };
  build(t_w, 0,             L.write_plans);
  build(t_r, t_w,           L.read_plans);
  build(t_i, t_w + t_r,     L.inval_plans);
  return L;
}
}  // namespace

int CxlKvStoreA::enable_write_ring(WriteRingMatrix *wr,
                                   ForwardStagingMatrix *fs,
                                   bool init_region, bool spawn_receiver) {
  if (!wr || !fs) return -1;
  wr_ = wr;
  fs_ = fs;
  // iter-12A Phase 5.1c (residual bimodal RCA): memset+flush regardless of
  // init_region. On init=false (host 1), this host's L1/L2/L3 retain dirty
  // cache lines for ring->head / entries[].req_op_id from the PRIOR process
  // (CXL is not coherent across hosts; cache state survives exec exec since
  // the CXL DAX page is physically-addressed). Without this rewrite, the
  // receiver reads ring->head non-atomically with no flush_line and observes
  // the stale terminal head (e.g. 181 from a prior run) — when head>tail,
  // the inner work loop never enters → entire ring direction stuck → every
  // worker 5ms-timeouts forever (manifested as workloadb T4 on kv1024
  // bimodal: 0 P5R_VS, head=181 stuck across 9309 polls in 118 s). Order is
  // race-free: host 1 reaches this point only after init_done bit 0x1, set
  // by host 0 *after* host 0's init=true memset; host 1's memset rewrites
  // the same 0s. The init_region parameter is retained for callsite docs
  // even though both branches now collapse to the same body.
  std::memset(wr, 0, write_ring_matrix_bytes());
  flush_region(wr, write_ring_matrix_bytes());
  std::memset(fs, 0, forward_staging_matrix_bytes());
  flush_region(fs, forward_staging_matrix_bytes());
  store_fence();
  (void)init_region;
  if (spawn_receiver) {
    write_receiver_stop_.store(false, std::memory_order_relaxed);
    // iter-17A Phase 3: spawn N write-receiver threads per layout.
    int T = g_num_workers;
    int S = g_actual_ring_shards;
    ReceiverLayout L = compute_receiver_layout(T, S);
    write_receivers_.reserve(L.write_plans.size());
    for (size_t i = 0; i < L.write_plans.size(); i++) {
      ReceiverPlan p = L.write_plans[i];  // copy for capture
      int idx = (int)i;
      write_receivers_.emplace_back([this, p, idx]() {
        char nm[16]; snprintf(nm, sizeof(nm), "WriteRecv%d", idx);
        pthread_setname_np(pthread_self(), nm);
        cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(p.cpu, &cs);
        pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
        fprintf(stderr,
          "[A:thread] %s pid=%d tid=%lu cpu=%d host=%d rings=[",
          nm, getpid(), (unsigned long)pthread_self(), p.cpu, host_id_);
        for (size_t k = 0; k < p.ring_indices.size(); k++) {
          fprintf(stderr, "%s%d", k ? "," : "", p.ring_indices[k]);
        }
        fprintf(stderr, "]\n");
        this->write_receiver_loop(p.ring_indices);
      });
    }
  }
  return 0;
}

void CxlKvStoreA::stop_write_receiver() {
  if (write_receivers_.empty()) return;
  write_receiver_stop_.store(true, std::memory_order_release);
  for (auto &t : write_receivers_) {
    if (t.joinable()) t.join();
  }
  write_receivers_.clear();
}

int CxlKvStoreA::enable_read_ring(ReadRingMatrix *rr, ReadStagingMatrix *rs,
                                  bool init_region, bool spawn_receiver) {
  if (!rr || !rs) return -1;
  rr_ = rr;
  rs_ = rs;
  // iter-12A Phase 5.1c: see comment in enable_write_ring above.
  std::memset(rr, 0, read_ring_matrix_bytes());
  flush_region(rr, read_ring_matrix_bytes());
  std::memset(rs, 0, read_staging_matrix_bytes());
  flush_region(rs, read_staging_matrix_bytes());
  store_fence();
  (void)init_region;
  if (spawn_receiver) {
    read_receiver_stop_.store(false, std::memory_order_relaxed);
    int T = g_num_workers;
    int S = g_actual_ring_shards;
    ReceiverLayout L = compute_receiver_layout(T, S);
    read_receivers_.reserve(L.read_plans.size());
    for (size_t i = 0; i < L.read_plans.size(); i++) {
      ReceiverPlan p = L.read_plans[i];
      int idx = (int)i;
      read_receivers_.emplace_back([this, p, idx]() {
        char nm[16]; snprintf(nm, sizeof(nm), "ReadRecv%d", idx);
        pthread_setname_np(pthread_self(), nm);
        cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(p.cpu, &cs);
        pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
        fprintf(stderr,
          "[A:thread] %s pid=%d tid=%lu cpu=%d host=%d rings=[",
          nm, getpid(), (unsigned long)pthread_self(), p.cpu, host_id_);
        for (size_t k = 0; k < p.ring_indices.size(); k++) {
          fprintf(stderr, "%s%d", k ? "," : "", p.ring_indices[k]);
        }
        fprintf(stderr, "]\n");
        this->read_receiver_loop(p.ring_indices);
      });
    }
  }
  return 0;
}

void CxlKvStoreA::stop_read_receiver() {
  if (read_receivers_.empty()) return;
  read_receiver_stop_.store(true, std::memory_order_release);
  for (auto &t : read_receivers_) {
    if (t.joinable()) t.join();
  }
  read_receivers_.clear();
}

// iter-9A Phase 2.C: per-worker aggregator routing id setter.
// iter-10A Phase 1.C: per-worker TLS cache attach setter.
// Backing thread_locals declared at file top (so execute_write_local
// can read them).
void CxlKvStoreA::set_worker_id(int wid) { g_aggr_worker_id = wid; }
void CxlKvStoreA::set_thread_tls_cache(TlsCache *tls) { g_thread_tls = tls; }

// iter-17A multi-ring scaling: set process-wide sharding parameters.
void CxlKvStoreA::configure_ring_sharding(int num_workers, int shards_factor) {
  if (num_workers < 1) num_workers = 1;
  g_num_workers = num_workers;
  if (shards_factor <= 0) {
    g_ring_shards_factor = 0;
    g_actual_ring_shards = 1;
    g_ring_block_size    = num_workers;
    fprintf(stderr, "[A:cfg] ring_sharding T=%d N=%d (disabled) -> shards=1 block=%d routing=%s\n",
            num_workers, shards_factor, num_workers,
            g_ring_routing_mode == 1 ? "key_hash" : "worker_id");
    return;
  }
  g_ring_shards_factor = shards_factor;
  int s = (num_workers + shards_factor - 1) / shards_factor;
  if (s < 1) s = 1;
  if (s > kRingShardsMax) s = kRingShardsMax;
  g_actual_ring_shards = s;
  int blk = (num_workers + s - 1) / s;
  if (blk < 1) blk = 1;
  g_ring_block_size    = blk;
  fprintf(stderr, "[A:cfg] ring_sharding T=%d N=%d -> shards=%d block=%d routing=%s\n",
          num_workers, shards_factor, s, blk,
          g_ring_routing_mode == 1 ? "key_hash" : "worker_id");
}

// iter-17A Plan B opt-in: select routing mode.
// 0 = worker_id (Plan A, default), 1 = key_hash (Plan B).
void CxlKvStoreA::set_ring_routing_mode(int mode) {
  g_ring_routing_mode = (mode == 1) ? 1 : 0;
}

int CxlKvStoreA::num_ring_shards()    { return g_actual_ring_shards; }
int CxlKvStoreA::ring_shards_factor() { return g_ring_shards_factor; }
int CxlKvStoreA::worker_ring_idx()    { return worker_ring_idx_helper(); }

// (compute_receiver_layout definition is up at file-scope anon namespace
// earlier so enable_write_ring/read/invalidate can see it. See above.)

// ---- Aggregator-routed worker dispatchers ----
//
// Each routes through aggregator_->slots[k][g_aggr_worker_id] when
// the aggregator is wired AND the calling thread has set its worker
// id. Otherwise falls back to the direct CXL path. Receivers (which
// never call set_worker_id) automatically take the direct path —
// important so receivers calling send_invalidate cannot starve on
// the aggregator's single-sender bottleneck.
int CxlKvStoreA::forward_write(uint32_t owner, uint64_t key,
                               const void *value, uint32_t value_len,
                               int op_kind) {
  if (!aggr_ || g_aggr_worker_id < 0 ||
      g_aggr_worker_id >= num_aggr_workers_) {
    return forward_write_direct(owner, key, value, value_len, op_kind);
  }
  int wid = g_aggr_worker_id;
  AggrSlot *s = aggregator_slot(aggr_, kAggrRingKindWrite, wid);
  // Wait for slot empty (initial state == 0 = empty after region zero).
  while (s->state.load(std::memory_order_acquire) != kAggrEmpty) {
    __builtin_ia32_pause();
  }
  if (value && value_len > 0) {
    std::memcpy(aggregator_value_buf(aggr_, kAggrRingKindWrite, wid),
                value, value_len);
  }
  s->key = key;
  s->op_kind = (uint8_t)op_kind;
  s->value_len = value_len;
  s->dst_host = owner;
  s->state.store(kAggrPending, std::memory_order_release);
  while (s->state.load(std::memory_order_acquire) != kAggrDone) {
    __builtin_ia32_pause();
  }
  int rc = s->result_status;
  s->state.store(kAggrEmpty, std::memory_order_release);
  return rc;
}

int CxlKvStoreA::forward_read(uint32_t owner, uint64_t key,
                              void *out_buf, uint32_t buf_len,
                              uint32_t *out_len) {
  if (!aggr_ || g_aggr_worker_id < 0 ||
      g_aggr_worker_id >= num_aggr_workers_) {
    return forward_read_direct(owner, key, out_buf, buf_len, out_len);
  }
  int wid = g_aggr_worker_id;
  AggrSlot *s = aggregator_slot(aggr_, kAggrRingKindRead, wid);
  while (s->state.load(std::memory_order_acquire) != kAggrEmpty) {
    __builtin_ia32_pause();
  }
  s->key = key;
  s->op_kind = (uint8_t)kOpKindCacheRegister;
  s->value_len = 0;
  s->dst_host = owner;
  s->state.store(kAggrPending, std::memory_order_release);
  while (s->state.load(std::memory_order_acquire) != kAggrDone) {
    __builtin_ia32_pause();
  }
  int rc = s->result_status;
  // For aggregator-routed reads, the sender writes the response value
  // bytes into value_bufs[Read][wid]; copy out to caller and reset.
  if (rc == 0) {
    uint32_t vlen = s->value_len;
    uint8_t *src = aggregator_value_buf(aggr_, kAggrRingKindRead, wid);
    if (out_len) *out_len = vlen;
    uint32_t copy_len = vlen < buf_len ? vlen : buf_len;
    if (out_buf && copy_len > 0) std::memcpy(out_buf, src, copy_len);
  } else {
    if (out_len) *out_len = 0;
  }
  s->state.store(kAggrEmpty, std::memory_order_release);
  return rc;
}

int CxlKvStoreA::send_invalidate(uint32_t target_host, uint64_t key) {
  if (!aggr_ || g_aggr_worker_id < 0 ||
      g_aggr_worker_id >= num_aggr_workers_) {
    return send_invalidate_direct(target_host, key);
  }
  int wid = g_aggr_worker_id;
  AggrSlot *s = aggregator_slot(aggr_, kAggrRingKindInval, wid);
  while (s->state.load(std::memory_order_acquire) != kAggrEmpty) {
    __builtin_ia32_pause();
  }
  s->key = key;
  s->dst_host = target_host;
  s->state.store(kAggrPending, std::memory_order_release);
  while (s->state.load(std::memory_order_acquire) != kAggrDone) {
    __builtin_ia32_pause();
  }
  int rc = s->result_status;
  s->state.store(kAggrEmpty, std::memory_order_release);
  return rc;
}

// ---- Sender threads — single producer per CXL ring ----
//
// Each sender owns the fetch_add on its CXL ring's tail; workers no
// longer contend on it. Polls all worker slots round-robin.
// iter-10A Phase 3: sender batch policies. Selected at runtime via
// FUSEE_BATCH_POLICY env (P0/P1/P2/P3), default P0 (per-slot serial,
// equivalent to iter-9A redo Phase 2.C aggregator behavior).
//
//  P0: per-slot serial — for each pending slot, call _direct (which
//      does fetch_add(1) + write entry + spin on resp). 1 op per
//      fetch_add, 1 op per blocking spin. Iter-9A redo baseline.
//  P1: fixed-K + timeout — collect up to K pending slots OR T_us,
//      group by dst, single fetch_add(K_dst) per dst in the batch.
//      Pipelined response collection. Tunable: FUSEE_BATCH_K (default
//      16), FUSEE_BATCH_TIMEOUT_US (default 100).
//  P2: adaptive drain-all — each iteration scan all pending slots,
//      group by dst, single fetch_add(N_dst) per dst. No K cap, no
//      timeout. Self-tuning.
//  P3: per-dst round-robin — each iteration drain ONE dst at a time
//      (full per-dst batch, no K cap). Tests "is the win from
//      fetch_add batching, or from per-dst grouping?"
namespace {
enum BatchPolicy { kBP_P0 = 0, kBP_P1, kBP_P2, kBP_P3 };
BatchPolicy parse_batch_policy() {
  const char *e = getenv("FUSEE_BATCH_POLICY");
  if (!e) return kBP_P0;
  if (e[0]=='P' && e[1]=='1') return kBP_P1;
  if (e[0]=='P' && e[1]=='2') return kBP_P2;
  if (e[0]=='P' && e[1]=='3') return kBP_P3;
  return kBP_P0;
}
uint32_t parse_batch_k() {
  if (const char *e = getenv("FUSEE_BATCH_K"); e && e[0]) {
    int v = atoi(e);
    if (v > 0 && v <= 256) return (uint32_t)v;
  }
  return 16;
}
uint64_t parse_batch_timeout_us() {
  if (const char *e = getenv("FUSEE_BATCH_TIMEOUT_US"); e && e[0]) {
    long v = atol(e);
    if (v > 0 && v <= 10000) return (uint64_t)v;
  }
  return 100;
}
inline uint64_t now_ns_mono() {
  timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
}  // namespace

// Drain a batch of write slots all targeting `dst`. Issues one
// fetch_add(n) on WriteRing[me][dst].tail, fills n entries +
// staging in parallel, single sfence at end, then spins on n
// resp_op_ids in round-robin and flips ack as each comes back.
// `slot_workers[]` is the worker_id per slot (so we know which
// AggrSlot to flip ack on).
// Returns number of slots successfully ACKed (n on success, < n if
// some timed out).
// iter-10A Phase 3 simplification: ring-state corruption risk if a
// batched fetch_add(N) gets a partial timeout (some slots ACK, others
// don't — receiver's head gets stuck because timeout cleared req=0).
// Resolved by keeping per-slot fetch_add semantics internally — the
// "batch policies" P1/P2/P3 differ only in WHEN/HOW many slots a
// sender processes in one scan-and-drain pass, not in HOW the slots
// are committed to the CXL ring. Each slot still uses
// forward_write_direct's per-slot fetch_add(1).
//
// True fetch_add(N) batching is iter-11A backlog (requires receiver-
// side gap-tolerance for unfilled slots within a batch).
int CxlKvStoreA::write_sender_drain_dst(int dst, int n,
                                         const int *slot_workers) {
  if (n <= 0 || n > num_aggr_workers_) return 0;
  int n_done = 0;
  for (int i = 0; i < n; i++) {
    AggrSlot *s = aggregator_slot(aggr_, kAggrRingKindWrite, slot_workers[i]);
    uint8_t *value = aggregator_value_buf(aggr_, kAggrRingKindWrite,
                                          slot_workers[i]);
    int rc = forward_write_direct((uint32_t)dst, s->key, value,
                                  s->value_len, (int)s->op_kind);
    s->result_status = rc;
    s->state.store(kAggrDone, std::memory_order_release);
    n_done++;
  }
  return n_done;
}

// Stub — body replaced above. Code below is the abandoned fetch_add(N)
// batched implementation, kept for reference / iter-11A revival.
#if 0  // iter-11A revival
int CxlKvStoreA::write_sender_drain_dst_v2_unused(int dst, int n,
                                         const int *slot_workers) {
  if (n <= 0 || n > num_aggr_workers_) return 0;
  // iter-17A: 3D ring matrix — unused/dead code, use shard 0
  WriteRing *ring = &wr_->rings[host_id_][dst][0];
  // Issue: fetch_add(n) on ring tail.
  uint64_t base = ring->tail.fetch_add((uint64_t)n, std::memory_order_acq_rel);
  flush_line((void *)&ring->tail);
  store_fence();

  // Generate per-slot op_ids.
  uint64_t op_ids[kMaxAggrWorkers];
  for (int i = 0; i < n; i++) {
    uint64_t my_op = write_op_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
    op_ids[i] = encode_op_id(host_id_, my_op);
  }

  // Wait for all n target slots free, then write all.
  for (int i = 0; i < n; i++) {
    uint32_t slot_idx = (uint32_t)((base + i) % kWriteRingDepth);
    WriteEntry *e = &ring->entries[slot_idx];
    AggrSlot *s = aggregator_slot(aggr_, kAggrRingKindWrite, slot_workers[i]);
    // Wait slot free
    for (;;) {
      flush_line((void *)&e->req_op_id);
      full_fence();
      if (e->req_op_id.load(std::memory_order_acquire) == 0) break;
      __builtin_ia32_pause();
    }
    // Copy value to staging + flush + sfence (BEFORE writing req_op_id
    // so receiver can't observe req before staging is committed to CXL).
    if (s->value_len > 0 && s->value_len <= kForwardStagingSlotBytes) {
      uint8_t *staging = forward_staging_bytes(fs_, host_id_, dst, (int)slot_idx);
      uint8_t *src = aggregator_value_buf(aggr_, kAggrRingKindWrite, slot_workers[i]);
      std::memcpy(staging, src, s->value_len);
      for (uint32_t off = 0; off < s->value_len; off += 64) {
        flush_line((void *)(staging + off));
      }
      store_fence();  // ⭐ critical: staging must be CXL-visible before req
    }
    e->key = s->key;
    e->op_kind = (uint8_t)s->op_kind;
    e->value_len = s->value_len;
    e->staging_off = slot_idx;
    e->staging_gen = 0;
    e->resp_op_id.store(0, std::memory_order_relaxed);
    e->status = 0;
    std::atomic_thread_fence(std::memory_order_release);
    e->req_op_id.store(op_ids[i], std::memory_order_release);
    flush_line((void *)&e->req_op_id);
    store_fence();  // ⭐ critical: req must be CXL-visible before next slot
  }

  // Spin on n responses round-robin.
  bool done[kMaxAggrWorkers];
  for (int i = 0; i < n; i++) done[i] = false;
  int n_done = 0;
  uint64_t spin_start_ns = now_ns_mono();
  const uint64_t kBudgetUs = 5000;
  while (n_done < n) {
    for (int i = 0; i < n; i++) {
      if (done[i]) continue;
      uint32_t slot_idx = (uint32_t)((base + i) % kWriteRingDepth);
      WriteEntry *e = &ring->entries[slot_idx];
      flush_line((void *)&e->resp_op_id);
      full_fence();
      uint64_t resp = e->resp_op_id.load(std::memory_order_acquire);
      if (resp == op_ids[i]) {
        AggrSlot *s = aggregator_slot(aggr_, kAggrRingKindWrite, slot_workers[i]);
        s->result_status = e->status;
        e->req_op_id.store(0, std::memory_order_release);
        flush_line((void *)&e->req_op_id);
        store_fence();
        s->state.store(kAggrDone, std::memory_order_release);
        done[i] = true;
        n_done++;
      }
    }
    if (n_done < n) {
      uint64_t now = now_ns_mono();
      if ((now - spin_start_ns) / 1000 > kBudgetUs) {
        // Timeout: mark remaining as failed
        for (int i = 0; i < n; i++) {
          if (done[i]) continue;
          uint32_t slot_idx = (uint32_t)((base + i) % kWriteRingDepth);
          WriteEntry *e = &ring->entries[slot_idx];
          e->req_op_id.store(0, std::memory_order_release);
          flush_line((void *)&e->req_op_id);
          AggrSlot *s = aggregator_slot(aggr_, kAggrRingKindWrite, slot_workers[i]);
          s->result_status = -11;
          s->state.store(kAggrDone, std::memory_order_release);
        }
        store_fence();
        break;
      }
      __builtin_ia32_pause();
    }
  }
  return n_done;
}
#endif

// Same shape for read sender. Each ReadRing slot has no staging arena
// (response carries blk_off). Reader (caller) does pool->read on hit.
int CxlKvStoreA::read_sender_drain_dst(int dst, int n,
                                        const int *slot_workers) {
  if (n <= 0 || n > num_aggr_workers_) return 0;
  // iter-10A Phase 3 simplification (see write_sender_drain_dst above).
  // Per-slot forward_read_direct calls; scheduling policies P1/P2/P3
  // differ in WHEN/HOW MANY slots a sender drains per pass, not in
  // CXL ring fetch_add batching.
  int n_done = 0;
  for (int i = 0; i < n; i++) {
    AggrSlot *s = aggregator_slot(aggr_, kAggrRingKindRead, slot_workers[i]);
    uint8_t *vbuf = aggregator_value_buf(aggr_, kAggrRingKindRead,
                                          slot_workers[i]);
    uint32_t got_len = 0;
    int rc = forward_read_direct((uint32_t)dst, s->key, vbuf,
                                  kAggrSlotValueBytes, &got_len);
    s->result_status = rc;
    s->value_len = got_len;
    s->state.store(kAggrDone, std::memory_order_release);
    n_done++;
  }
  return n_done;
}

#if 0  // iter-11A revival
int CxlKvStoreA::read_sender_drain_dst_v2_unused(int dst, int n,
                                        const int *slot_workers) {
  if (n <= 0 || n > num_aggr_workers_) return 0;
  // iter-17A: 3D ring matrix — unused/dead code, use shard 0
  ReadRing *ring = &rr_->rings[host_id_][dst][0];
  uint64_t base = ring->tail.fetch_add((uint64_t)n, std::memory_order_acq_rel);
  flush_line((void *)&ring->tail);
  store_fence();

  uint64_t op_ids[kMaxAggrWorkers];
  for (int i = 0; i < n; i++) {
    uint64_t my_op = read_op_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
    op_ids[i] = encode_op_id(host_id_, my_op);
  }

  for (int i = 0; i < n; i++) {
    uint32_t slot_idx = (uint32_t)((base + i) % kReadRingDepth);
    ReadEntry *e = &ring->entries[slot_idx];
    AggrSlot *s = aggregator_slot(aggr_, kAggrRingKindRead, slot_workers[i]);
    for (;;) {
      flush_line((void *)&e->req_op_id);
      full_fence();
      if (e->req_op_id.load(std::memory_order_acquire) == 0) break;
      __builtin_ia32_pause();
    }
    e->key = s->key;
    e->resp_op_id.store(0, std::memory_order_relaxed);
    e->status = 0;
    e->resp_value_len = 0;
    e->resp_blk_off = 0;
    std::atomic_thread_fence(std::memory_order_release);
    e->req_op_id.store(op_ids[i], std::memory_order_release);
    flush_line((void *)&e->req_op_id);
    store_fence();  // ⭐ slot N must commit before slot N+1
  }

  bool done[kMaxAggrWorkers];
  for (int i = 0; i < n; i++) done[i] = false;
  int n_done = 0;
  uint64_t spin_start_ns = now_ns_mono();
  const uint64_t kBudgetUs = 5000;
  while (n_done < n) {
    for (int i = 0; i < n; i++) {
      if (done[i]) continue;
      uint32_t slot_idx = (uint32_t)((base + i) % kReadRingDepth);
      ReadEntry *e = &ring->entries[slot_idx];
      flush_line((void *)&e->resp_op_id);
      full_fence();
      uint64_t resp = e->resp_op_id.load(std::memory_order_acquire);
      if (resp == op_ids[i]) {
        AggrSlot *s = aggregator_slot(aggr_, kAggrRingKindRead, slot_workers[i]);
        s->result_status = e->status;
        if (e->status == 0) {
          uint32_t vlen = e->resp_value_len;
          uint64_t blk_off = e->resp_blk_off;
          uint8_t *vbuf = aggregator_value_buf(aggr_, kAggrRingKindRead,
                                                slot_workers[i]);
          if (blk_off == 0 && vlen <= 8) {
            std::memcpy(vbuf, &e->resp_blk_off, vlen);
          } else if (vlen > 0 && vlen <= kForwardStagingSlotBytes &&
                     pool_) {
            pool_->read(blk_off + 4, vbuf, vlen);
          }
          s->value_len = vlen;
        }
        e->req_op_id.store(0, std::memory_order_release);
        flush_line((void *)&e->req_op_id);
        store_fence();
        s->state.store(kAggrDone, std::memory_order_release);
        done[i] = true;
        n_done++;
      }
    }
    if (n_done < n) {
      uint64_t now = now_ns_mono();
      if ((now - spin_start_ns) / 1000 > kBudgetUs) {
        for (int i = 0; i < n; i++) {
          if (done[i]) continue;
          uint32_t slot_idx = (uint32_t)((base + i) % kReadRingDepth);
          ReadEntry *e = &ring->entries[slot_idx];
          e->req_op_id.store(0, std::memory_order_release);
          flush_line((void *)&e->req_op_id);
          AggrSlot *s = aggregator_slot(aggr_, kAggrRingKindRead, slot_workers[i]);
          s->result_status = -11;
          s->state.store(kAggrDone, std::memory_order_release);
        }
        store_fence();
        break;
      }
      __builtin_ia32_pause();
    }
  }
  return n_done;
}
#endif

// Inval batched. InvalEntry carries only key.
int CxlKvStoreA::inval_sender_drain_dst(int dst, int n,
                                         const int *slot_workers) {
  if (n <= 0 || n > num_aggr_workers_) return 0;
  // iter-10A Phase 3 simplification — see write_sender_drain_dst.
  int n_done = 0;
  for (int i = 0; i < n; i++) {
    AggrSlot *s = aggregator_slot(aggr_, kAggrRingKindInval, slot_workers[i]);
    int rc = send_invalidate_direct((uint32_t)dst, s->key);
    s->result_status = rc;
    s->state.store(kAggrDone, std::memory_order_release);
    n_done++;
  }
  return n_done;
}

#if 0  // iter-11A revival
int CxlKvStoreA::inval_sender_drain_dst_v2_unused(int dst, int n,
                                         const int *slot_workers) {
  if (n <= 0 || n > num_aggr_workers_) return 0;
  // iter-17A: 3D ring matrix — unused/dead code, use shard 0
  InvalRing *ring = &ir_->rings[host_id_][dst][0];
  uint64_t base = ring->tail.fetch_add((uint64_t)n, std::memory_order_acq_rel);
  flush_line((void *)&ring->tail);
  store_fence();

  uint64_t op_ids[kMaxAggrWorkers];
  for (int i = 0; i < n; i++) {
    uint64_t my_op = inval_op_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
    op_ids[i] = encode_op_id(host_id_, my_op);
  }

  for (int i = 0; i < n; i++) {
    uint32_t slot_idx = (uint32_t)((base + i) % kInvalRingDepth);
    InvalEntry *e = &ring->entries[slot_idx];
    AggrSlot *s = aggregator_slot(aggr_, kAggrRingKindInval, slot_workers[i]);
    for (;;) {
      flush_line((void *)&e->req_op_id);
      full_fence();
      if (e->req_op_id.load(std::memory_order_acquire) == 0) break;
      __builtin_ia32_pause();
    }
    e->key = s->key;
    e->resp_op_id.store(0, std::memory_order_relaxed);
    e->status = 0;
    std::atomic_thread_fence(std::memory_order_release);
    e->req_op_id.store(op_ids[i], std::memory_order_release);
    flush_line((void *)&e->req_op_id);
    store_fence();  // ⭐ slot N must commit before slot N+1
  }

  bool done[kMaxAggrWorkers];
  for (int i = 0; i < n; i++) done[i] = false;
  int n_done = 0;
  uint64_t spin_start_ns = now_ns_mono();
  const uint64_t kBudgetUs = 5000;
  while (n_done < n) {
    for (int i = 0; i < n; i++) {
      if (done[i]) continue;
      uint32_t slot_idx = (uint32_t)((base + i) % kInvalRingDepth);
      InvalEntry *e = &ring->entries[slot_idx];
      flush_line((void *)&e->resp_op_id);
      full_fence();
      uint64_t resp = e->resp_op_id.load(std::memory_order_acquire);
      if (resp == op_ids[i]) {
        AggrSlot *s = aggregator_slot(aggr_, kAggrRingKindInval, slot_workers[i]);
        s->result_status = e->status;
        e->req_op_id.store(0, std::memory_order_release);
        flush_line((void *)&e->req_op_id);
        store_fence();
        s->state.store(kAggrDone, std::memory_order_release);
        done[i] = true;
        n_done++;
      }
    }
    if (n_done < n) {
      uint64_t now = now_ns_mono();
      if ((now - spin_start_ns) / 1000 > kBudgetUs) {
        for (int i = 0; i < n; i++) {
          if (done[i]) continue;
          uint32_t slot_idx = (uint32_t)((base + i) % kInvalRingDepth);
          InvalEntry *e = &ring->entries[slot_idx];
          e->req_op_id.store(0, std::memory_order_release);
          flush_line((void *)&e->req_op_id);
          AggrSlot *s = aggregator_slot(aggr_, kAggrRingKindInval, slot_workers[i]);
          s->result_status = -11;
          s->state.store(kAggrDone, std::memory_order_release);
        }
        store_fence();
        break;
      }
      __builtin_ia32_pause();
    }
  }
  return n_done;
}
#endif

// Generic per-policy sender body.
template <int RING_KIND>
void CxlKvStoreA::sender_loop_dispatch(std::atomic<bool> *stop_flag) {
  BatchPolicy policy = parse_batch_policy();
  uint32_t K = parse_batch_k();
  uint64_t T_us = parse_batch_timeout_us();
  fprintf(stderr, "[A:sender] kind=%d policy=P%d K=%u T_us=%lu host_id=%d\n",
          RING_KIND, (int)policy, K, T_us, host_id_);

  int slot_workers[kMaxAggrWorkers];

  // Per-policy drain dispatcher
  auto drain_dst = [this](int dst, int n, const int *workers) -> int {
    if constexpr (RING_KIND == kAggrRingKindWrite) {
      return this->write_sender_drain_dst(dst, n, workers);
    } else if constexpr (RING_KIND == kAggrRingKindRead) {
      return this->read_sender_drain_dst(dst, n, workers);
    } else {
      return this->inval_sender_drain_dst(dst, n, workers);
    }
  };

  uint64_t batch_t_start_ns = now_ns_mono();
  while (!stop_flag->load(std::memory_order_acquire)) {
    int per_dst_n[kForwardStagingMaxHosts] = {0};
    int per_dst_workers[kForwardStagingMaxHosts][kMaxAggrWorkers];

    // Scan all worker slots, group by dst
    for (int w = 0; w < num_aggr_workers_; w++) {
      AggrSlot *s = aggregator_slot(aggr_, RING_KIND, w);
      if (s->state.load(std::memory_order_acquire) != kAggrPending) continue;
      uint32_t dst = s->dst_host;
      if (dst >= kForwardStagingMaxHosts) continue;
      per_dst_workers[dst][per_dst_n[dst]++] = w;
    }

    int total_pending = 0;
    for (int d = 0; d < kForwardStagingMaxHosts; d++)
      total_pending += per_dst_n[d];

    if (total_pending == 0) {
      __builtin_ia32_pause();
      continue;
    }

    if (policy == kBP_P0) {
      // Per-slot serial — fall through to per-policy logic but with K=1
      for (int d = 0; d < kForwardStagingMaxHosts; d++) {
        for (int i = 0; i < per_dst_n[d]; i++) {
          drain_dst(d, 1, &per_dst_workers[d][i]);
        }
      }
    } else if (policy == kBP_P1) {
      // Fixed K + timeout — flush a dst when its pending count >= K
      // OR T_us has elapsed since last flush.
      uint64_t now = now_ns_mono();
      bool timeout_exceeded = (now - batch_t_start_ns) / 1000 >= T_us;
      bool flushed_any = false;
      for (int d = 0; d < kForwardStagingMaxHosts; d++) {
        if (per_dst_n[d] >= (int)K || (timeout_exceeded && per_dst_n[d] > 0)) {
          int n_to_send = per_dst_n[d];
          if (n_to_send > (int)K) n_to_send = (int)K;
          drain_dst(d, n_to_send, per_dst_workers[d]);
          flushed_any = true;
        }
      }
      if (flushed_any) batch_t_start_ns = now_ns_mono();
    } else if (policy == kBP_P2) {
      // Adaptive drain-all — per dst, single fetch_add(N_dst)
      for (int d = 0; d < kForwardStagingMaxHosts; d++) {
        if (per_dst_n[d] > 0) {
          drain_dst(d, per_dst_n[d], per_dst_workers[d]);
        }
      }
    } else {  // kBP_P3
      // Per-dst round-robin: drain ONE dst's full batch per iteration
      // The "round-robin" effect comes from re-scanning next iteration.
      int chosen = -1;
      for (int d = 0; d < kForwardStagingMaxHosts; d++) {
        if (per_dst_n[d] > 0) { chosen = d; break; }
      }
      if (chosen >= 0) {
        drain_dst(chosen, per_dst_n[chosen], per_dst_workers[chosen]);
      }
    }
  }
  (void)slot_workers;
}

void CxlKvStoreA::write_sender_loop() {
  sender_loop_dispatch<kAggrRingKindWrite>(&write_sender_stop_);
}
void CxlKvStoreA::read_sender_loop() {
  sender_loop_dispatch<kAggrRingKindRead>(&read_sender_stop_);
}
void CxlKvStoreA::inval_sender_loop() {
  sender_loop_dispatch<kAggrRingKindInval>(&inval_sender_stop_);
}

int CxlKvStoreA::enable_senders(AggregatorRegion *ar, int num_workers,
                                bool spawn_senders) {
  // iter-17A Phase 4: senders removed.
  // Critical: do NOT wire aggr_ (leave it nullptr). If we did, the
  // forward_write()/forward_read()/send_invalidate() dispatchers
  // would route through aggregator slots and wait forever for sender
  // threads that no longer exist. With aggr_ = nullptr, those
  // dispatchers fall through to *_direct path immediately (which is
  // what we want — workers write CXL ring directly).
  (void)ar; (void)num_workers; (void)spawn_senders;
  num_aggr_workers_ = 0;
  // aggr_ stays nullptr.
  return 0;
}

// iter-17A Phase 4: sender threads removed (see enable_senders).
// Stop functions become no-ops since no thread to join.
void CxlKvStoreA::stop_write_sender() {
  if (!write_sender_.joinable()) return;
  write_sender_stop_.store(true, std::memory_order_release);
  write_sender_.join();
}

void CxlKvStoreA::stop_read_sender() {
  if (!read_sender_.joinable()) return;
  read_sender_stop_.store(true, std::memory_order_release);
  read_sender_.join();
}

void CxlKvStoreA::stop_inval_sender() {
  if (!inval_sender_.joinable()) return;
  inval_sender_stop_.store(true, std::memory_order_release);
  inval_sender_.join();
}

int CxlKvStoreA::assert_n_to_n_active() {
  // iter-9A Phase 2.G — C4 startup assert (replaces obsolete
  // phys_hosts_pr_ check). In multi-host mode the entire 3-ring +
  // staging mesh must be wired before any cross-host op.
  if (num_hosts_ < 2) return 0;
  bool ok = (wr_ != nullptr) && (rr_ != nullptr) &&
            (fs_ != nullptr) && (rs_ != nullptr) && (ir_ != nullptr);
  if (!ok) {
    fprintf(stderr,
            "FATAL [iter-9A C4 + iter-11A C13]: cross-host op attempted "
            "with incomplete N:1:1:N wiring on host %d (num_hosts=%d): "
            "wr_=%p rr_=%p fs_=%p rs_=%p ir_=%p — must call "
            "enable_write_ring + enable_read_ring + enable_invalidate "
            "before first cross-host op\n",
            host_id_, num_hosts_,
            (void *)wr_, (void *)rr_, (void *)fs_, (void *)rs_, (void *)ir_);
    std::abort();
  }
  return 0;
}

// iter-9A Phase 2.A direct path — write-path forward. Splits value
// bytes off the message ring into ForwardStaging arena (C2 compliance).
int CxlKvStoreA::forward_write_direct(uint32_t owner, uint64_t key,
                                      const void *value, uint32_t value_len,
                                      int op_kind) {
  if (!wr_ || !fs_) return -10;
  if (value_len > kForwardStagingSlotBytes) return -5;
  PATH_CTR(n_forward_write);

#if FUSEE_XHOST_WRITE_SELF_INVAL
  // iter-14A Phase 2: self-invalidate local cache BEFORE forwarding.
  // Receiver excludes us from invalidate broadcast (saves 1 cross-host
  // roundtrip per write). MUST be sequenced before WriteEntry enqueue;
  // x86 program-order is sufficient (both are stores, release-default).
  // Safe regardless of whether `key` was actually cached locally —
  // set_stale + tls_evict are idempotent / no-op on miss.
  //
  // iter-16A status (2026-05-21): DEAD CODE in default builds.
  // FUSEE_XHOST_WRITE_SELF_INVAL defaults to 0 in cxl_read_guard.h:86;
  // current production builds do not override. Block compiles out entirely.
  // Kept as opt-in flag for future experiment (no maintenance cost).
#if !FUSEE_DISABLE_CACHE_POOL
  if (cache_) cache_pool_set_stale(cache_, key);
#endif
#if !FUSEE_DISABLE_TLS
  if (g_thread_tls) tls_evict(g_thread_tls, key);
#endif
#endif  // FUSEE_XHOST_WRITE_SELF_INVAL

  // iter-17A: route to ring shard. Plan A (default) uses worker_id;
  // Plan B (env FUSEE_RING_ROUTING=key_hash) uses fnv1a(key).
  int ring_idx = compute_ring_idx(key);
  WriteRing *ring = &wr_->rings[host_id_][owner][ring_idx];
  uint64_t my_op = write_op_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
  uint64_t op_id = encode_op_id(host_id_, my_op);

  // iter-16A Stage 1 (slot_reserve) start.
  PROBE_OP("XWS1S", op_id);

  // Reserve slot.
  uint64_t tpos = ring->tail.fetch_add(1, std::memory_order_acq_rel);
  flush_line((void *)&ring->tail);
  store_fence();
  uint32_t slot_idx = (uint32_t)(tpos % kWriteRingDepth);
  WriteEntry *e = &ring->entries[slot_idx];

  // iter-16A Stage 1 end ≡ Stage 2 (slot_wait) start.
  PROBE_OP("XWS1E", op_id);

  // Wait for slot free (cacheline 1 holds req_op_id).
  // iter-16A Stage 2 micro-opt: lfence is sufficient to order clflushopt
  // before the subsequent load (Intel manual: clflushopt ordered w/r/t
  // following loads by LFENCE). mfence is overkill here — saves ~10-20 ns/op.
  int c_iters = 0;
  for (;;) {
    flush_line((void *)&e->req_op_id);
    __builtin_ia32_lfence();
    if (e->req_op_id.load(std::memory_order_acquire) == 0) break;
    c_iters++;
    __builtin_ia32_pause();
  }

  // iter-16A Stage 2 end ≡ Stage 3 (value_xfer) start.
  PROBE_OP("XWS2E", op_id);
  if (c_iters > 0) {
    // Conditional probe — emits ONLY if Stage 2 actually spun.
    // Payload = number of spin iterations (NOT op_id).
    PROBE_OP("XWS2R", (uint64_t)c_iters);
  }

  // iter-13A Phase 2: select write-path based on FUSEE_WRITE_ALLOC.
  uint64_t direct_blk_off = 0;
  bool direct_used = false;
#if FUSEE_WRITE_ALLOC == FUSEE_WRITE_ALLOC_RESERVED
  // W1: bump-allocate from owner's reserved-for-me sub-segment (local DRAM).
  if (value && value_len > 0 && op_kind != kOpKindDelete && pool_) {
    direct_blk_off = pool_->alloc_peer((int)owner);
  }
#elif FUSEE_WRITE_ALLOC == FUSEE_WRITE_ALLOC_BATCHED
  // W3: per-(thread, owner) DRAM queue; refill via ReservationRing.
  if (value && value_len > 0 && op_kind != kOpKindDelete && rsv_ && pool_) {
    static thread_local uint64_t q_blk_offs[kReservMaxHosts][kReservMaxBatchK];
    static thread_local uint32_t q_next_idx[kReservMaxHosts] = {0};
    static thread_local uint32_t q_filled[kReservMaxHosts] = {0};
    // Read batch size K from env once per thread.
    static thread_local uint32_t batch_k = 0;
    if (batch_k == 0) {
      const char *e_k = getenv("FUSEE_BATCH_K");
      batch_k = (e_k && atoi(e_k) > 0) ? (uint32_t)atoi(e_k) : 128;
      if (batch_k > kReservMaxBatchK) batch_k = kReservMaxBatchK;
    }
    if (q_next_idx[owner] >= q_filled[owner]) {
      // Queue empty: send reservation request, wait, copy K blk_offs.
      uint64_t r_my_op = my_op | 0x8000000000000000ULL;  // mark as reserve req
      uint64_t r_tpos = rsv_->tails[host_id_][owner].fetch_add(1,
                            std::memory_order_acq_rel);
      flush_line(&rsv_->tails[host_id_][owner]);
      store_fence();
      uint32_t r_slot = (uint32_t)(r_tpos % kReservRingDepth);
      ReservationEntry *re = reservation_entry(rsv_, host_id_, (int)owner,
                                                (int)r_slot);
      // Wait for slot free.
      for (;;) {
        flush_line(&re->req_op_id);
        full_fence();
        if (re->req_op_id.load(std::memory_order_acquire) == 0) break;
        __builtin_ia32_pause();
      }
      re->batch_k = batch_k;
      re->filled_count = 0;
      re->status = 0;
      re->resp_op_id.store(0, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_release);
      re->req_op_id.store(r_my_op, std::memory_order_release);
      flush_line(&re->req_op_id);
      store_fence();
      // Spin on resp_op_id.
      for (;;) {
        flush_line(&re->resp_op_id);
        full_fence();
        if (re->resp_op_id.load(std::memory_order_acquire) == r_my_op) break;
        __builtin_ia32_pause();
      }
      // Pull K blk_offs from response. Flush response cachelines first.
      flush_line(&re->filled_count);
      for (uint32_t off = 0; off < batch_k * 8; off += 64) {
        flush_line((char *)re->blk_offs + off);
      }
      full_fence();
      uint32_t got = re->filled_count;
      for (uint32_t i = 0; i < got; i++) q_blk_offs[owner][i] = re->blk_offs[i];
      q_filled[owner] = got;
      q_next_idx[owner] = 0;
      // Free request slot.
      re->req_op_id.store(0, std::memory_order_release);
      flush_line(&re->req_op_id);
      store_fence();
    }
    if (q_next_idx[owner] < q_filled[owner]) {
      direct_blk_off = q_blk_offs[owner][q_next_idx[owner]++];
    }
  }
#endif
#if FUSEE_WRITE_ALLOC != FUSEE_WRITE_ALLOC_STAGING
  if (direct_blk_off != 0) {
    // Direct path: write value bytes into the pool (header + value).
    uint8_t hdr_buf[4];
    std::memcpy(hdr_buf, &value_len, 4);
    pool_->write(direct_blk_off, hdr_buf, 4);
    pool_->write(direct_blk_off + 4, value, value_len);
    direct_used = true;
  }
  if (!direct_used) {
    // Fallback STAGING.
    if (value && value_len > 0) {
      uint8_t *staging =
          forward_staging_bytes(fs_, host_id_, (int)owner, (int)slot_idx);
      std::memcpy(staging, value, value_len);
      for (uint32_t off = 0; off < value_len; off += 64) {
        flush_line((void *)(staging + off));
      }
      store_fence();
    }
  }
#else
  // STAGING (default): copy value bytes into the staging arena slot
  // (op_kind=DELETE skips this — value_len == 0).
  if (value && value_len > 0) {
    uint8_t *staging =
        forward_staging_bytes(fs_, host_id_, (int)owner, (int)slot_idx);
    std::memcpy(staging, value, value_len);
    for (uint32_t off = 0; off < value_len; off += 64) {
      flush_line((void *)(staging + off));
    }
    store_fence();
  }
#endif

  // iter-16A Stage 3 end ≡ Stage 4 (ctrl_publish) start.
  PROBE_OP("XWS3E", op_id);

  // Fill control message (cacheline 1).
  e->key = key;
  e->op_kind = (uint8_t)op_kind;
  e->value_len = value_len;
#if FUSEE_WRITE_ALLOC != FUSEE_WRITE_ALLOC_STAGING
  // iter-13A Phase 2 W1/W3: reuse staging_off field to carry blk_off in
  // direct-pool-write mode. staging_gen=0 signals "use blk_off"; =1
  // signals "fallback to staging_off as slot index" (staging copy used).
  if (direct_used) {
    e->staging_off = direct_blk_off;
    e->staging_gen = 0;
  } else {
    e->staging_off = slot_idx;
    e->staging_gen = 1;
  }
#else
  e->staging_off = slot_idx;          // sanity check; receiver asserts
  e->staging_gen = 0;                 // reserved (iter-10A pool-gen)
#endif
  // iter-17A Stage 4 audit: these two resets LOOK dead (op_id is
  // host-tagged so prev resp can't collide; status only read after
  // resp match → receiver always overwrites it). Removal smoke at
  // T=1/8/64 measured -5% / -60% / -40% throughput regression with
  // w_p99 inflated to 1.85-2.37 ms. Root cause not pinpointed (likely
  // CXL inter-host clflushopt writeback race — see iter-9A
  // CXL-atomic-flush memory). EMPIRICALLY REQUIRED — do not remove
  // without re-running 4-path microbench + xhost study.
  e->resp_op_id.store(0, std::memory_order_relaxed);
  e->status = 0;
  std::atomic_thread_fence(std::memory_order_release);

  e->req_op_id.store(op_id, std::memory_order_release);
  flush_line((void *)&e->req_op_id);  // publish cacheline 1
  store_fence();

  // iter-16A Stage 4 end ≡ Stage 5 (ack_wait) start.
  PROBE_OP("XWS4E", op_id);

  int status = 0;
  int rc = generic_spin_wait(e, op_id, &status);

  // iter-16A Stage 5 end. XWS5E always fires; XWS5T conditional on timeout
  // (timeout indicates receiver pathology — investigate occurrences).
  PROBE_OP("XWS5E", op_id);
  if (rc != 0) {
    PROBE_OP("XWS5T", op_id);
  }

  if (rc != 0) return rc;
  return status;
}

// iter-5A: send OP_INVALIDATE on the SEPARATE InvalRing channel.
// Producer reserves a slot via fetch_add(tail) + flush, writes the
// key, and spins on resp_op_id (which the dispatcher_loop on the
// target host will set). No interaction with the WriteRing or ReadRing.
//
// iter-9A Phase 2.C: this is the DIRECT path. Workers call the
// public send_invalidate which routes through the aggregator if
// enabled. Receivers (write_handler → execute_write_local)
// ALWAYS call this direct version (they are not workers).
int CxlKvStoreA::send_invalidate_direct(uint32_t target_host, uint64_t key) {
  if (!ir_) return -10;  // invalidate channel not enabled
  // iter-17A: Plan A worker_id / Plan B key_hash routing.
  int ring_idx = compute_ring_idx(key);
  InvalRing *ring = &ir_->rings[host_id_][target_host][ring_idx];
  uint64_t my_op = inval_op_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
  uint64_t op_id = ((uint64_t)(host_id_ + 1) << 56) |
                   (my_op & 0x00FFFFFFFFFFFFFFULL);

  uint64_t tpos = ring->tail.fetch_add(1, std::memory_order_acq_rel);
  flush_line((void *)&ring->tail);
  store_fence();
  PROBE_PATH("I1", op_id);
  uint32_t slot = (uint32_t)(tpos % kInvalRingDepth);
  InvalEntry *e = &ring->entries[slot];

  // Wait for slot free.
  for (;;) {
    flush_line((void *)e);
    full_fence();
    if (e->req_op_id.load(std::memory_order_acquire) == 0) break;
    __builtin_ia32_pause();
  }

  e->key = key;
  e->resp_op_id.store(0, std::memory_order_relaxed);
  e->status = 0;
  std::atomic_thread_fence(std::memory_order_release);
  e->req_op_id.store(op_id, std::memory_order_release);
  flush_line((void *)e);
  store_fence();
  PROBE_PATH("I2", op_id);

  // Spin on response.
  // iter-6A: resp_op_id lives on the SECOND cacheline of InvalEntry —
  // flush_line must target that line, not (void*)e (which is line 1).
  // Timeout reduced from 200ms (iter-5A) to 5ms — the cacheline split
  // alone does not eliminate the rare ACK-not-observed pattern; bound
  // p99 at ≤ 5ms so total throughput regression is also bounded. Root
  // cause investigation deferred to iter-7A.
  const uint64_t kBudgetUs = 5000;
  uint64_t spin_start_ns = 0;
  for (;;) {
    flush_line((void *)&e->resp_op_id);
    full_fence();
    uint64_t resp = e->resp_op_id.load(std::memory_order_acquire);
    if (resp == op_id) {
      PROBE_PATH("I7", op_id);
      int rc = e->status;
      e->req_op_id.store(0, std::memory_order_release);
      flush_line((void *)e);  // line 1 (req_op_id)
      store_fence();
      PROBE_PATH("I8", op_id);
      return rc;
    }
    if (spin_start_ns == 0) {
      timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
      spin_start_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    } else {
      timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
      uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
      if ((now_ns - spin_start_ns) / 1000 > kBudgetUs) {
        e->req_op_id.store(0, std::memory_order_release);
        static std::atomic<uint64_t> g_inval_timeout_count{0};
        uint64_t t = g_inval_timeout_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((t & 0xFF) == 1) {
          fprintf(stderr, "[A] inval timeout #%lu (key=%lx tpos=%lu)\n",
                  t, key, tpos);
        }
        return -11;
      }
    }
    __builtin_ia32_pause();
  }
}

int CxlKvStoreA::enable_reservation_ring(ReservationRingMatrix *rsv,
                                         bool init_region,
                                         bool spawn_handler) {
  if (!rsv) return -1;
  rsv_ = rsv;
  // iter-12A Phase 5 always-memset-flush pattern.
  std::memset(rsv, 0, reservation_ring_matrix_bytes());
  flush_region(rsv, reservation_ring_matrix_bytes());
  store_fence();
  (void)init_region;
  if (spawn_handler) {
    rsv_handler_stop_.store(false, std::memory_order_relaxed);
    rsv_handler_ = std::thread([this]() {
      pthread_setname_np(pthread_self(), "ReservHandler");
      // iter-17A: pin to last CPU (85) — last receiver thread also at 85
      // in T=64 N=4 packing case, but ReservHandler is mostly idle under
      // FUSEE_WRITE_ALLOC=RESERVED (only services W3 BATCHED requests).
      // Pinning it to 85 prevents it from drifting onto worker CPUs and
      // stealing cycles. Tail-CPU sharing with InvalRecv6 is acceptable
      // since ReservHandler is idle.
      long nproc = sysconf(_SC_NPROCESSORS_ONLN);
      int rsv_cpu = (nproc > 0) ? (int)nproc - 1 : 85;
      cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(rsv_cpu, &cs);
      pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
      fprintf(stderr,
        "[A:thread] ReservHandler pid=%d tid=%lu pinned cpu=%d (host_id=%d)\n",
        getpid(), (unsigned long)pthread_self(), rsv_cpu, host_id_);
      this->reservation_handler_loop();
    });
  }
  return 0;
}

void CxlKvStoreA::stop_reservation_handler() {
  if (!rsv_handler_.joinable()) return;
  rsv_handler_stop_.store(true, std::memory_order_release);
  rsv_handler_.join();
}

// iter-13A Phase 2 W3: handler loop on owner side. For each reservation
// request (from peer P), call pool_->alloc K times, return blk_offs in
// the same entry, publish resp_op_id.
void CxlKvStoreA::reservation_handler_loop() {
  while (!rsv_handler_stop_.load(std::memory_order_acquire)) {
    bool did_work = false;
    for (int src = 0; src < num_hosts_; src++) {
      if (src == host_id_) continue;
      // Tail tracks the producer (peer) head; consumer (us) scans tail-head.
      flush_line(&rsv_->tails[src][host_id_]);
      full_fence();
      uint64_t tail = rsv_->tails[src][host_id_].load(std::memory_order_acquire);
      // Simple per-(src,me) head counter in DRAM (only this thread reads).
      static thread_local uint64_t local_heads[kReservMaxHosts][kReservMaxHosts] = {{0}};
      uint64_t head = local_heads[src][host_id_];
      while (head < tail) {
        uint32_t slot = (uint32_t)(head % kReservRingDepth);
        ReservationEntry *e = reservation_entry(rsv_, src, host_id_, (int)slot);
        flush_line((void *)&e->req_op_id);
        full_fence();
        uint64_t op_id = e->req_op_id.load(std::memory_order_acquire);
        // gap-tolerance (per iter-12A Phase 5.1 pattern)
        if (op_id == 0) {
          for (int g = 0; g < 4096 && op_id == 0; g++) {
            __builtin_ia32_pause();
            flush_line((void *)&e->req_op_id);
            full_fence();
            op_id = e->req_op_id.load(std::memory_order_acquire);
          }
          if (op_id == 0) break;
        }
        uint32_t K = e->batch_k;
        if (K > kReservMaxBatchK) K = kReservMaxBatchK;
        uint32_t filled = 0;
        for (uint32_t k = 0; k < K; k++) {
          uint64_t bo = pool_ ? pool_->alloc_local() : 0;
          if (bo == 0) break;
          e->blk_offs[k] = bo;
          filled++;
        }
        e->filled_count = filled;
        e->status = (filled == K) ? 0 : -1;
        // Flush blk_offs payload before publishing resp_op_id.
        for (uint32_t off = 0; off < filled * 8; off += 64) {
          flush_line((char *)e->blk_offs + off);
        }
        flush_line(&e->status);
        store_fence();
        e->resp_op_id.store(op_id, std::memory_order_release);
        flush_line((void *)&e->resp_op_id);
        store_fence();
        head++;
        did_work = true;
      }
      local_heads[src][host_id_] = head;
    }
    if (!did_work) __builtin_ia32_pause();
  }
}

int CxlKvStoreA::enable_read_guard(RcuDomain *rcu, HazardDomain *haz,
                                   bool init_region) {
  if (!rcu || !haz) return -1;
  rcu_ = rcu;
  haz_ = haz;
  // iter-12A Phase 5.1c pattern: always memset+flush so host 1's stale
  // dirty cache lines from prior process get invalidated.
  std::memset(rcu, 0, rcu_domain_bytes());
  flush_region(rcu, rcu_domain_bytes());
  std::memset(haz, 0, hazard_domain_bytes());
  flush_region(haz, hazard_domain_bytes());
  store_fence();
  (void)init_region;
  return 0;
}

int CxlKvStoreA::enable_invalidate(InvalRingMatrix *ir, bool init_region,
                                   bool spawn_dispatcher) {
  if (!ir) return -1;
  ir_ = ir;
  // iter-12A Phase 5.1c: see comment in enable_write_ring above.
  std::memset(ir, 0, inval_ring_matrix_bytes());
  flush_region(ir, inval_ring_matrix_bytes());
  store_fence();
  (void)init_region;
  if (spawn_dispatcher) {
    // iter-11A Phase 2 reverted (commit 455379e): the
    // InvalDispatcher + 8 InvalWorker design passed hash-diff but
    // implementation regressed w_p99 26× (from ~25 µs to 700-900 µs)
    // due to DRAM queue handoff overhead between dispatcher and
    // worker. The plan's predicted gain (591 µs → 50 µs) was based
    // on a misinterpretation of iter-10A's I6 measurement (I6 was
    // receiver IDLE-GAP between bursts, not processing time per
    // inval; actual per-inval handle time is ~1.5 µs in single-
    // thread design). Restored to iter-10A single-thread receiver.
    // Proper redesign of parallel inval (lower-overhead handoff,
    // measurement-driven detection of bursts) deferred to
    // iter-12A backlog #8.
    inval_receiver_stop_.store(false, std::memory_order_relaxed);
    int T = g_num_workers;
    int S = g_actual_ring_shards;
    ReceiverLayout L = compute_receiver_layout(T, S);
    inval_receivers_.reserve(L.inval_plans.size());
    for (size_t i = 0; i < L.inval_plans.size(); i++) {
      ReceiverPlan p = L.inval_plans[i];
      int idx = (int)i;
      inval_receivers_.emplace_back([this, p, idx]() {
        char nm[16]; snprintf(nm, sizeof(nm), "InvalRecv%d", idx);
        pthread_setname_np(pthread_self(), nm);
        cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(p.cpu, &cs);
        pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
        fprintf(stderr,
          "[A:thread] %s pid=%d tid=%lu cpu=%d host=%d rings=[",
          nm, getpid(), (unsigned long)pthread_self(), p.cpu, host_id_);
        for (size_t k = 0; k < p.ring_indices.size(); k++) {
          fprintf(stderr, "%s%d", k ? "," : "", p.ring_indices[k]);
        }
        fprintf(stderr, "]\n");
        this->inval_receiver_loop(p.ring_indices);
      });
    }
  }
  return 0;
}

void CxlKvStoreA::stop_inval_receiver() {
  if (inval_receivers_.empty()) return;
  inval_receiver_stop_.store(true, std::memory_order_release);
  for (auto &t : inval_receivers_) {
    if (t.joinable()) t.join();
  }
  inval_receivers_.clear();
}

// InvalReceiver: drain incoming InvalRing[*][me]; for each entry mark
// the key stale in the local cache_pool and ACK via resp_op_id.
// CRUCIALLY this thread does NOT acquire the directory spinlock and
// does NOT call execute_write_local — so it cannot deadlock with
// the WriteReceiver/ReadReceiver that are processing forwards.
//
// iter-11A Phase 2 reverted (455379e → restored): single-thread
// processing has lower per-op latency (~1.5 µs) than the parallel
// dispatcher+worker design (3-5 µs/op + handoff tail) when invals
// are not bursty enough to saturate a single thread.
void CxlKvStoreA::inval_receiver_loop(std::vector<int> ring_indices) {
  probe_ring();
  while (!inval_receiver_stop_.load(std::memory_order_acquire)) {
    bool did_work = false;
   for (int ring_idx : ring_indices) {
    for (int src = 0; src < num_hosts_; src++) {
      if (src == host_id_) continue;
      InvalRing *ring = &ir_->rings[src][host_id_][ring_idx];
      uint64_t head = ring->head;
      flush_line((void *)&ring->tail);
      full_fence();
      uint64_t tail = ring->tail.load(std::memory_order_acquire);
      while (head < tail) {
        PROBE_PATH("I3", tail);
        uint32_t slot = (uint32_t)(head % kInvalRingDepth);
        InvalEntry *e = &ring->entries[slot];
        flush_line((void *)e);
        full_fence();
        uint64_t op_id = e->req_op_id.load(std::memory_order_acquire);
        // iter-12A bimodal fix: gap-tolerance budget before bailing.
        // The producer just did fetch_add(tail) on this ring; if it
        // hasn't yet written req_op_id, that's a brief publish gap
        // (CPU 0 OS jitter, CXL store visibility window). Spinning in
        // place is far cheaper than letting the producer's
        // generic_spin_wait time out on its side.
        if (op_id == 0) {
          for (int gap_iter = 0; gap_iter < 4096 && op_id == 0; gap_iter++) {
            __builtin_ia32_pause();
            flush_line((void *)e);
            full_fence();
            op_id = e->req_op_id.load(std::memory_order_acquire);
          }
          if (op_id == 0) break;
        }
        PROBE_PATH("I4", op_id);
#if !FUSEE_DISABLE_CACHE_POOL
        cache_pool_set_stale(cache_, e->key);
#endif
        PROBE_PATH("I5", op_id);
        e->status = 0;
        std::atomic_thread_fence(std::memory_order_release);
        e->resp_op_id.store(op_id, std::memory_order_release);
        flush_line((void *)&e->resp_op_id);
        store_fence();
        PROBE_PATH("I6", op_id);
        head++;
        did_work = true;
      }
      ring->head = head;
    }
   }  // for ring_idx
    if (!did_work) __builtin_ia32_pause();
  }
  probe_flush();
}

// iter-9A Phase 2.A direct path — read-path register-then-fill via
// ReadRing. Producer enqueues control-only req; ReadReceiver responds
// with (status, owner_blk_off, value_len). Reader pulls value bytes
// directly via pool_->read — no value bytes on the message ring.
// iter-11A Phase 1 forwarder-pool-direct reader path:
//   1. Allocate ReadRing slot via fetch_add(tail). Wait for slot
//      to be free (req_op_id == 0).
//   2. Capture my_epoch_at_send = bucket_epoch(key). Used by C13
//      validation post-response.
//   3. CLEAR staging.ready_op_id = 0 (so we don't observe the
//      previous user of this slot's stale signal).
//   4. Write ReadEntry{key, req_op_id}, flush, sfence.
//   5. Spin on staging.ready_op_id == req_op_id. Owner forwarder
//      writes value bytes + lookup_epoch + status, then publishes
//      ready_op_id.
//   6. C13 validate: staging.lookup_epoch >= my_epoch_at_send.
//      Stale → retry (loop back to step 1).
//   7. Copy staging.value_bytes to out_buf. No second pool->read.
int CxlKvStoreA::forward_read_direct(uint32_t owner, uint64_t key,
                                     void *out_buf, uint32_t buf_len,
                                     uint32_t *out_len) {
  if (!rr_ || !rs_) return -10;
  // iter-17A: Plan A worker_id / Plan B key_hash routing.
  int ring_idx = compute_ring_idx(key);
  ReadRing *ring = &rr_->rings[host_id_][owner][ring_idx];
  uint64_t my_op = read_op_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
  uint64_t op_id = encode_op_id(host_id_, my_op);

  // iter-18A Stage 1 (slot_reserve) start.
  PROBE_READ_OP("XRS1S", op_id);

  uint64_t tpos = ring->tail.fetch_add(1, std::memory_order_acq_rel);
  flush_line((void *)&ring->tail);
  store_fence();
  uint32_t slot_idx = (uint32_t)(tpos % kReadRingDepth);
  ReadEntry *e = &ring->entries[slot_idx];

  // iter-18A Stage 1 end ≡ Stage 2 (slot_wait) start.
  PROBE_READ_OP("XRS1E", op_id);

  int c_iters = 0;
  for (;;) {
    flush_line((void *)&e->req_op_id);
    full_fence();
    if (e->req_op_id.load(std::memory_order_acquire) == 0) break;
    c_iters++;
    __builtin_ia32_pause();
  }

  // iter-18A Stage 2 end ≡ Stage 3 (req_publish) start.
  PROBE_READ_OP("XRS2E", op_id);
  if (c_iters > 0) PROBE_READ_OP("XRS2R", (uint64_t)c_iters);

#if FUSEE_READ_GUARD == FUSEE_READ_GUARD_RCU
  // iter-13A Phase 1 (RCU): publish my reader epoch BEFORE sending the
  // request. Owner cannot reclaim a block I might subsequently observe
  // by blk_off until rcu_synchronize() past this epoch.
  if (rcu_) rcu_enter(rcu_, host_id_, (int)(g_aggr_worker_id >= 0 ? g_aggr_worker_id : 0));
#endif

  // C13: my_epoch_at_send (bucket epoch reader observed prior to send).
  uint64_t my_epoch_at_send =
      cache_ ? cache_pool_bucket_epoch(cache_, key) : 0;

  // Clear staging slot's ready_op_id so we don't observe prior
  // user's stale signal. Owner will re-publish with our op_id.
  ReadStagingSlot *st =
      read_staging_slot(rs_, host_id_, (int)owner, (int)slot_idx);
  st->ready_op_id.store(0, std::memory_order_release);
  flush_line(&st->ready_op_id);
  store_fence();

  e->key = key;
  e->resp_op_id.store(0, std::memory_order_relaxed);
  e->status = 0;
  e->resp_value_len = 0;
  e->resp_blk_off = 0;
  std::atomic_thread_fence(std::memory_order_release);
  e->req_op_id.store(op_id, std::memory_order_release);
  flush_line((void *)&e->req_op_id);
  store_fence();

  // iter-18A Stage 3 end ≡ Stage 4 (ack_wait) start.
  PROBE_READ_OP("XRS3E", op_id);

  // Spin on staging.ready_op_id (single cacheline, faster than
  // the legacy 2-step ack-then-pool-read).
  // iter-18A C3: pause 4× between flushes to reduce CXL polling noise
  // (peer's ack write needs ≥600 ns to propagate; aggressive flushing
  // creates bus contention with peer's store).
  const uint64_t kReadSpinTimeoutNs = 200ULL * 1000 * 1000;  // 200 ms
  uint64_t t0 = now_ns_mono();
  bool timed_out = false;
  for (;;) {
    flush_line(&st->ready_op_id);
    full_fence();
    uint64_t r = st->ready_op_id.load(std::memory_order_acquire);
    if (r == op_id) break;
    if (now_ns_mono() - t0 > kReadSpinTimeoutNs) {
      timed_out = true;
      break;
    }
    __builtin_ia32_pause(); __builtin_ia32_pause();
    __builtin_ia32_pause(); __builtin_ia32_pause();
  }

  // iter-18A Stage 4 end ≡ Stage 5 (post_ack_cleanup_and_validate) start.
  PROBE_READ_OP("XRS4E", op_id);
  if (timed_out) PROBE_READ_OP("XRS4T", op_id);
  // CRITICAL: free the ring slot by clearing req_op_id=0 BEFORE
  // returning. Otherwise the slot stays "busy" and the next
  // wraparound deadlocks at fetch_add+spin-on-zero in this same
  // function. (Legacy reader path did this inside generic_spin_wait;
  // we replaced it with the staging poll above and forgot to free
  // the slot — caused 100% hang at >256 reads per (req_host, owner)
  // pair, exposed by Phase 1 hash-diff battery 2026-05-10.)
  e->req_op_id.store(0, std::memory_order_release);
  flush_line((void *)&e->req_op_id);
  store_fence();
  if (timed_out) {
#if FUSEE_READ_GUARD == FUSEE_READ_GUARD_RCU
    if (rcu_) rcu_exit(rcu_, host_id_, (int)(g_aggr_worker_id >= 0 ? g_aggr_worker_id : 0));
#endif
    if (out_len) *out_len = 0;
    return -2;
  }

  // C13: validate lookup_epoch >= my_epoch_at_send. If stale, the
  // forwarder's lookup observed an OLDER cache snapshot than ours
  // (highly unlikely on shared-bus CXL, but defensive). Treat as
  // miss + retry (caller will fall back via cache_pool_lookup).
  flush_line(st);
  full_fence();
  if (st->lookup_epoch < my_epoch_at_send) {
#if FUSEE_READ_GUARD == FUSEE_READ_GUARD_RCU
    if (rcu_) rcu_exit(rcu_, host_id_, (int)(g_aggr_worker_id >= 0 ? g_aggr_worker_id : 0));
#endif
    if (out_len) *out_len = 0;
    return -3;  // stale snapshot — caller retries
  }
  if (st->status != 0) {
#if FUSEE_READ_GUARD == FUSEE_READ_GUARD_RCU
    if (rcu_) rcu_exit(rcu_, host_id_, (int)(g_aggr_worker_id >= 0 ? g_aggr_worker_id : 0));
#endif
    if (out_len) *out_len = 0;
    return st->status;
  }
  uint32_t vlen = st->value_size;
  if (vlen == 0) {
#if FUSEE_READ_GUARD == FUSEE_READ_GUARD_RCU
    if (rcu_) rcu_exit(rcu_, host_id_, (int)(g_aggr_worker_id >= 0 ? g_aggr_worker_id : 0));
#endif
    if (out_len) *out_len = 0;
    return 0;
  }
  if (vlen > kReadStagingSlotBytes) {
#if FUSEE_READ_GUARD == FUSEE_READ_GUARD_RCU
    if (rcu_) rcu_exit(rcu_, host_id_, (int)(g_aggr_worker_id >= 0 ? g_aggr_worker_id : 0));
#endif
    if (out_len) *out_len = 0;
    return -1;
  }
  // iter-18A Stage 5 end ≡ Stage 6 (value_recv) start.
  PROBE_READ_OP("XRS5E", op_id);
#if FUSEE_READ_GUARD == FUSEE_READ_GUARD_STAGING
  // STAGING (default): direct copy from CXL staging — owner placed
  // value bytes into st->value_bytes in read_handler (1× CXL→DRAM→CXL
  // extra copy on owner side, but reader does one bulk read here).
  for (uint32_t off = 0; off < vlen; off += 64) {
    flush_line(st->value_bytes + off);
  }
  full_fence();
  uint32_t copy_len = vlen < buf_len ? vlen : buf_len;
  if (out_buf && copy_len > 0) {
    std::memcpy(out_buf, st->value_bytes, copy_len);
  }
#else
  // RCU / HAZARD: direct pool read. Staging only carries control fields
  // (blk_off, vlen, lookup_epoch, status) — no value bytes copy on owner.
  // st->resp_blk_off was populated by read_handler with the actual pool
  // blk_off (high bit signals inline-8B fallback; see read_handler for
  // the encoding shared with the legacy path).
  // ABA note: current pool is bump-only (free_lazy is a stub), so the
  // blk_off captured here cannot be re-allocated to a different key
  // during this read. iter-14A+ freelist GC will need a generation tag
  // in the encoded slot value (use the high bits of cxl_slot_pack).
  uint64_t blk_off = st->resp_blk_off;
  int tid = (int)(g_aggr_worker_id >= 0 ? g_aggr_worker_id : 0);
  (void)tid;
#if FUSEE_READ_GUARD == FUSEE_READ_GUARD_HAZARD
  // Hazard: publish blk_off to my hazard slot BEFORE the pool read.
  // (Re-validation against the bucket isn't possible here — reader
  // doesn't share the bucket with owner. ABA protection deferred to
  // iter-14A per RAP §V_CORRECTNESS Attack 5 defense.)
  if (haz_) hazard_protect(haz_, host_id_, tid, blk_off);
#endif
  if (blk_off == 0 || pool_ == nullptr) {
    // Inline-8B fallback: value was packed into st->resp_blk_off itself
    // by read_handler (see kSizeClassInline branch). Copy directly.
    uint64_t encoded = st->resp_blk_off;
    uint32_t copy_len = vlen < buf_len ? vlen : buf_len;
    if (out_buf && copy_len > 0) {
      std::memcpy(out_buf, &encoded, copy_len);
    }
  } else {
    // True pool block: skip 4B header, read vlen value bytes directly
    // from CXL into out_buf (caller's DRAM).
    uint32_t copy_len = vlen < buf_len ? vlen : buf_len;
    if (out_buf && copy_len > 0) {
      pool_->read(blk_off + 4, out_buf, copy_len);
    }
  }
#if FUSEE_READ_GUARD == FUSEE_READ_GUARD_HAZARD
  if (haz_) hazard_release(haz_, host_id_, tid);
#endif
#if FUSEE_READ_GUARD == FUSEE_READ_GUARD_RCU
  if (rcu_) rcu_exit(rcu_, host_id_, tid);
#endif
#endif  // FUSEE_READ_GUARD branches
  if (out_len) *out_len = vlen;
  // iter-18A Stage 6 (value_recv) end.
  PROBE_READ_OP("XRS6E", op_id);
  return 0;
}

void CxlKvStoreA::write_handler(WriteEntry *e, int src) {
  // L3: ack only — skip ALL work, just return. Caller's loop publishes
  // resp_op_id; we just need a status set.
  int nlevel = recv_noop_level();
  if (nlevel >= 3) {
    e->status = 0;
    return;
  }
  // L2: CXL slot publish + ack only — write a dummy CoW pointer into a
  // fixed slot to simulate the publish cost (1 CXL cacheline + flush_line
  // + sfence). Skip bucket scan, directory state, invalidate broadcast.
  // NOT semantically valid (slot 0 reused arbitrarily); measures publish
  // overhead only.
  if (nlevel >= 2) {
    if (e->op_kind != kOpKindDelete && e->staging_gen == 0 && buckets_) {
      uint32_t b = bucket_idx(e->key);
      CxlKvBucket *bucket = &buckets_[b];
      uint64_t encoded = cxl_slot_pack(e->staging_off, kSizeClassBlock256,
                                       key_fingerprint(e->key));
      bucket->slots[0].value = encoded;
      flush_line(&bucket->slots[0]);
      store_fence();
    }
    e->status = 0;
    return;
  }
  uint32_t value_len = e->value_len;
  // iter-14A Phase 2: forward src host to execute_write_local; if
  // FUSEE_XHOST_WRITE_SELF_INVAL is on, src has already self-invalidated
  // its local cache, so we exclude src from the invalidate broadcast.
  // When flag is off, the src parameter is silently ignored — behavior
  // matches iter-13A baseline.
  // For L1, src is forwarded to execute_write_local_with_blk /
  // execute_write_local; those functions check recv_noop_level() and
  // skip the invalidate broadcast inner loop.
  if (e->op_kind == kOpKindDelete) {
    e->status = execute_write_local(e->key, nullptr, 0, kOpKindDelete, src);
    return;
  }
  if (value_len == 0 || value_len > kForwardStagingSlotBytes) {
    e->status = -5;
    return;
  }
#if FUSEE_WRITE_ALLOC != FUSEE_WRITE_ALLOC_STAGING
  // iter-13A Phase 2 W1/W3: if worker took the direct-pool path
  // (staging_gen==0), the blk_off is already in owner's CXL pool — we
  // just need to wire it into the bucket without re-allocating or
  // copying. If worker fell back to staging (staging_gen==1), use the
  // legacy path below.
  if (e->staging_gen == 0) {
    // Direct-pool path: blk_off in e->staging_off; skip pool->alloc and
    // pool->write. Need a code path that just updates bucket->slots[i].value
    // = encode(blk_off, vlen, fingerprint). The simplest hook is to add a
    // variant of execute_write_local that takes pre-allocated blk_off.
    uint64_t blk_off = e->staging_off;
    // iter-17A Stage 7: removed dead "defensive flush" call here. The
    // prior code was `pool_->read(blk_off, nullptr, 0)` which early-returns
    // at len==0 with NO clflushopt issued (see cxl_kv_blockpool.cc:180).
    // The companion full_fence had nothing to order. execute_write_local_with_blk
    // does not read pool bytes (it only embeds blk_off into the slot encoding);
    // future readers do their own pool_->read which performs the flush.
    // Call into execute_write_local with the special "pre-allocated"
    // signal — implemented as a new internal helper:
    e->status = execute_write_local_with_blk(e->key, blk_off, value_len,
                                              (int)e->op_kind, src);
    return;
  }
  // staging_gen == 1: fallback path, fall through to STAGING below.
#endif
  // STAGING (default and fallback): read worker's value bytes from
  // ForwardStaging[src][me][slot_idx], call execute_write_local which
  // allocates a fresh block and copies into it.
  uint32_t slot_idx = (uint32_t)e->staging_off;
  uint8_t *staging =
      forward_staging_bytes(fs_, src, host_id_, (int)slot_idx);
  for (uint32_t off = 0; off < value_len; off += 64) {
    flush_line((void *)(staging + off));
  }
  full_fence();
  e->status = execute_write_local(e->key, staging, value_len,
                                   (int)e->op_kind, src);
}

// iter-11A Phase 1 forwarder-pool-direct: owner's read_handler writes
// value bytes directly into rs_[src][me][slot_idx].value_bytes, sets
// staging.lookup_epoch (C13 tag) + status + value_size, then publishes
// staging.ready_op_id = req_op_id. Reader polls ready_op_id and
// memcpy's value_bytes from staging — no second pool->read needed.
//
// Legacy resp_op_id/resp_blk_off/resp_value_len on the ReadEntry are
// still set for protocol compatibility but no longer consulted by the
// new reader path.
void CxlKvStoreA::read_handler(ReadEntry *e, int src, uint32_t slot_idx) {
  PATH_CTR(n_read_handler_served);  // iter-15A HR-2 gate anchor
  uint64_t req_op_id = e->req_op_id.load(std::memory_order_acquire);
  uint32_t b = bucket_idx(e->key);
  CxlKvBucket *bucket = &buckets_[b];

  // iter-16A Receiver-NOOP study (per docs/microbench_xhost_spec.md §D).
  // L3: only ack — set staging to no-data, publish ready_op_id, return.
  //     Reader's spin loop unblocks immediately on ready_op_id.
  // L2: lookup + dummy publish — skip pool->read of value bytes.
  // L1: == L0 (read path has no invalidate broadcast; nothing to skip).
  int nlevel = recv_noop_level();
  if (nlevel >= 2) {
    ReadStagingSlot *st_noop =
        read_staging_slot(rs_, src, host_id_, (int)slot_idx);
    if (nlevel >= 3) {
      // L3: pure ack
      st_noop->key = e->key;
      st_noop->value_size = 0;
      st_noop->status = 0;
      st_noop->lookup_epoch = 0;
      st_noop->resp_blk_off = 0;
      flush_line(st_noop);
      store_fence();
      st_noop->ready_op_id.store(req_op_id, std::memory_order_release);
      flush_line(&st_noop->ready_op_id);
      store_fence();
      e->status = 0;
      e->resp_value_len = 0;
      e->resp_blk_off = 0;
      return;
    }
    // L2: bucket cacheline read (CXL line touch cost), no pool->read for value.
    flush_line(bucket);
    full_fence();
    (void)bucket->slots[0].value;  // force CXL line into cache
    st_noop->key = e->key;
    st_noop->value_size = 0;
    st_noop->status = 0;
    st_noop->lookup_epoch = 0;
    st_noop->resp_blk_off = 0;
    flush_line(st_noop);
    store_fence();
    st_noop->ready_op_id.store(req_op_id, std::memory_order_release);
    flush_line(&st_noop->ready_op_id);
    store_fence();
    e->status = 0;
    e->resp_value_len = 0;
    e->resp_blk_off = 0;
    return;
  }
  // L0/L1: full path below.
  flush_line(bucket);
  flush_line((char *)bucket + 64);
  full_fence();

  // C13: capture bucket epoch at lookup time. Reader validates
  // staging.lookup_epoch >= my_epoch_at_send.
  uint64_t lookup_epoch =
      cache_ ? cache_pool_bucket_epoch(cache_, e->key) : 0;
  ReadStagingSlot *st =
      read_staging_slot(rs_, src, host_id_, (int)slot_idx);

  auto publish_staging = [&](int32_t st_status, uint32_t vlen) {
    st->key = e->key;
    st->value_size = vlen;
    st->status = st_status;
    st->lookup_epoch = lookup_epoch;
    // Flush control cacheline (excluding ready_op_id, published last).
    flush_line(st);
#if FUSEE_READ_GUARD == FUSEE_READ_GUARD_STAGING
    // STAGING (default): value bytes already copied into st->value_bytes
    // by caller; flush them so reader can pull from staging directly.
    if (vlen > 0) {
      for (uint32_t off = 0; off < vlen; off += 64) {
        flush_line(st->value_bytes + off);
      }
    }
#else
    // RCU / HAZARD: NO value bytes copy on owner side. Reader pulls
    // value directly from pool_ via st->resp_blk_off (set by caller).
    // st->value_bytes is unused in this build.
#endif
    store_fence();
    // Release-publish: reader polling ready_op_id observes the
    // staging fields populated above only after this store.
    st->ready_op_id.store(req_op_id, std::memory_order_release);
    flush_line(&st->ready_op_id);
    store_fence();
  };

  int found = -1;
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
    if (bucket->slots[s].key == e->key) { found = s; break; }
  }
  if (found < 0) {
    publish_staging(-1, 0);
    e->status = -1;
    e->resp_value_len = 0;
    e->resp_blk_off = 0;
    return;
  }

  SlotDirectoryEntry *de =
      slot_directory_entry(dir_, b, (uint32_t)found);
  slot_directory_lock(de);
  if (src >= 0 && src < num_hosts_ && src != host_id_) {
    de->sharer_bitmap = (uint8_t)(de->sharer_bitmap | (1u << src));
  }
  uint64_t encoded = bucket->slots[found].value;
  slot_directory_unlock(de);

  uint8_t sc = cxl_slot_size_class(encoded);
  if (sc == kSizeClassInline || pool_ == nullptr) {
    // Inline u64 fallback — pack 8 bytes into both staging value_bytes
    // (STAGING reader) and resp_blk_off (RCU/HAZARD reader's inline path).
    std::memcpy(st->value_bytes, &encoded, 8);
    st->resp_blk_off = encoded;  // iter-13A: inline 8B in this field
    publish_staging(0, 8);
    e->resp_value_len = 8;
    std::memcpy(&e->resp_blk_off, &encoded, 8);
    e->status = 0;
    return;
  }

  uint64_t blk_off = cxl_slot_blk_off(encoded);
  if (blk_off == 0) {
    st->resp_blk_off = 0;
    publish_staging(-1, 0);
    e->status = -1;
    e->resp_value_len = 0;
    e->resp_blk_off = 0;
    return;
  }

  // Read value-length header out of the pool block.
  uint8_t hdr[4];
  pool_->read(blk_off, hdr, 4);
  uint32_t vlen = 0;
  std::memcpy(&vlen, hdr, 4);
  if (vlen == 0 || vlen > kReadStagingSlotBytes) {
    st->resp_blk_off = 0;
    publish_staging(-1, 0);
    e->status = -1;
    e->resp_value_len = 0;
    e->resp_blk_off = 0;
    return;
  }

#if FUSEE_READ_GUARD == FUSEE_READ_GUARD_STAGING
  // STAGING (default): owner copies value bytes pool→staging — this is
  // the redundant copy iter-13A is eliminating for RCU/HAZARD builds.
  pool_->read(blk_off + 4, st->value_bytes, vlen);
#else
  // RCU / HAZARD: skip the copy. Just hand reader the blk_off so it can
  // pool_->read() directly into its own DRAM buffer.
#endif
  st->resp_blk_off = blk_off;
  publish_staging(0, vlen);

  // Legacy resp fields (compat — new reader ignores).
  e->resp_value_len = vlen;
  e->resp_blk_off = blk_off;
  e->status = 0;
}

void CxlKvStoreA::write_receiver_loop(std::vector<int> ring_indices) {
  probe_ring();
  // iter-17A Stage 6 micro-opt: lfence-vs-mfence applied at sites A+C only.
  // Site B (per-slot req_op_id load) MUST remain mfence: see iter-17A
  // scaling design doc for full reasoning.
  // iter-17A Phase 3: receiver may own multiple ring shards (packing);
  // we round-robin across ring_indices to ensure fairness.
  while (!write_receiver_stop_.load(std::memory_order_acquire)) {
    bool did_work = false;
   for (int ring_idx : ring_indices) {
    for (int src = 0; src < num_hosts_; src++) {
      if (src == host_id_) continue;
      WriteRing *ring = &wr_->rings[src][host_id_][ring_idx];
      uint64_t head = ring->head;
      flush_line((void *)&ring->tail);
      __builtin_ia32_lfence();  // site A (single-field tail load, safe)
      uint64_t tail = ring->tail.load(std::memory_order_acquire);
      while (head < tail) {
        uint32_t slot = (uint32_t)(head % kWriteRingDepth);
        WriteEntry *e = &ring->entries[slot];
        // iter-16A Stage 6 (rcv_poll) start. Payload packs head+tail
        // (head in low 32b, tail in high 32b) since op_id is not yet known.
        PROBE_OP("XWR6S",
                 (head & 0xFFFFFFFFULL) | ((tail & 0xFFFFFFFFULL) << 32));
        flush_line((void *)&e->req_op_id);
        full_fence();
        uint64_t op_id = e->req_op_id.load(std::memory_order_acquire);
        if (op_id == 0) {
          // Gap encountered (worker fetch_add'd but hasn't published yet).
          PROBE_OP("XWR6Z", head);
          for (int gap_iter = 0; gap_iter < 4096 && op_id == 0; gap_iter++) {
            __builtin_ia32_pause();
            flush_line((void *)&e->req_op_id);
            __builtin_ia32_lfence();  // site C (loop self-corrects, safe)
            op_id = e->req_op_id.load(std::memory_order_acquire);
          }
          if (op_id == 0) {
            // Gap budget exhausted — bail out of inner loop, retry next outer iter.
            PROBE_OP("XWR6X", head);
            break;
          }
          // Gap healed during budget spin.
          PROBE_OP("XWR6H", op_id);
        }
        // iter-16A Stage 6 end ≡ Stage 7 (rcv_work) start.
        PROBE_OP("XWR6E", op_id);
        // Pull the rest of cacheline 1 (key, op_kind, value_len,
        // staging_off, staging_gen) — they're on the same line as
        // req_op_id, the flush above already fetched them.
        write_handler(e, src);
        // iter-16A Stage 7 end ≡ Stage 8 (ack_publish) start.
        PROBE_OP("XWR7E", op_id);

        std::atomic_thread_fence(std::memory_order_release);
        e->resp_op_id.store(op_id, std::memory_order_release);
        flush_line((void *)&e->resp_op_id);
        store_fence();
        // iter-16A Stage 8 end.
        PROBE_OP("XWR8E", op_id);
        head++;
        did_work = true;
      }
      ring->head = head;
    }
   }  // for ring_idx
    if (!did_work) __builtin_ia32_pause();
  }
  probe_flush();
}

void CxlKvStoreA::read_receiver_loop(std::vector<int> ring_indices) {
  probe_ring();
  while (!read_receiver_stop_.load(std::memory_order_acquire)) {
    bool did_work = false;
   for (int ring_idx : ring_indices) {
    for (int src = 0; src < num_hosts_; src++) {
      if (src == host_id_) continue;
      ReadRing *ring = &rr_->rings[src][host_id_][ring_idx];
      uint64_t head = ring->head;
      // iter-18A Stage RR1 (ring_drain) start.
      PROBE_READ_OP("XRR1S", head);
      flush_line((void *)&ring->tail);
      full_fence();
      uint64_t tail = ring->tail.load(std::memory_order_acquire);
      while (head < tail) {
        uint32_t slot = (uint32_t)(head % kReadRingDepth);
        ReadEntry *e = &ring->entries[slot];
        flush_line((void *)&e->req_op_id);
        full_fence();
        uint64_t op_id = e->req_op_id.load(std::memory_order_acquire);
        // iter-12A bimodal fix: gap-tolerance budget before bailing.
        // See note in inval_receiver_loop for rationale.
        if (op_id == 0) {
          PROBE_READ_OP("XRR1Z", head);
          for (int gap_iter = 0; gap_iter < 4096 && op_id == 0; gap_iter++) {
            // iter-18A C4: pause 4× before flushing — symmetric to worker
            // C3 (Stage 4): reduces CXL polling pressure on the req_op_id
            // line so peer's worker store can settle.
            __builtin_ia32_pause(); __builtin_ia32_pause();
            __builtin_ia32_pause(); __builtin_ia32_pause();
            flush_line((void *)&e->req_op_id);
            full_fence();
            op_id = e->req_op_id.load(std::memory_order_acquire);
          }
          if (op_id == 0) {
            PROBE_READ_OP("XRR1X", head);
            break;
          }
        }
        // iter-18A Stage RR1 end ≡ Stage RR2 (handler: bucket+pool+staging) start.
        PROBE_READ_OP("XRR1E", op_id);
        // iter-11A Phase 1: pass slot_idx so read_handler can deposit
        // value bytes directly into rs_[src][me][slot_idx].
        read_handler(e, src, slot);
        // iter-18A Stage RR2 end ≡ Stage RR3 (ack publish) start.
        PROBE_READ_OP("XRR2E", op_id);

        std::atomic_thread_fence(std::memory_order_release);
        e->resp_op_id.store(op_id, std::memory_order_release);
        flush_line((void *)&e->resp_op_id);
        store_fence();
        // iter-18A Stage RR3 end.
        PROBE_READ_OP("XRR3E", op_id);
        head++;
        did_work = true;
      }
      ring->head = head;
    }
   }  // for ring_idx
    if (!did_work) __builtin_ia32_pause();
  }
  probe_flush();
}

int CxlKvStoreA::search(uint64_t key, void *out_buf, uint32_t buf_len,
                        uint32_t *out_len) {
  if (key == kEmptyKey) return -1;
  PROBE_PATH("R1", key);

  // iter-10A Phase 1.C: TLS L1 lookup (per-worker private DRAM, 0
  // cross-core MESI traffic on hit). Only enabled if worker called
  // set_thread_tls_cache(). bucket_epoch is a single 8-B atomic load
  // — small cross-core cost vs the 16-cacheline value_bytes memcpy
  // that shared cache_pool_lookup does on hot Zipf keys.
#if !FUSEE_DISABLE_TLS
  if (g_thread_tls) {
    uint64_t cur_epoch = cache_pool_bucket_epoch(cache_, key);
    uint8_t tls_buf[kForwardStagingSlotBytes];
    uint32_t tls_sz = 0;
    if (tls_lookup(g_thread_tls, key, cur_epoch, tls_buf,
                   sizeof(tls_buf), &tls_sz)) {
      PROBE_PATH("R0_tls_hit", key);
      PATH_CTR(n_tls_hit);
      if (out_len) *out_len = tls_sz;
      uint32_t copy_len = tls_sz < buf_len ? tls_sz : buf_len;
      if (out_buf && copy_len > 0) std::memcpy(out_buf, tls_buf, copy_len);
      PROBE_PATH("R6", key);
      return 0;
    }
  }
#endif

  // Fast path L2: shared cache_pool lookup with stale check.
#if !FUSEE_DISABLE_CACHE_POOL
  uint8_t buf[kForwardStagingSlotBytes];
  uint32_t sz = 0;
  if (cache_pool_lookup(cache_, key, buf, sizeof(buf), &sz)) {
    PROBE_PATH("R2hit", key);
    PATH_CTR(n_r2hit);
    // populate TLS L1 with the freshly-fetched value + current epoch
#if !FUSEE_DISABLE_TLS
    if (g_thread_tls) {
      uint64_t cur_epoch = cache_pool_bucket_epoch(cache_, key);
      tls_insert(g_thread_tls, key, buf, sz, cur_epoch);
    }
#endif
    if (out_len) *out_len = sz;
    uint32_t copy_len = sz < buf_len ? sz : buf_len;
    if (out_buf && copy_len > 0) std::memcpy(out_buf, buf, copy_len);
    PROBE_PATH("R6", key);
    return 0;
  }
#endif
  PROBE_PATH("R2miss", key);

  uint32_t owner = owner_host(key);

  // Cross-host miss: §I9 register-then-fill via OP_CACHE_REGISTER.
  if (owner != (uint32_t)host_id_ && rr_) {
    PROBE_PATH("R3", key);
    PATH_CTR(n_r3);
    uint8_t v[kForwardStagingSlotBytes];
    uint32_t vlen = 0;
    int rc = forward_read(owner, key, v, sizeof(v), &vlen);
    if (rc != 0) return rc;
    PROBE_PATH("R4", key);
    // §AP15: populate cache ONLY after register ACK (we got it here).
    if (vlen > 0) {
#if !FUSEE_DISABLE_CACHE_POOL
      cache_pool_insert(cache_, key, v, vlen);
      PATH_CTR(n_cache_pool_insert_from_read);
#endif
      // Also populate TLS L1 with new epoch (cache_pool_insert just
      // bumped it).
#if !FUSEE_DISABLE_TLS
      if (g_thread_tls) {
        uint64_t cur_epoch = cache_pool_bucket_epoch(cache_, key);
        tls_insert(g_thread_tls, key, v, vlen, cur_epoch);
      }
#endif
    }
    if (out_len) *out_len = vlen;
    uint32_t copy_len = vlen < buf_len ? vlen : buf_len;
    if (out_buf && copy_len > 0) std::memcpy(out_buf, v, copy_len);
    PROBE_PATH("R6", key);
    return 0;
  }

  // Owner-self miss: direct CXL bucket scan + own pool fetch.
  PATH_CTR(n_r2miss_local);
  uint32_t b = bucket_idx(key);
  CxlKvBucket *bucket = &buckets_[b];
  flush_line(bucket);
  flush_line((char *)bucket + 64);
  full_fence();
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
    if (bucket->slots[s].key == key) {
      uint64_t encoded = bucket->slots[s].value;
      uint8_t sc = cxl_slot_size_class(encoded);
      uint8_t v[kForwardStagingSlotBytes];
      uint32_t vlen = 0;
      if (sc == kSizeClassInline || pool_ == nullptr) {
        std::memcpy(v, &encoded, 8);
        vlen = 8;
      } else {
        uint64_t blk_off = cxl_slot_blk_off(encoded);
        if (blk_off != 0) {
          uint8_t hdr_buf[4];
          pool_->read(blk_off, hdr_buf, 4);
          std::memcpy(&vlen, hdr_buf, 4);
          if (vlen > 0 && vlen <= kForwardStagingSlotBytes) {
            pool_->read(blk_off + 4, v, vlen);
          } else {
            return -1;
          }
        }
      }
      if (out_len) *out_len = vlen;
      uint32_t copy_len = vlen < buf_len ? vlen : buf_len;
      if (out_buf && copy_len > 0) std::memcpy(out_buf, v, copy_len);
      cache_pool_insert(cache_, key, v, vlen);
      // Populate TLS L1 with new epoch.
      if (g_thread_tls) {
        uint64_t cur_epoch = cache_pool_bucket_epoch(cache_, key);
        tls_insert(g_thread_tls, key, v, vlen, cur_epoch);
      }
      PROBE_PATH("R6", key);
      return 0;
    }
  }
  return -1;
}

}  // namespace fusee
