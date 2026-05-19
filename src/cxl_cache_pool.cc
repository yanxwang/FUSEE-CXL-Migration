#include "cxl_cache_pool.h"

#include <cstring>
#include <pthread.h>
#if defined(__x86_64__)
#include <x86intrin.h>
#endif

#include "cxl_read_guard.h"  // FUSEE_LRU_SAMPLE
#include "cxl_sharding.h"    // for sharding_hash_u64

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
    // iter-10A Phase 2: spinlock retained for cache_pool_destroy
    // teardown only — insert/evict no longer take it. pthread_spin_init
    // is still cheap to call so we leave it (avoids changing destroy
    // path + leaves a bypass if a future iter wants the lock back).
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
      e->seq.store(0, std::memory_order_relaxed);
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

// iter-10A Phase 2: lookup is a seqlock reader.
// Algorithm:
//   for each entry e in bucket:
//     s1 = e.seq.load(acquire)
//     if s1 odd → entry mid-update, skip (treat as not found in this scan
//                   — hot keys will be retried next op)
//     if e.key != myKey → continue
//     if e.stale → return false (stale=miss)
//     copy value_bytes
//     s2 = e.seq.load(acquire)
//     if s1 != s2 → entry got updated mid-read, retry by continuing scan
//     return true
bool cache_pool_lookup(KvCachePool *pool, uint64_t key, uint8_t *out,
                       uint32_t out_cap, uint32_t *value_size) {
  if (!pool || key == kCacheKeyEmpty || key == kCacheKeyTomb) return false;
  auto *bk = &pool->buckets[bucket_idx(pool, key)];

  for (int i = 0; i < kCacheEntriesPerBucket; i++) {
    auto *e = &bk->entries[i];
    uint32_t s1 = e->seq.load(std::memory_order_acquire);
    if (s1 & 1) continue;  // mid-update, skip — caller may retry whole search
    if (e->key.load(std::memory_order_acquire) != key) continue;
    if (e->stale.load(std::memory_order_acquire) != 0) return false;
    uint32_t sz = e->value_size;
    if (sz > out_cap) sz = out_cap;
    std::memcpy(out, e->value_bytes, sz);
    uint32_t s2 = e->seq.load(std::memory_order_acquire);
    if (s1 != s2) {
      // Entry got modified mid-read; the bytes we copied may be torn.
      // Treat as miss — caller will fall through to forward_read /
      // owner-self path and re-populate.
      continue;
    }
    if (value_size) *value_size = sz;
    // LRU touch (relaxed RMW). iter-14A F2: when FUSEE_LRU_SAMPLE=1,
    // sample 1/64 hits via rdtsc low bits to remove MESI ping-pong on
    // cacheline 0 of KvCacheEntry under hot Zipf multi-reader.
#if FUSEE_LRU_SAMPLE
    if ((__rdtsc() & 0x3FULL) == 0) {
      uint64_t ge = pool->global_epoch.load(std::memory_order_relaxed);
      e->lru_epoch.store(ge, std::memory_order_relaxed);
    }
#else
    uint64_t ge = pool->global_epoch.load(std::memory_order_relaxed);
    e->lru_epoch.store(ge, std::memory_order_relaxed);
#endif
    return true;
  }
  return false;
}

// iter-10A Phase 2: lock-free seqlock-based insert.
// Algorithm:
//   retry:
//     scan bucket for (a) match → update, (b) empty/tomb, (c) LRU-min
//     pick target per priority a > b > c
//     CAS target.seq from EVEN to EVEN+1 (claim)
//       if CAS fails (someone else mid-update), retry from scan
//     write payload + key + clear stale + bump LRU + bump bucket epoch
//     store target.seq = EVEN+2 (publish + release)
int cache_pool_insert(KvCachePool *pool, uint64_t key,
                      const uint8_t *value, uint32_t value_size) {
  if (!pool || key == kCacheKeyEmpty || key == kCacheKeyTomb) return -1;
  if (value_size > kCacheValueMaxBytes) return -1;
  auto *bk = &pool->buckets[bucket_idx(pool, key)];

  // Bounded retry: avoid pathological live-lock if every entry is
  // perpetually mid-update. 8 retries is generous (4 entries × 2).
  for (int retry = 0; retry < 8; retry++) {
    KvCacheEntry *match = nullptr;
    KvCacheEntry *empty_slot = nullptr;
    KvCacheEntry *lru_slot = nullptr;
    uint64_t lru_epoch = ~0ULL;
    for (int i = 0; i < kCacheEntriesPerBucket; i++) {
      auto *e = &bk->entries[i];
      uint64_t k = e->key.load(std::memory_order_acquire);
      if (k == key) { match = e; break; }
      if (!empty_slot && (k == kCacheKeyEmpty || k == kCacheKeyTomb)) {
        empty_slot = e;
      }
      uint64_t le = e->lru_epoch.load(std::memory_order_relaxed);
      if (le < lru_epoch) { lru_epoch = le; lru_slot = e; }
    }
    KvCacheEntry *target = match ? match : (empty_slot ? empty_slot : lru_slot);
    if (!target) return -1;  // bucket somehow empty (impossible w/ 4 entries)

    // Claim target via CAS even → odd.
    uint32_t s_old = target->seq.load(std::memory_order_acquire);
    if (s_old & 1) {
      // Already mid-update by someone else. Spin-pause + retry from scan.
      __builtin_ia32_pause();
      continue;
    }
    if (!target->seq.compare_exchange_weak(
            s_old, s_old + 1, std::memory_order_acquire,
            std::memory_order_relaxed)) {
      // Lost the CAS race. Retry.
      __builtin_ia32_pause();
      continue;
    }

    // Now exclusively own target. Write payload.
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
    // Release seq.
    target->seq.store(s_old + 2, std::memory_order_release);
    return 0;
  }
  // Retry budget exhausted. Caller treats this as soft failure (the
  // cache_pool_insert is best-effort; the WRITE to CXL slot already
  // happened and is durable). The bucket epoch is bumped above —
  // any TLS reader auto-evicts.
  return -2;
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

// iter-10A Phase 2: lock-free seqlock-based evict.
void cache_pool_evict(KvCachePool *pool, uint64_t key) {
  if (!pool || key == kCacheKeyEmpty || key == kCacheKeyTomb) return;
  auto *bk = &pool->buckets[bucket_idx(pool, key)];
  for (int retry = 0; retry < 8; retry++) {
    KvCacheEntry *target = nullptr;
    for (int i = 0; i < kCacheEntriesPerBucket; i++) {
      auto *e = &bk->entries[i];
      if (e->key.load(std::memory_order_acquire) == key) {
        target = e;
        break;
      }
    }
    if (!target) return;  // not present, nothing to do

    uint32_t s_old = target->seq.load(std::memory_order_acquire);
    if (s_old & 1) { __builtin_ia32_pause(); continue; }
    if (!target->seq.compare_exchange_weak(
            s_old, s_old + 1, std::memory_order_acquire,
            std::memory_order_relaxed)) {
      __builtin_ia32_pause();
      continue;
    }

    target->key.store(kCacheKeyTomb, std::memory_order_release);
    target->stale.store(0, std::memory_order_relaxed);
    target->value_size = 0;
    bk->epoch.fetch_add(1, std::memory_order_release);
    target->seq.store(s_old + 2, std::memory_order_release);
    return;
  }
}

}  // namespace fusee
