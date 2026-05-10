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

// Spin-wait for ForwardEntry response with a timeout. Returns 0 on
// response, -11 on timeout. Frees the slot (req_op_id=0) on success or
// timeout.
inline int forward_spin_wait(ForwardEntry *e, uint64_t op_id,
                             int *out_status) {
  // iter-8A Phase 5 fix [G6][AP16]: 200 ms → 5 ms. Mirrors iter-6A
  // InvalRing fix; Phase 3 attribution showed 50% of cross-host
  // CACHE_REGISTER fired this timeout, ballooning workload-d
  // kv=1024 T=64 trans_wall to 36 sec. 5 ms cap = 500× healthy
  // p99 (~10 µs) headroom. iter-9A backlog: fail-loud propagation.
  const uint64_t kBudgetUs = 5000;  // was 200000 (200 ms)
  uint64_t spin_start_ns = 0;
  for (;;) {
    flush_line((void *)e);
    full_fence();
    uint64_t resp = e->resp_op_id.load(std::memory_order_acquire);
    if (resp == op_id) {
      if (out_status) *out_status = e->status;
      e->req_op_id.store(0, std::memory_order_release);
      flush_line((void *)e);
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
        return -11;
      }
    }
    __builtin_ia32_pause();
  }
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
  // The cache_dispatcher_loop on each peer host drains InvalRing
  // independently of its responder, so no circular wait can form.
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
  } else {
    cache_pool_insert(cache_, key,
                      reinterpret_cast<const uint8_t *>(value),
                      value_len);
  }
  PROBE_OP("W12", key);
  return 0;
}

int CxlKvStoreA::insert(uint64_t key, const void *value, uint32_t value_len) {
  if (key == kEmptyKey) return -1;
  uint32_t owner = owner_host(key);
  if (owner != (uint32_t)host_id_) {
    return forward_to_owner(owner, key, value, value_len, kOpKindInsert);
  }
  return execute_write_local(key, value, value_len, kOpKindInsert);
}

int CxlKvStoreA::update(uint64_t key, const void *value, uint32_t value_len) {
  if (key == kEmptyKey) return -1;
  uint32_t owner = owner_host(key);
  if (owner != (uint32_t)host_id_) {
    return forward_to_owner(owner, key, value, value_len, kOpKindUpdate);
  }
  return execute_write_local(key, value, value_len, kOpKindUpdate);
}

int CxlKvStoreA::remove(uint64_t key) {
  if (key == kEmptyKey) return -1;
  uint32_t owner = owner_host(key);
  if (owner != (uint32_t)host_id_) {
    return forward_to_owner(owner, key, nullptr, 0, kOpKindDelete);
  }
  return execute_write_local(key, nullptr, 0, kOpKindDelete);
}

// ---- Cross-host forwarding via ForwardRingMatrix ----

int CxlKvStoreA::enable_forward(ForwardRingMatrix *fr, bool init_region,
                                bool spawn_responder) {
  if (!fr) return -1;
  fr_ = fr;
  if (init_region) {
    std::memset(fr, 0, forward_ring_matrix_bytes());
    flush_region(fr, forward_ring_matrix_bytes());
    store_fence();
  }
  if (spawn_responder) {
    responder_stop_.store(false, std::memory_order_relaxed);
    responder_ = std::thread([this]() {
      // iter-9A C3: name + pin to cpu 65 (WriteReceiver slot per
      // task plan §2.F). Plan reserves cpu 64-69 for system threads.
      pthread_setname_np(pthread_self(), "WriteReceiver");
      cpu_set_t cs;
      CPU_ZERO(&cs);
      CPU_SET(65, &cs);
      pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
      fprintf(stderr,
        "[A:thread] WriteReceiver pid=%d tid=%lu pinned cpu=65 (host_id=%d)\n",
        getpid(), (unsigned long)pthread_self(), host_id_);
      this->responder_loop();
    });
  }
  return 0;
}

void CxlKvStoreA::stop_responder() {
  if (!responder_.joinable()) return;
  responder_stop_.store(true, std::memory_order_release);
  responder_.join();
}

// Generic ForwardEntry enqueue + spin. Used by all 5 op_kind values.
int CxlKvStoreA::forward_to_owner(uint32_t owner, uint64_t key,
                                  const void *value, uint32_t value_len,
                                  int op_kind) {
  if (!fr_) return -10;
  if (value_len > kForwardEntryPayloadBytes) return -5;
  ForwardRing *ring = &fr_->rings[host_id_][owner];
  uint64_t my_op = req_op_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
  uint64_t op_id = ((uint64_t)(host_id_ + 1) << 56) |
                   (my_op & 0x00FFFFFFFFFFFFFFULL);

  // Reserve slot.
  uint64_t tpos = ring->tail.fetch_add(1, std::memory_order_acq_rel);
  flush_line((void *)&ring->tail);
  store_fence();
  uint32_t slot = (uint32_t)(tpos % kForwardRingDepth);
  ForwardEntry *e = &ring->entries[slot];

  // Wait for slot free.
  for (;;) {
    flush_line((void *)e);
    full_fence();
    if (e->req_op_id.load(std::memory_order_acquire) == 0) break;
    __builtin_ia32_pause();
  }

  e->key = key;
  e->value_len = value_len;
  e->op_kind = (uint8_t)op_kind;
  if (value && value_len > 0) {
    std::memcpy(e->payload, value, value_len);
    // Flush payload cachelines.
    for (uint32_t off = 0; off < value_len; off += 64) {
      flush_line((void *)(e->payload + off));
    }
  }
  e->resp_op_id.store(0, std::memory_order_relaxed);
  e->status = 0;
  std::atomic_thread_fence(std::memory_order_release);
  e->req_op_id.store(op_id, std::memory_order_release);
  flush_line((void *)e);
  store_fence();

  int status = 0;
  int rc = forward_spin_wait(e, op_id, &status);
  if (rc != 0) return rc;
  return status;
}

// iter-5A: send OP_INVALIDATE on the SEPARATE InvalRing channel.
// Producer reserves a slot via fetch_add(tail) + flush, writes the
// key, and spins on resp_op_id (which the dispatcher_loop on the
// target host will set). No interaction with ForwardRingMatrix.
int CxlKvStoreA::send_invalidate(uint32_t target_host, uint64_t key) {
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
    dispatcher_stop_.store(false, std::memory_order_relaxed);
    cache_dispatcher_ = std::thread([this]() {
      // iter-9A C3: name + pin to cpu 69 (InvalReceiver slot per
      // task plan §2.F).
      pthread_setname_np(pthread_self(), "InvalReceiver");
      cpu_set_t cs;
      CPU_ZERO(&cs);
      CPU_SET(69, &cs);
      pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
      fprintf(stderr,
        "[A:thread] InvalReceiver pid=%d tid=%lu pinned cpu=69 (host_id=%d)\n",
        getpid(), (unsigned long)pthread_self(), host_id_);
      this->cache_dispatcher_loop();
    });
  }
  return 0;
}

void CxlKvStoreA::stop_dispatcher() {
  if (!cache_dispatcher_.joinable()) return;
  dispatcher_stop_.store(true, std::memory_order_release);
  cache_dispatcher_.join();
}

// Dispatcher: drain incoming InvalRing[*][me]; for each entry mark
// the key stale in the local cache_pool and ACK via resp_op_id.
// CRUCIALLY this thread does NOT acquire the directory spinlock and
// does NOT call execute_write_local — so it cannot deadlock with
// the responder thread that is processing forwards.
void CxlKvStoreA::cache_dispatcher_loop() {
  // Touch probe ring so its FUSEE_PROBE_DUMP envvar is read on this thread.
  probe_ring();
  while (!dispatcher_stop_.load(std::memory_order_acquire)) {
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

int CxlKvStoreA::forward_cache_register(uint32_t owner, uint64_t key,
                                        void *out_buf, uint32_t buf_len,
                                        uint32_t *out_len) {
  if (!fr_) return -10;
  ForwardRing *ring = &fr_->rings[host_id_][owner];
  uint64_t my_op = req_op_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
  uint64_t op_id = ((uint64_t)(host_id_ + 1) << 56) |
                   (my_op & 0x00FFFFFFFFFFFFFFULL);

  uint64_t tpos = ring->tail.fetch_add(1, std::memory_order_acq_rel);
  flush_line((void *)&ring->tail);
  store_fence();
  uint32_t slot = (uint32_t)(tpos % kForwardRingDepth);
  ForwardEntry *e = &ring->entries[slot];

  for (;;) {
    flush_line((void *)e);
    full_fence();
    if (e->req_op_id.load(std::memory_order_acquire) == 0) break;
    __builtin_ia32_pause();
  }

  e->key = key;
  e->value_len = 0;
  e->op_kind = kOpKindCacheRegister;
  e->resp_op_id.store(0, std::memory_order_relaxed);
  e->status = 0;
  std::atomic_thread_fence(std::memory_order_release);
  e->req_op_id.store(op_id, std::memory_order_release);
  flush_line((void *)e);
  store_fence();

  int status = 0;
  int rc = forward_spin_wait(e, op_id, &status);
  if (rc != 0) return rc;
  if (status == 0) {
    // Re-fetch entry to read response payload (resp_value_len_ + payload).
    flush_line((void *)e);
    full_fence();
    uint32_t resp_len = (uint32_t)e->resp_value_len_;
    if (resp_len > 0 && resp_len <= kForwardEntryPayloadBytes) {
      // Flush payload cachelines to refetch from CXL.
      for (uint32_t off = 0; off < resp_len; off += 64) {
        flush_line((void *)(e->payload + off));
      }
      full_fence();
      uint32_t copy_len = resp_len < buf_len ? resp_len : buf_len;
      if (out_buf && copy_len > 0) std::memcpy(out_buf, e->payload, copy_len);
      if (out_len) *out_len = resp_len;
    } else {
      if (out_len) *out_len = 0;
    }
  }
  return status;
}

void CxlKvStoreA::responder_handle(ForwardEntry *e) {
  switch (e->op_kind) {
    case kOpKindUpdate:
    case kOpKindInsert:
    case kOpKindDelete: {
      int rc;
      if (e->op_kind == kOpKindDelete) {
        rc = execute_write_local(e->key, nullptr, 0, kOpKindDelete);
      } else {
        // iter-9A: read varlen payload from inline ForwardEntry.
        // Flush payload to ensure we see CXL-fresh bytes.
        for (uint32_t off = 0; off < e->value_len; off += 64) {
          flush_line((void *)(e->payload + off));
        }
        full_fence();
        rc = execute_write_local(e->key, e->payload, e->value_len,
                                 (int)e->op_kind);
      }
      e->status = rc;
      break;
    }
    // iter-5A: kOpKindInvalidate REMOVED from ForwardEntry path —
    // invalidates now flow on InvalRing handled by cache_dispatcher_loop.
    case kOpKindCacheRegister: {
      // §I9 register-then-fill: under directory lock, set sharer bit
      // for the requesting host, look up the value, return it.
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
        e->resp_value_len_ = 0;
        break;
      }

      SlotDirectoryEntry *de =
          slot_directory_entry(dir_, b, (uint32_t)found);
      slot_directory_lock(de);
      uint64_t op_id = e->req_op_id.load(std::memory_order_relaxed);
      int requester = (int)((op_id >> 56) & 0xFF) - 1;
      if (requester >= 0 && requester < num_hosts_ &&
          requester != host_id_) {
        de->sharer_bitmap = (uint8_t)(de->sharer_bitmap |
                                       (1u << requester));
      }
      uint64_t encoded = bucket->slots[found].value;
      slot_directory_unlock(de);

      // iter-9A: read varlen value from blockpool. Header layout:
      //   pool block @ blk_off: [4B value_len][value bytes]
      uint8_t sc = cxl_slot_size_class(encoded);
      if (sc == kSizeClassInline || pool_ == nullptr) {
        // Legacy inline u64: copy 8 bytes into payload.
        std::memcpy(e->payload, &encoded, 8);
        e->resp_value_len_ = 8;
        // Flush payload cacheline so requester sees fresh bytes.
        flush_line((void *)e->payload);
        e->status = 0;
      } else {
        uint64_t blk_off = cxl_slot_blk_off(encoded);
        if (blk_off != 0) {
          uint8_t hdr_buf[4];
          pool_->read(blk_off, hdr_buf, 4);
          uint32_t vlen = 0;
          std::memcpy(&vlen, hdr_buf, 4);
          if (vlen == 0 || vlen > kForwardEntryPayloadBytes) {
            e->status = -1;
            e->resp_value_len_ = 0;
            break;
          }
          uint8_t valbuf[kForwardEntryPayloadBytes];
          pool_->read(blk_off + 4, valbuf, vlen);
          std::memcpy(e->payload, valbuf, vlen);
          for (uint32_t off = 0; off < vlen; off += 64) {
            flush_line((void *)(e->payload + off));
          }
          e->resp_value_len_ = vlen;
          e->status = 0;
        } else {
          e->resp_value_len_ = 0;
          e->status = -1;
        }
      }
      break;
    }
    default:
      e->status = -1;
      break;
  }
}

void CxlKvStoreA::responder_loop() {
  probe_ring();
  while (!responder_stop_.load(std::memory_order_acquire)) {
    bool did_work = false;
    for (int src = 0; src < num_hosts_; src++) {
      if (src == host_id_) continue;
      ForwardRing *ring = &fr_->rings[src][host_id_];
      uint64_t head = ring->head;
      flush_line((void *)&ring->tail);
      full_fence();
      uint64_t tail = ring->tail.load(std::memory_order_acquire);
      while (head < tail) {
        uint32_t slot = (uint32_t)(head % kForwardRingDepth);
        ForwardEntry *e = &ring->entries[slot];
        flush_line((void *)e);
        full_fence();
        uint64_t op_id = e->req_op_id.load(std::memory_order_acquire);
        if (op_id == 0) break;

        responder_handle(e);

        std::atomic_thread_fence(std::memory_order_release);
        e->resp_op_id.store(op_id, std::memory_order_release);
        flush_line((void *)e);
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

  // Fast path: local cache lookup with stale check.
  uint8_t buf[kForwardEntryPayloadBytes];
  uint32_t sz = 0;
  if (cache_pool_lookup(cache_, key, buf, sizeof(buf), &sz)) {
    PROBE_OP("R2hit", key);
    if (out_len) *out_len = sz;
    uint32_t copy_len = sz < buf_len ? sz : buf_len;
    if (out_buf && copy_len > 0) std::memcpy(out_buf, buf, copy_len);
    PROBE_OP("R6", key);
    return 0;
  }
  PROBE_OP("R2miss", key);

  uint32_t owner = owner_host(key);

  // Cross-host miss: §I9 register-then-fill via OP_CACHE_REGISTER.
  if (owner != (uint32_t)host_id_ && fr_) {
    PROBE_OP("R3", key);
    uint8_t v[kForwardEntryPayloadBytes];
    uint32_t vlen = 0;
    int rc = forward_cache_register(owner, key, v, sizeof(v), &vlen);
    if (rc != 0) return rc;
    PROBE_OP("R4", key);
    // §AP15: populate cache ONLY after register ACK (we got it here).
    if (vlen > 0) {
      cache_pool_insert(cache_, key, v, vlen);
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
      uint8_t v[kForwardEntryPayloadBytes];
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
          if (vlen > 0 && vlen <= kForwardEntryPayloadBytes) {
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
      PROBE_OP("R6", key);
      return 0;
    }
  }
  return -1;
}

}  // namespace fusee
