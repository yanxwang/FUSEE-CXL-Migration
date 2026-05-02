#include "cxl_kv_ops_A.h"

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
  const uint64_t kBudgetUs = 200000;  // 200 ms
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
int CxlKvStoreA::execute_write_local(uint64_t key, uint64_t new_value,
                                     int op_kind) {
  if (key == kEmptyKey) return -1;
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

  flush_line(bucket);
  full_fence();
  CxlKvSlot *slot = &bucket->slots[target_slot];

  // Step 4: invalidate sharers \ {self}. spec §I9 / I10.
  // We must broadcast OP_INVALIDATE to every host bit set in
  // sharer_bitmap other than self, then wait for all ACKs, BEFORE
  // publishing the new slot value. This is the strict-A
  // linearizability point.
  uint8_t bitmap = de->sharer_bitmap;
  if (op_kind != kOpKindInsert /* INSERT: no prior sharers */ &&
      num_hosts_ > 1 && fr_) {
    for (int h = 0; h < num_hosts_; h++) {
      if (h == host_id_) continue;
      if ((bitmap & (1u << h)) == 0) continue;
      // Send invalidate; ignore status (target may have evicted already).
      forward_invalidate((uint32_t)h, key);
    }
  }

  // Step 5: CoW publish.
  if (op_kind == kOpKindDelete) {
    retire_slot(slot);
  } else {
    // CoW: alloc new block, write value, encode slot.
    uint64_t blk_off = pool_->alloc();
    if (blk_off == 0) {
      slot_directory_unlock(de);
      return -4;  // pool exhausted
    }
    pool_->write(blk_off, &new_value, sizeof(new_value));
    uint8_t fp = key_fingerprint(key);
    uint8_t sc = kSizeClassBlock256;  // iter-4A-redo: single size class
    uint64_t encoded = cxl_slot_pack(blk_off, sc, fp);
    publish_slot_cow(slot, key, encoded);
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
  slot_directory_unlock(de);

  // Step 7: own cache.
  if (op_kind == kOpKindDelete) {
    cache_pool_evict(cache_, key);
  } else {
    cache_pool_insert(cache_, key,
                      reinterpret_cast<const uint8_t *>(&new_value),
                      sizeof(new_value));
  }
  return 0;
}

int CxlKvStoreA::insert(uint64_t key, uint64_t value) {
  if (key == kEmptyKey) return -1;
  uint32_t owner = owner_host(key);
  if (owner != (uint32_t)host_id_) {
    return forward_to_owner(owner, key, value, kOpKindInsert);
  }
  return execute_write_local(key, value, kOpKindInsert);
}

int CxlKvStoreA::update(uint64_t key, uint64_t value) {
  if (key == kEmptyKey) return -1;
  uint32_t owner = owner_host(key);
  if (owner != (uint32_t)host_id_) {
    return forward_to_owner(owner, key, value, kOpKindUpdate);
  }
  return execute_write_local(key, value, kOpKindUpdate);
}

int CxlKvStoreA::remove(uint64_t key) {
  if (key == kEmptyKey) return -1;
  uint32_t owner = owner_host(key);
  if (owner != (uint32_t)host_id_) {
    return forward_to_owner(owner, key, 0, kOpKindDelete);
  }
  return execute_write_local(key, 0, kOpKindDelete);
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
    responder_ = std::thread([this]() { this->responder_loop(); });
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
                                  uint64_t value, int op_kind) {
  if (!fr_) return -10;
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
  e->value = value;
  e->op_kind = (uint8_t)op_kind;
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

int CxlKvStoreA::forward_invalidate(uint32_t target_host, uint64_t key) {
  return forward_to_owner(target_host, key, 0, kOpKindInvalidate);
}

int CxlKvStoreA::forward_cache_register(uint32_t owner, uint64_t key,
                                        uint64_t *out_value) {
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
  e->value = 0;
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
    // Read e->value AFTER we've spun and confirmed resp_op_id matches
    // and BEFORE the slot is freed. forward_spin_wait already cleared
    // req_op_id; we must read value before that, but since we got
    // status from forward_spin_wait, it has already read. Re-read e:
    // actually the spin_wait reads e but does not return value. We
    // need the value field from the responder's response. Re-fetch:
    flush_line((void *)e);
    full_fence();
    *out_value = e->value;
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
        rc = execute_write_local(e->key, 0, kOpKindDelete);
      } else {
        rc = execute_write_local(e->key, e->value, (int)e->op_kind);
      }
      e->status = rc;
      break;
    }
    case kOpKindInvalidate: {
      // Mark cache stale (lazy stale flag, AP4 — no physical delete).
      cache_pool_set_stale(cache_, e->key);
      e->status = 0;
      break;
    }
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
        e->value = 0;
        break;
      }

      SlotDirectoryEntry *de =
          slot_directory_entry(dir_, b, (uint32_t)found);
      slot_directory_lock(de);

      // Identify requesting host from op_id high byte (encoded in
      // forward_cache_register as ((host_id_ + 1) << 56)).
      uint64_t op_id = e->req_op_id.load(std::memory_order_relaxed);
      int requester = (int)((op_id >> 56) & 0xFF) - 1;
      if (requester >= 0 && requester < num_hosts_ &&
          requester != host_id_) {
        de->sharer_bitmap = (uint8_t)(de->sharer_bitmap |
                                       (1u << requester));
      }
      uint64_t encoded = bucket->slots[found].value;
      slot_directory_unlock(de);

      // Decode block ptr, fetch value bytes.
      uint64_t blk_off = cxl_slot_blk_off(encoded);
      if (blk_off != 0 && pool_) {
        uint64_t v = 0;
        pool_->read(blk_off, &v, sizeof(v));
        e->value = v;
        e->status = 0;
      } else {
        e->value = 0;
        e->status = -1;
      }
      break;
    }
    default:
      e->status = -1;
      break;
  }
}

void CxlKvStoreA::responder_loop() {
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
}

int CxlKvStoreA::search(uint64_t key, uint64_t *out) {
  if (key == kEmptyKey) return -1;

  // Fast path: local cache lookup with stale check.
  uint8_t buf[8];
  uint32_t sz;
  if (cache_pool_lookup(cache_, key, buf, sizeof(buf), &sz)) {
    if (sz != sizeof(uint64_t)) return -1;
    std::memcpy(out, buf, sizeof(uint64_t));
    return 0;
  }

  uint32_t owner = owner_host(key);

  // Cross-host miss: §I9 register-then-fill via OP_CACHE_REGISTER.
  if (owner != (uint32_t)host_id_ && fr_) {
    uint64_t v = 0;
    int rc = forward_cache_register(owner, key, &v);
    if (rc != 0) return rc;
    // §AP15: populate cache ONLY after register ACK (we got it here).
    cache_pool_insert(cache_, key,
                      reinterpret_cast<const uint8_t *>(&v),
                      sizeof(v));
    *out = v;
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
      uint64_t blk_off = cxl_slot_blk_off(encoded);
      uint64_t v = 0;
      if (blk_off != 0 && pool_) {
        pool_->read(blk_off, &v, sizeof(v));
      }
      *out = v;
      cache_pool_insert(cache_, key,
                        reinterpret_cast<const uint8_t *>(&v),
                        sizeof(v));
      return 0;
    }
  }
  return -1;
}

}  // namespace fusee
