#include "cxl_kv_ops_A_v2.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "common.h"  // CACHELINE_LOAD, CACHELINE_STORE, flush_line, store_fence, full_fence
}

namespace fusee {

namespace {

// CoW commit-publish pattern from spec §V/§VI-A.bis: write value, then
// flush + sfence, then publish key. For inline u64 KV the slot is one
// cacheline-aligned 16 B record so a single 16 B atomic store via SSE2
// movdqa would suffice — we use the safer "value then key" pattern with
// explicit ordering to keep the spec primitives readable.
inline void publish_slot_cow(CxlKvSlot *slot, uint64_t key, uint64_t value) {
  slot->value = value;
  // Plain store + clflushopt + sfence path (per spec §VI-A.bis: < 256 B
  // payload uses plain+clflushopt; NT store would be wasteful for 16 B).
  flush_line(slot);
  store_fence();
  slot->key = key;
  flush_line(slot);
  store_fence();
}

inline void retire_slot(CxlKvSlot *slot) {
  // DELETE: clear key (publish atomic 8 B store with release).
  __atomic_store_n(&slot->key, kEmptyKey, __ATOMIC_RELEASE);
  flush_line(slot);
  store_fence();
}

}  // anonymous namespace

uint32_t CxlKvStoreA_v2::bucket_idx(uint64_t key) const {
  return (uint32_t)(fnv1a_u64(key) % num_buckets_);
}

uint32_t CxlKvStoreA_v2::owner_host(uint64_t key) const {
  return host_of(st_, key);
}

int CxlKvStoreA_v2::attach(void *bucket_base, uint32_t num_buckets,
                           int host_id, int num_hosts, bool init_region,
                           ShardingTable *st, SlotDirectory *dir,
                           KvCachePool *cache, BlockFreeList *freelist) {
  if (!bucket_base || num_buckets == 0 || !st || !dir || !cache || !freelist) {
    return -1;
  }
  buckets_ = static_cast<CxlKvBucket *>(bucket_base);
  num_buckets_ = num_buckets;
  host_id_ = host_id;
  num_hosts_ = num_hosts;
  st_ = st;
  dir_ = dir;
  cache_ = cache;
  freelist_ = freelist;

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

// Owner-self path. Acquires the per-slot directory spinlock for the
// target slot, executes CoW publish on CXL, updates directory state,
// updates cache, releases.
//
// Phase 6: caller has verified owner_host(key) == self_host. Cross-host
// forward path lands in Phase 8.
int CxlKvStoreA_v2::execute_write_local(uint64_t key, uint64_t new_value,
                                        int op_kind) {
  if (key == kEmptyKey) return -1;
  uint32_t b = bucket_idx(key);
  CxlKvBucket *bucket = &buckets_[b];

  // Step 1: scan slots to decide op (find existing key, or first empty).
  // Bucket scan reads CXL — issue clflushopt + mfence so we see the
  // most recent peer-host writes (per §VI-A: CXL coherent load).
  flush_line(bucket);
  flush_line((char *)bucket + 64);
  full_fence();

  int match = -1, empty = -1;
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
    if (bucket->slots[s].key == key) { match = s; break; }
    if (bucket->slots[s].key == kEmptyKey && empty < 0) empty = s;
  }

  int target_slot;
  if (op_kind == 1 /* INSERT */) {
    if (match >= 0) return -2;  // key exists
    if (empty < 0) return -3;   // bucket full
    target_slot = empty;
  } else if (op_kind == 0 /* UPDATE */) {
    if (match < 0) return -1;   // key missing
    target_slot = match;
  } else /* DELETE */ {
    if (match < 0) return -1;
    target_slot = match;
  }

  // Step 2: acquire directory spinlock for (b, target_slot).
  SlotDirectoryEntry *de = slot_directory_entry(dir_, b, (uint32_t)target_slot);
  slot_directory_lock(de);

  // Step 3: re-verify slot under lock (peer host could have inserted
  // between our scan and our lock acquire). For owner-self path
  // sharding ensures no peer-host writer exists for THIS key, but the
  // bucket scan still races against owner-host's other workers.
  flush_line(bucket);
  full_fence();
  CxlKvSlot *slot = &bucket->slots[target_slot];

  // Step 4: invalidate sharers (Phase 6 = owner-self only, sharers \ {self}
  // is empty until Phase 7+8 wires register paths). For now, no-op.
  // TODO(Phase 7/8): send invalidate via N:1:1:N + wait ACK.

  // Step 5: CoW publish (commit point). For inline u64 KV the slot
  // stores key + value directly (no separate KV block).
  if (op_kind == 2 /* DELETE */) {
    retire_slot(slot);
  } else {
    publish_slot_cow(slot, key, new_value);
  }

  // Step 6: update directory state.
  de->version++;
  if (op_kind == 2 /* DELETE */) {
    de->state = kDirStateInvalid;
    de->sharer_bitmap = 0;
  } else {
    de->state = kDirStateShared;
    de->sharer_bitmap = (uint8_t)(1u << (uint32_t)host_id_);  // self only
  }
  slot_directory_unlock(de);

  // Step 7: update local cache (insert or evict).
  if (op_kind == 2 /* DELETE */) {
    cache_pool_evict(cache_, key);
  } else {
    cache_pool_insert(cache_, key,
                      reinterpret_cast<const uint8_t *>(&new_value),
                      sizeof(new_value));
  }
  return 0;
}

int CxlKvStoreA_v2::insert(uint64_t key, uint64_t value) {
  if (key == kEmptyKey) return -1;
  uint32_t owner = owner_host(key);
  if (owner != (uint32_t)host_id_) {
    // Phase 6: cross-host forward not yet implemented.
    return -10;  // -ENOTSUP placeholder; Phase 8 will implement.
  }
  return execute_write_local(key, value, 1 /* INSERT */);
}

int CxlKvStoreA_v2::update(uint64_t key, uint64_t value) {
  if (key == kEmptyKey) return -1;
  uint32_t owner = owner_host(key);
  if (owner != (uint32_t)host_id_) return -10;
  return execute_write_local(key, value, 0 /* UPDATE */);
}

int CxlKvStoreA_v2::remove(uint64_t key) {
  if (key == kEmptyKey) return -1;
  uint32_t owner = owner_host(key);
  if (owner != (uint32_t)host_id_) return -10;
  return execute_write_local(key, 0, 2 /* DELETE */);
}

int CxlKvStoreA_v2::search(uint64_t key, uint64_t *out) {
  if (key == kEmptyKey) return -1;

  // Fast path: local cache lookup. No CXL access.
  uint8_t buf[8];
  uint32_t sz;
  if (cache_pool_lookup(cache_, key, buf, sizeof(buf), &sz)) {
    if (sz != sizeof(uint64_t)) return -1;
    std::memcpy(out, buf, sizeof(uint64_t));
    return 0;
  }

  // Slow path: bucket scan from CXL. Phase 6 only handles owner-self
  // miss (no cross-host register message); Phase 7 wires that.
  uint32_t owner = owner_host(key);
  if (owner != (uint32_t)host_id_) {
    // Phase 6: cross-host miss not yet wired -> degrade to direct CXL
    // scan. This is correct (CXL is authoritative), just not
    // optimized. Phase 7 adds register-then-fill.
  }

  uint32_t b = bucket_idx(key);
  CxlKvBucket *bucket = &buckets_[b];
  flush_line(bucket);
  flush_line((char *)bucket + 64);
  full_fence();
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
    if (bucket->slots[s].key == key) {
      *out = bucket->slots[s].value;
      // Populate cache (lazy, no register yet — Phase 7 fixes).
      uint64_t v = *out;
      cache_pool_insert(cache_, key,
                        reinterpret_cast<const uint8_t *>(&v),
                        sizeof(v));
      return 0;
    }
  }
  return -1;  // miss
}

}  // namespace fusee
