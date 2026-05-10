#include "cxl_cache_pool.h"

#include <cstring>
#include <pthread.h>

#include "cxl_sharding.h"  // for sharding_hash_u64

namespace fusee {

static inline uint32_t bucket_idx(const KvCachePool *pool, uint64_t key) {
  // Reuse FNV-1a from sharding for consistency. Mask to bucket count.
  return (uint32_t)(sharding_hash_u64(key) & pool->mask);
}

int cache_pool_init(KvCachePool *pool, void *backing_mem,
                    uint32_t num_buckets) {
  if (!pool || !backing_mem || num_buckets == 0) return -1;
  if ((num_buckets & (num_buckets - 1)) != 0) return -1;  // power of two

  pool->num_buckets = num_buckets;
  pool->mask = num_buckets - 1;
  pool->global_epoch.store(0, std::memory_order_relaxed);
  pool->buckets = static_cast<KvCacheBucket *>(backing_mem);

  for (uint32_t b = 0; b < num_buckets; b++) {
    auto *bk = &pool->buckets[b];
    if (pthread_spin_init(&bk->spinlock, PTHREAD_PROCESS_SHARED) != 0) {
      return -2;
    }
    bk->epoch.store(0, std::memory_order_relaxed);
    for (int i = 0; i < kCacheEntriesPerBucket; i++) {
      auto *e = &bk->entries[i];
      e->key.store(kCacheKeyEmpty, std::memory_order_relaxed);
      e->stale.store(0, std::memory_order_relaxed);
      e->value_size = 0;
      e->lru_epoch.store(0, std::memory_order_relaxed);
    }
  }
  return 0;
}

void cache_pool_destroy(KvCachePool *pool) {
  if (!pool || !pool->buckets) return;
  for (uint32_t b = 0; b < pool->num_buckets; b++) {
    pthread_spin_destroy(&pool->buckets[b].spinlock);
  }
  pool->buckets = nullptr;
  pool->num_buckets = 0;
}

bool cache_pool_lookup(KvCachePool *pool, uint64_t key, uint8_t *out,
                       uint32_t out_cap, uint32_t *value_size) {
  if (!pool || key == kCacheKeyEmpty || key == kCacheKeyTomb) return false;
  auto *bk = &pool->buckets[bucket_idx(pool, key)];

  // Fast path: scan entries with plain (acquire) loads.
  for (int i = 0; i < kCacheEntriesPerBucket; i++) {
    auto *e = &bk->entries[i];
    if (e->key.load(std::memory_order_acquire) != key) continue;
    if (e->stale.load(std::memory_order_acquire) != 0) return false;
    // Found, fresh. Copy.
    uint32_t sz = e->value_size;
    if (sz > out_cap) sz = out_cap;
    std::memcpy(out, e->value_bytes, sz);
    if (value_size) *value_size = sz;
    // LRU touch (relaxed RMW).
    uint64_t ge = pool->global_epoch.load(std::memory_order_relaxed);
    e->lru_epoch.store(ge, std::memory_order_relaxed);
    return true;
  }
  return false;
}

int cache_pool_insert(KvCachePool *pool, uint64_t key,
                      const uint8_t *value, uint32_t value_size) {
  if (!pool || key == kCacheKeyEmpty || key == kCacheKeyTomb) return -1;
  if (value_size > kCacheValueMaxBytes) return -1;
  auto *bk = &pool->buckets[bucket_idx(pool, key)];

  pthread_spin_lock(&bk->spinlock);
  // Search: prefer matching key (update); else first empty/tomb slot.
  KvCacheEntry *match = nullptr;
  KvCacheEntry *empty_slot = nullptr;
  KvCacheEntry *lru_slot = nullptr;
  uint64_t lru_epoch = ~0ULL;
  for (int i = 0; i < kCacheEntriesPerBucket; i++) {
    auto *e = &bk->entries[i];
    uint64_t k = e->key.load(std::memory_order_relaxed);
    if (k == key) { match = e; break; }
    if (!empty_slot && (k == kCacheKeyEmpty || k == kCacheKeyTomb)) {
      empty_slot = e;
    }
    uint64_t le = e->lru_epoch.load(std::memory_order_relaxed);
    if (le < lru_epoch) { lru_epoch = le; lru_slot = e; }
  }
  KvCacheEntry *target = match ? match : (empty_slot ? empty_slot : lru_slot);
  if (!target) {
    pthread_spin_unlock(&bk->spinlock);
    return -1;
  }
  // Update payload.
  target->value_size = value_size;
  std::memcpy(target->value_bytes, value, value_size);
  uint64_t ge = pool->global_epoch.fetch_add(1, std::memory_order_relaxed) + 1;
  target->lru_epoch.store(ge, std::memory_order_relaxed);
  // Publish: clear stale before key (release ordering).
  target->stale.store(0, std::memory_order_release);
  target->key.store(key, std::memory_order_release);
  // iter-10A Phase 1.B: bump bucket epoch so any TLS reader observing
  // an older epoch detects stale and re-fetches.
  bk->epoch.fetch_add(1, std::memory_order_release);
  pthread_spin_unlock(&bk->spinlock);
  return 0;
}

void cache_pool_set_stale(KvCachePool *pool, uint64_t key) {
  if (!pool || key == kCacheKeyEmpty || key == kCacheKeyTomb) return;
  auto *bk = &pool->buckets[bucket_idx(pool, key)];
  for (int i = 0; i < kCacheEntriesPerBucket; i++) {
    auto *e = &bk->entries[i];
    if (e->key.load(std::memory_order_acquire) == key) {
      e->stale.store(1, std::memory_order_release);
      // iter-10A Phase 1.B: bump bucket epoch so per-worker TLS
      // caches detect stale on next lookup of any key in this bucket.
      bk->epoch.fetch_add(1, std::memory_order_release);
      return;
    }
  }
}

void cache_pool_evict(KvCachePool *pool, uint64_t key) {
  if (!pool || key == kCacheKeyEmpty || key == kCacheKeyTomb) return;
  auto *bk = &pool->buckets[bucket_idx(pool, key)];
  pthread_spin_lock(&bk->spinlock);
  for (int i = 0; i < kCacheEntriesPerBucket; i++) {
    auto *e = &bk->entries[i];
    if (e->key.load(std::memory_order_relaxed) == key) {
      e->key.store(kCacheKeyTomb, std::memory_order_release);
      e->stale.store(0, std::memory_order_relaxed);
      e->value_size = 0;
      // iter-10A Phase 1.B: bump bucket epoch.
      bk->epoch.fetch_add(1, std::memory_order_release);
      break;
    }
  }
  pthread_spin_unlock(&bk->spinlock);
}

}  // namespace fusee
