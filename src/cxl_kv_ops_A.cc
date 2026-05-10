#define _GNU_SOURCE
#include <sched.h>
#include <pthread.h>
#include <unistd.h>

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
}  // namespace

namespace {

// CoW slot publish: write value bytes to a freshly allocated CXL block
// (already done by caller before reaching here), then atomically update
// slot.{key,value}. We use the spec §VI-A.bis "plain + clflushopt +
// sfence" pattern for the 16 B slot itself (slot is < 256 B → NT store
// not warranted).
inline void publish_slot_cow(CxlKvSlot *slot, uint64_t key,
                             uint64_t encoded_value) {
  slot->value = encoded_value;
  flush_line(slot);
  store_fence();
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
template <typename Entry>
inline int generic_spin_wait(Entry *e, uint64_t op_id, int *out_status) {
  const uint64_t kBudgetUs = 5000;
  uint64_t spin_start_ns = 0;
  for (;;) {
    flush_line((void *)&e->resp_op_id);
    full_fence();
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

// Owner-self write. Called by local insert/update/remove and by
// responder on behalf of a remote forwarder. Steps follow spec §V.
//
// iter-9A Phase 1: variable-length value bytes. value/value_len
// is the user payload. value=nullptr+value_len=0 is the DELETE
// convention. value_len > pool_->block_size() is rejected.
int CxlKvStoreA::execute_write_local(uint64_t key, const void *value,
                                     uint32_t value_len, int op_kind) {
  if (key == kEmptyKey) return -1;
  if (op_kind != kOpKindDelete) {
    if (!value || value_len == 0) return -1;
    if (pool_ && value_len + 4 > pool_->block_size()) return -5;
  }
  PROBE_OP("W1", key);
  uint32_t b = bucket_idx(key);
  CxlKvBucket *bucket = &buckets_[b];

  flush_line(bucket);
  flush_line((char *)bucket + 64);
  full_fence();

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
  PROBE_OP("W2", key);

  flush_line(bucket);
  full_fence();
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
  PROBE_OP("W3", key);
  if (op_kind != kOpKindInsert && num_hosts_ > 1 && ir_) {
    int n_sent = 0;
    for (int h = 0; h < num_hosts_; h++) {
      if (h == host_id_) continue;
      if ((bitmap & (1u << h)) == 0) continue;
      if (n_sent == 0) PROBE_OP("W4", key);
      send_invalidate((uint32_t)h, key);
      n_sent++;
    }
    if (n_sent > 0) PROBE_OP("W6", key);
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
    PROBE_OP("W9", key);
  } else if (pool_ == nullptr) {
    // Inline u64 fallback (legacy / Protocol A degenerate mode).
    // Only valid when caller passes 8 bytes; truncated otherwise.
    uint64_t inline_v = 0;
    std::memcpy(&inline_v, value, value_len < 8 ? value_len : 8);
    publish_slot_cow(slot, key, inline_v);
    PROBE_OP("W9", key);
  } else {
    // Full blockpool CoW path with real value_len bytes.
    uint64_t blk_off = pool_->alloc();
    PROBE_OP("W7", key);
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
    // reads the 4B header to know how much to copy back.
    uint8_t buf[4096];  // local stack scratch (max block 4 KB)
    uint32_t total = 4 + value_len;
    if (total > sizeof(buf)) {
      slot_directory_unlock(de);
      return -5;
    }
    std::memcpy(buf, &value_len, 4);
    std::memcpy(buf + 4, value, value_len);
    pool_->write(blk_off, buf, total);
    PROBE_OP("W8", key);
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
    PROBE_OP("W9", key);
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
  PROBE_OP("W10", key);
  slot_directory_unlock(de);

  // Step 7: own cache.
  if (op_kind == kOpKindDelete) {
    cache_pool_evict(cache_, key);
    // iter-10A Phase 1.C: also evict from TLS so subsequent reads
    // don't see the deleted entry. (cache_pool_evict already bumped
    // bucket_epoch so any TLS reader without our explicit evict would
    // also detect stale on next access — this is belt + suspenders.)
    if (g_thread_tls) tls_evict(g_thread_tls, key);
  } else {
    cache_pool_insert(cache_, key,
                      reinterpret_cast<const uint8_t *>(value),
                      value_len);
    // iter-10A Phase 1.C: populate TLS L1 with the just-written value
    // and the post-bump epoch so this thread's next read hits TLS.
    if (g_thread_tls) {
      uint64_t cur_epoch = cache_pool_bucket_epoch(cache_, key);
      tls_insert(g_thread_tls, key,
                 reinterpret_cast<const uint8_t *>(value),
                 value_len, cur_epoch);
    }
  }
  PROBE_OP("W12", key);
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

int CxlKvStoreA::enable_write_ring(WriteRingMatrix *wr,
                                   ForwardStagingMatrix *fs,
                                   bool init_region, bool spawn_receiver) {
  if (!wr || !fs) return -1;
  wr_ = wr;
  fs_ = fs;
  if (init_region) {
    std::memset(wr, 0, write_ring_matrix_bytes());
    flush_region(wr, write_ring_matrix_bytes());
    std::memset(fs, 0, forward_staging_matrix_bytes());
    flush_region(fs, forward_staging_matrix_bytes());
    store_fence();
  }
  if (spawn_receiver) {
    write_receiver_stop_.store(false, std::memory_order_relaxed);
    write_receiver_ = std::thread([this]() {
      // iter-9A Phase 2.E + C3: named + CPU-pinned per task plan §2.F.
      pthread_setname_np(pthread_self(), "WriteReceiver");
      cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(65, &cs);
      pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
      fprintf(stderr,
        "[A:thread] WriteReceiver pid=%d tid=%lu pinned cpu=65 (host_id=%d)\n",
        getpid(), (unsigned long)pthread_self(), host_id_);
      this->write_receiver_loop();
    });
  }
  return 0;
}

void CxlKvStoreA::stop_write_receiver() {
  if (!write_receiver_.joinable()) return;
  write_receiver_stop_.store(true, std::memory_order_release);
  write_receiver_.join();
}

int CxlKvStoreA::enable_read_ring(ReadRingMatrix *rr, bool init_region,
                                  bool spawn_receiver) {
  if (!rr) return -1;
  rr_ = rr;
  if (init_region) {
    std::memset(rr, 0, read_ring_matrix_bytes());
    flush_region(rr, read_ring_matrix_bytes());
    store_fence();
  }
  if (spawn_receiver) {
    read_receiver_stop_.store(false, std::memory_order_relaxed);
    read_receiver_ = std::thread([this]() {
      pthread_setname_np(pthread_self(), "ReadReceiver");
      cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(67, &cs);
      pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
      fprintf(stderr,
        "[A:thread] ReadReceiver pid=%d tid=%lu pinned cpu=67 (host_id=%d)\n",
        getpid(), (unsigned long)pthread_self(), host_id_);
      this->read_receiver_loop();
    });
  }
  return 0;
}

void CxlKvStoreA::stop_read_receiver() {
  if (!read_receiver_.joinable()) return;
  read_receiver_stop_.store(true, std::memory_order_release);
  read_receiver_.join();
}

// iter-9A Phase 2.C: per-worker aggregator routing id setter.
// iter-10A Phase 1.C: per-worker TLS cache attach setter.
// Backing thread_locals declared at file top (so execute_write_local
// can read them).
void CxlKvStoreA::set_worker_id(int wid) { g_aggr_worker_id = wid; }
void CxlKvStoreA::set_thread_tls_cache(TlsCache *tls) { g_thread_tls = tls; }

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
void CxlKvStoreA::write_sender_loop() {
  while (!write_sender_stop_.load(std::memory_order_acquire)) {
    bool did_work = false;
    for (int w = 0; w < num_aggr_workers_; w++) {
      AggrSlot *s = aggregator_slot(aggr_, kAggrRingKindWrite, w);
      if (s->state.load(std::memory_order_acquire) != kAggrPending) continue;
      uint8_t *value = aggregator_value_buf(aggr_, kAggrRingKindWrite, w);
      int rc = forward_write_direct(s->dst_host, s->key,
                                    value, s->value_len,
                                    (int)s->op_kind);
      s->result_status = rc;
      s->state.store(kAggrDone, std::memory_order_release);
      did_work = true;
    }
    if (!did_work) __builtin_ia32_pause();
  }
}

void CxlKvStoreA::read_sender_loop() {
  while (!read_sender_stop_.load(std::memory_order_acquire)) {
    bool did_work = false;
    for (int w = 0; w < num_aggr_workers_; w++) {
      AggrSlot *s = aggregator_slot(aggr_, kAggrRingKindRead, w);
      if (s->state.load(std::memory_order_acquire) != kAggrPending) continue;
      uint8_t *vbuf = aggregator_value_buf(aggr_, kAggrRingKindRead, w);
      uint32_t got_len = 0;
      int rc = forward_read_direct(s->dst_host, s->key, vbuf,
                                   kAggrSlotValueBytes, &got_len);
      s->result_status = rc;
      s->value_len = got_len;
      s->state.store(kAggrDone, std::memory_order_release);
      did_work = true;
    }
    if (!did_work) __builtin_ia32_pause();
  }
}

void CxlKvStoreA::inval_sender_loop() {
  while (!inval_sender_stop_.load(std::memory_order_acquire)) {
    bool did_work = false;
    for (int w = 0; w < num_aggr_workers_; w++) {
      AggrSlot *s = aggregator_slot(aggr_, kAggrRingKindInval, w);
      if (s->state.load(std::memory_order_acquire) != kAggrPending) continue;
      int rc = send_invalidate_direct(s->dst_host, s->key);
      s->result_status = rc;
      s->state.store(kAggrDone, std::memory_order_release);
      did_work = true;
    }
    if (!did_work) __builtin_ia32_pause();
  }
}

int CxlKvStoreA::enable_senders(AggregatorRegion *ar, int num_workers,
                                bool spawn_senders) {
  if (!ar) return -1;
  if (num_workers <= 0 || num_workers > kMaxAggrWorkers) return -2;
  aggr_ = ar;
  num_aggr_workers_ = num_workers;
  if (spawn_senders) {
    write_sender_stop_.store(false, std::memory_order_relaxed);
    read_sender_stop_.store(false, std::memory_order_relaxed);
    inval_sender_stop_.store(false, std::memory_order_relaxed);
    write_sender_ = std::thread([this]() {
      pthread_setname_np(pthread_self(), "WriteSender");
      cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(64, &cs);
      pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
      fprintf(stderr,
        "[A:thread] WriteSender pid=%d tid=%lu pinned cpu=64 (host_id=%d)\n",
        getpid(), (unsigned long)pthread_self(), host_id_);
      this->write_sender_loop();
    });
    read_sender_ = std::thread([this]() {
      pthread_setname_np(pthread_self(), "ReadSender");
      cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(66, &cs);
      pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
      fprintf(stderr,
        "[A:thread] ReadSender pid=%d tid=%lu pinned cpu=66 (host_id=%d)\n",
        getpid(), (unsigned long)pthread_self(), host_id_);
      this->read_sender_loop();
    });
    inval_sender_ = std::thread([this]() {
      pthread_setname_np(pthread_self(), "InvalSender");
      cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(68, &cs);
      pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
      fprintf(stderr,
        "[A:thread] InvalSender pid=%d tid=%lu pinned cpu=68 (host_id=%d)\n",
        getpid(), (unsigned long)pthread_self(), host_id_);
      this->inval_sender_loop();
    });
  }
  return 0;
}

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
            (fs_ != nullptr) && (ir_ != nullptr);
  if (!ok) {
    fprintf(stderr,
            "FATAL [iter-9A C4]: cross-host op attempted with "
            "incomplete N:1:1:N wiring on host %d (num_hosts=%d): "
            "wr_=%p rr_=%p fs_=%p ir_=%p — must call "
            "enable_write_ring + enable_read_ring + enable_invalidate "
            "before first cross-host op\n",
            host_id_, num_hosts_,
            (void *)wr_, (void *)rr_, (void *)fs_, (void *)ir_);
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
  WriteRing *ring = &wr_->rings[host_id_][owner];
  uint64_t my_op = write_op_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
  uint64_t op_id = encode_op_id(host_id_, my_op);

  // Reserve slot.
  uint64_t tpos = ring->tail.fetch_add(1, std::memory_order_acq_rel);
  flush_line((void *)&ring->tail);
  store_fence();
  uint32_t slot_idx = (uint32_t)(tpos % kWriteRingDepth);
  WriteEntry *e = &ring->entries[slot_idx];

  // Wait for slot free (cacheline 1 holds req_op_id).
  for (;;) {
    flush_line((void *)&e->req_op_id);
    full_fence();
    if (e->req_op_id.load(std::memory_order_acquire) == 0) break;
    __builtin_ia32_pause();
  }

  // Copy value bytes into the staging arena slot first (op_kind=DELETE
  // skips this — value_len == 0).
  if (value && value_len > 0) {
    uint8_t *staging =
        forward_staging_bytes(fs_, host_id_, (int)owner, (int)slot_idx);
    std::memcpy(staging, value, value_len);
    for (uint32_t off = 0; off < value_len; off += 64) {
      flush_line((void *)(staging + off));
    }
    store_fence();
  }

  // Fill control message (cacheline 1).
  e->key = key;
  e->op_kind = (uint8_t)op_kind;
  e->value_len = value_len;
  e->staging_off = slot_idx;          // sanity check; receiver asserts
  e->staging_gen = 0;                 // reserved (iter-10A pool-gen)
  e->resp_op_id.store(0, std::memory_order_relaxed);
  e->status = 0;
  std::atomic_thread_fence(std::memory_order_release);
  e->req_op_id.store(op_id, std::memory_order_release);
  flush_line((void *)&e->req_op_id);  // publish cacheline 1
  store_fence();

  int status = 0;
  int rc = generic_spin_wait(e, op_id, &status);
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
  InvalRing *ring = &ir_->rings[host_id_][target_host];
  uint64_t my_op = inval_op_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
  uint64_t op_id = ((uint64_t)(host_id_ + 1) << 56) |
                   (my_op & 0x00FFFFFFFFFFFFFFULL);

  uint64_t tpos = ring->tail.fetch_add(1, std::memory_order_acq_rel);
  flush_line((void *)&ring->tail);
  store_fence();
  PROBE_OP("I1", op_id);
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
  PROBE_OP("I2", op_id);

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
      PROBE_OP("I7", op_id);
      int rc = e->status;
      e->req_op_id.store(0, std::memory_order_release);
      flush_line((void *)e);  // line 1 (req_op_id)
      store_fence();
      PROBE_OP("I8", op_id);
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

int CxlKvStoreA::enable_invalidate(InvalRingMatrix *ir, bool init_region,
                                   bool spawn_dispatcher) {
  if (!ir) return -1;
  ir_ = ir;
  if (init_region) {
    std::memset(ir, 0, inval_ring_matrix_bytes());
    flush_region(ir, inval_ring_matrix_bytes());
    store_fence();
  }
  if (spawn_dispatcher) {
    inval_receiver_stop_.store(false, std::memory_order_relaxed);
    inval_receiver_ = std::thread([this]() {
      // iter-9A Phase 2.E + C3: named + CPU-pinned per task plan §2.F.
      pthread_setname_np(pthread_self(), "InvalReceiver");
      cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(69, &cs);
      pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
      fprintf(stderr,
        "[A:thread] InvalReceiver pid=%d tid=%lu pinned cpu=69 (host_id=%d)\n",
        getpid(), (unsigned long)pthread_self(), host_id_);
      this->inval_receiver_loop();
    });
  }
  return 0;
}

void CxlKvStoreA::stop_inval_receiver() {
  if (!inval_receiver_.joinable()) return;
  inval_receiver_stop_.store(true, std::memory_order_release);
  inval_receiver_.join();
}

// InvalReceiver: drain incoming InvalRing[*][me]; for each entry mark
// the key stale in the local cache_pool and ACK via resp_op_id.
// CRUCIALLY this thread does NOT acquire the directory spinlock and
// does NOT call execute_write_local — so it cannot deadlock with
// the WriteReceiver/ReadReceiver that are processing forwards.
void CxlKvStoreA::inval_receiver_loop() {
  // Touch probe ring so its FUSEE_PROBE_DUMP envvar is read on this thread.
  probe_ring();
  while (!inval_receiver_stop_.load(std::memory_order_acquire)) {
    bool did_work = false;
    for (int src = 0; src < num_hosts_; src++) {
      if (src == host_id_) continue;
      InvalRing *ring = &ir_->rings[src][host_id_];
      uint64_t head = ring->head;
      flush_line((void *)&ring->tail);
      full_fence();
      uint64_t tail = ring->tail.load(std::memory_order_acquire);
      while (head < tail) {
        PROBE_OP("I3", tail);
        uint32_t slot = (uint32_t)(head % kInvalRingDepth);
        InvalEntry *e = &ring->entries[slot];
        flush_line((void *)e);
        full_fence();
        uint64_t op_id = e->req_op_id.load(std::memory_order_acquire);
        if (op_id == 0) break;
        PROBE_OP("I4", op_id);

        // Lazy stale flag — no directory lock, no execute_write_local.
        cache_pool_set_stale(cache_, e->key);
        PROBE_OP("I5", op_id);
        e->status = 0;
        std::atomic_thread_fence(std::memory_order_release);
        e->resp_op_id.store(op_id, std::memory_order_release);
        // iter-6A: resp_op_id is on the SECOND cacheline; flush THAT
        // line so the producer's CXL-coherent load on resp_op_id sees
        // the new value.
        flush_line((void *)&e->resp_op_id);
        store_fence();
        PROBE_OP("I6", op_id);
        head++;
        did_work = true;
      }
      ring->head = head;
    }
    if (!did_work) __builtin_ia32_pause();
  }
  probe_flush();
}

// iter-9A Phase 2.A direct path — read-path register-then-fill via
// ReadRing. Producer enqueues control-only req; ReadReceiver responds
// with (status, owner_blk_off, value_len). Reader pulls value bytes
// directly via pool_->read — no value bytes on the message ring.
int CxlKvStoreA::forward_read_direct(uint32_t owner, uint64_t key,
                                     void *out_buf, uint32_t buf_len,
                                     uint32_t *out_len) {
  if (!rr_) return -10;
  ReadRing *ring = &rr_->rings[host_id_][owner];
  uint64_t my_op = read_op_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
  uint64_t op_id = encode_op_id(host_id_, my_op);

  uint64_t tpos = ring->tail.fetch_add(1, std::memory_order_acq_rel);
  flush_line((void *)&ring->tail);
  store_fence();
  uint32_t slot_idx = (uint32_t)(tpos % kReadRingDepth);
  ReadEntry *e = &ring->entries[slot_idx];

  for (;;) {
    flush_line((void *)&e->req_op_id);
    full_fence();
    if (e->req_op_id.load(std::memory_order_acquire) == 0) break;
    __builtin_ia32_pause();
  }

  e->key = key;
  e->resp_op_id.store(0, std::memory_order_relaxed);
  e->status = 0;
  e->resp_value_len = 0;
  e->resp_blk_off = 0;
  std::atomic_thread_fence(std::memory_order_release);
  e->req_op_id.store(op_id, std::memory_order_release);
  flush_line((void *)&e->req_op_id);
  store_fence();

  int status = 0;
  int rc = generic_spin_wait(e, op_id, &status);
  if (rc != 0) return rc;
  if (status != 0) {
    if (out_len) *out_len = 0;
    return status;
  }
  // Owner ACKed; read response side (cacheline 2) for blk_off + len.
  flush_line((void *)&e->resp_op_id);
  full_fence();
  uint32_t vlen = e->resp_value_len;
  uint64_t blk_off = e->resp_blk_off;

  // Two response shapes:
  //   blk_off == 0  →  inline u64 (legacy fallback, owner had no pool
  //                    or slot stored inline) — vlen carries the bytes
  //                    in the low half of resp_blk_off (overloaded).
  //   blk_off != 0  →  pool-backed: pool_->read(blk_off + 4, ...) of
  //                    vlen bytes.
  if (blk_off == 0 && vlen <= 8) {
    if (out_buf && vlen > 0) {
      std::memcpy(out_buf, &e->resp_blk_off, vlen);
    }
    if (out_len) *out_len = vlen;
    return 0;
  }

  if (vlen == 0 || vlen > kForwardStagingSlotBytes || pool_ == nullptr) {
    if (out_len) *out_len = 0;
    return -1;
  }
  uint8_t scratch[kForwardStagingSlotBytes];
  // Skip the 4 B value-length header (see execute_write_local block layout).
  pool_->read(blk_off + 4, scratch, vlen);
  uint32_t copy_len = vlen < buf_len ? vlen : buf_len;
  if (out_buf && copy_len > 0) std::memcpy(out_buf, scratch, copy_len);
  if (out_len) *out_len = vlen;
  return 0;
}

void CxlKvStoreA::write_handler(WriteEntry *e, int src) {
  // C2 staging contract: read forwarder's value bytes from
  // ForwardStaging[src][me][slot_idx]. slot_idx is encoded in
  // staging_off (sanity check: must equal e - ring->entries[0]).
  uint32_t value_len = e->value_len;
  uint32_t slot_idx = (uint32_t)e->staging_off;
  if (e->op_kind == kOpKindDelete) {
    e->status = execute_write_local(e->key, nullptr, 0, kOpKindDelete);
    return;
  }
  if (value_len == 0 || value_len > kForwardStagingSlotBytes) {
    e->status = -5;
    return;
  }
  uint8_t *staging =
      forward_staging_bytes(fs_, src, host_id_, (int)slot_idx);
  for (uint32_t off = 0; off < value_len; off += 64) {
    flush_line((void *)(staging + off));
  }
  full_fence();
  e->status = execute_write_local(e->key, staging, value_len,
                                   (int)e->op_kind);
}

void CxlKvStoreA::read_handler(ReadEntry *e, int src) {
  // §I9 register-then-fill: lookup slot under directory lock, set
  // sharer bit, respond with owner_blk_off + value_len. Value bytes
  // stay in owner's pool — reader pulls directly via pool->read.
  uint32_t b = bucket_idx(e->key);
  CxlKvBucket *bucket = &buckets_[b];
  flush_line(bucket);
  flush_line((char *)bucket + 64);
  full_fence();

  int found = -1;
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
    if (bucket->slots[s].key == e->key) { found = s; break; }
  }
  if (found < 0) {
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
    // Inline u64 fallback — pack the 8 bytes into resp_blk_off
    // (whose `blk_off == 0` sentinel tells the reader to interpret
    // the field as inline value bytes; see forward_read).
    e->resp_value_len = 8;
    std::memcpy(&e->resp_blk_off, &encoded, 8);
    e->status = 0;
    return;
  }
  uint64_t blk_off = cxl_slot_blk_off(encoded);
  if (blk_off == 0) {
    e->status = -1;
    e->resp_value_len = 0;
    e->resp_blk_off = 0;
    return;
  }
  // Read the 4 B value-length header out of the pool block. We
  // intentionally don't fetch value bytes here — reader does that.
  uint8_t hdr[4];
  pool_->read(blk_off, hdr, 4);
  uint32_t vlen = 0;
  std::memcpy(&vlen, hdr, 4);
  if (vlen == 0 || vlen > kForwardStagingSlotBytes) {
    e->status = -1;
    e->resp_value_len = 0;
    e->resp_blk_off = 0;
    return;
  }
  e->resp_value_len = vlen;
  e->resp_blk_off = blk_off;
  e->status = 0;
}

void CxlKvStoreA::write_receiver_loop() {
  probe_ring();
  while (!write_receiver_stop_.load(std::memory_order_acquire)) {
    bool did_work = false;
    for (int src = 0; src < num_hosts_; src++) {
      if (src == host_id_) continue;
      WriteRing *ring = &wr_->rings[src][host_id_];
      uint64_t head = ring->head;
      flush_line((void *)&ring->tail);
      full_fence();
      uint64_t tail = ring->tail.load(std::memory_order_acquire);
      while (head < tail) {
        uint32_t slot = (uint32_t)(head % kWriteRingDepth);
        WriteEntry *e = &ring->entries[slot];
        flush_line((void *)&e->req_op_id);
        full_fence();
        uint64_t op_id = e->req_op_id.load(std::memory_order_acquire);
        if (op_id == 0) break;
        // Pull the rest of cacheline 1 (key, op_kind, value_len,
        // staging_off, staging_gen) — they're on the same line as
        // req_op_id, the flush above already fetched them.
        write_handler(e, src);

        std::atomic_thread_fence(std::memory_order_release);
        e->resp_op_id.store(op_id, std::memory_order_release);
        flush_line((void *)&e->resp_op_id);
        store_fence();
        head++;
        did_work = true;
      }
      ring->head = head;
    }
    if (!did_work) __builtin_ia32_pause();
  }
  probe_flush();
}

void CxlKvStoreA::read_receiver_loop() {
  probe_ring();
  while (!read_receiver_stop_.load(std::memory_order_acquire)) {
    bool did_work = false;
    for (int src = 0; src < num_hosts_; src++) {
      if (src == host_id_) continue;
      ReadRing *ring = &rr_->rings[src][host_id_];
      uint64_t head = ring->head;
      flush_line((void *)&ring->tail);
      full_fence();
      uint64_t tail = ring->tail.load(std::memory_order_acquire);
      while (head < tail) {
        uint32_t slot = (uint32_t)(head % kReadRingDepth);
        ReadEntry *e = &ring->entries[slot];
        flush_line((void *)&e->req_op_id);
        full_fence();
        uint64_t op_id = e->req_op_id.load(std::memory_order_acquire);
        if (op_id == 0) break;
        read_handler(e, src);

        std::atomic_thread_fence(std::memory_order_release);
        e->resp_op_id.store(op_id, std::memory_order_release);
        flush_line((void *)&e->resp_op_id);
        store_fence();
        head++;
        did_work = true;
      }
      ring->head = head;
    }
    if (!did_work) __builtin_ia32_pause();
  }
  probe_flush();
}

int CxlKvStoreA::search(uint64_t key, void *out_buf, uint32_t buf_len,
                        uint32_t *out_len) {
  if (key == kEmptyKey) return -1;
  PROBE_OP("R1", key);

  // iter-10A Phase 1.C: TLS L1 lookup (per-worker private DRAM, 0
  // cross-core MESI traffic on hit). Only enabled if worker called
  // set_thread_tls_cache(). bucket_epoch is a single 8-B atomic load
  // — small cross-core cost vs the 16-cacheline value_bytes memcpy
  // that shared cache_pool_lookup does on hot Zipf keys.
  if (g_thread_tls) {
    uint64_t cur_epoch = cache_pool_bucket_epoch(cache_, key);
    uint8_t tls_buf[kForwardStagingSlotBytes];
    uint32_t tls_sz = 0;
    if (tls_lookup(g_thread_tls, key, cur_epoch, tls_buf,
                   sizeof(tls_buf), &tls_sz)) {
      PROBE_OP("R0_tls_hit", key);
      if (out_len) *out_len = tls_sz;
      uint32_t copy_len = tls_sz < buf_len ? tls_sz : buf_len;
      if (out_buf && copy_len > 0) std::memcpy(out_buf, tls_buf, copy_len);
      PROBE_OP("R6", key);
      return 0;
    }
  }

  // Fast path L2: shared cache_pool lookup with stale check.
  uint8_t buf[kForwardStagingSlotBytes];
  uint32_t sz = 0;
  if (cache_pool_lookup(cache_, key, buf, sizeof(buf), &sz)) {
    PROBE_OP("R2hit", key);
    // populate TLS L1 with the freshly-fetched value + current epoch
    if (g_thread_tls) {
      uint64_t cur_epoch = cache_pool_bucket_epoch(cache_, key);
      tls_insert(g_thread_tls, key, buf, sz, cur_epoch);
    }
    if (out_len) *out_len = sz;
    uint32_t copy_len = sz < buf_len ? sz : buf_len;
    if (out_buf && copy_len > 0) std::memcpy(out_buf, buf, copy_len);
    PROBE_OP("R6", key);
    return 0;
  }
  PROBE_OP("R2miss", key);

  uint32_t owner = owner_host(key);

  // Cross-host miss: §I9 register-then-fill via OP_CACHE_REGISTER.
  if (owner != (uint32_t)host_id_ && rr_) {
    PROBE_OP("R3", key);
    uint8_t v[kForwardStagingSlotBytes];
    uint32_t vlen = 0;
    int rc = forward_read(owner, key, v, sizeof(v), &vlen);
    if (rc != 0) return rc;
    PROBE_OP("R4", key);
    // §AP15: populate cache ONLY after register ACK (we got it here).
    if (vlen > 0) {
      cache_pool_insert(cache_, key, v, vlen);
      // Also populate TLS L1 with new epoch (cache_pool_insert just
      // bumped it).
      if (g_thread_tls) {
        uint64_t cur_epoch = cache_pool_bucket_epoch(cache_, key);
        tls_insert(g_thread_tls, key, v, vlen, cur_epoch);
      }
    }
    if (out_len) *out_len = vlen;
    uint32_t copy_len = vlen < buf_len ? vlen : buf_len;
    if (out_buf && copy_len > 0) std::memcpy(out_buf, v, copy_len);
    PROBE_OP("R6", key);
    return 0;
  }

  // Owner-self miss: direct CXL bucket scan + own pool fetch.
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
      PROBE_OP("R6", key);
      return 0;
    }
  }
  return -1;
}

}  // namespace fusee
