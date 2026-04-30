#ifndef FUSEE_CXL_CACHE_POOL_H_
#define FUSEE_CXL_CACHE_POOL_H_

// Protocol A v2 — same-host shared KV cache pool.
//
// Spec: docs/design_goals.md §I3 (MAP_SHARED across same-host
// workers; sharer dimension = host, not worker), §I4 (lazy stale flag,
// not physical delete on invalidate), AP3 (no per-worker private
// cache), AP4 (no physical delete on invalidate).
//
// Hashmap from key → cache entry. Entry stores (key, value bytes
// pointer into a per-pool slab, size, stale flag, LRU epoch). One
// pool per physical host, mmap'd MAP_SHARED|MAP_ANONYMOUS pre-fork
// so all workers on that host share it.
//
// Concurrency: open-addressing fixed-size hashmap with per-bucket
// host_local_spinlock_t (PROCESS_SHARED). Lookup is lock-free on the
// fast path (plain load + stale-byte check); insert/update/evict
// take the bucket lock. The cache fast-path latency target is
// ≤ 100 ns (plan §Phase 3 bottleneck check).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <pthread.h>

#include "cxl_directory.h"  // for host_local_spinlock_t typedef

namespace fusee {

constexpr uint32_t kCacheValueMaxBytes = 1024;   // pad to MAX of expected sizes
constexpr uint64_t kCacheKeyEmpty       = 0;     // sentinel
constexpr uint64_t kCacheKeyTomb        = ~0ULL; // tombstone (after evict)

struct alignas(64) KvCacheEntry {
  std::atomic<uint64_t> key;          // 8 B; kCacheKeyEmpty / kCacheKeyTomb / real key
  std::atomic<uint8_t>  stale;        // 1 B; release-store on invalidate
  uint8_t  _pad_a[3];
  uint32_t value_size;                // 4 B
  std::atomic<uint64_t> lru_epoch;    // 8 B; relaxed RMW on hit
  uint8_t  value_bytes[kCacheValueMaxBytes];  // 1024 B inline
  // total = 8 + 4 + 4 + 8 + 1024 = 1048 B; pad implicit by alignas(64)
};

// Bucket = chain of N entries; collision resolved by linear scan.
constexpr int kCacheEntriesPerBucket = 4;

struct alignas(64) KvCacheBucket {
  host_local_spinlock_t spinlock;       // 4 B
  uint32_t _pad_a;                      // 4 B  -> 8 total
  uint8_t  _pad_b[56];                  // pad spinlock to its own cacheline
  KvCacheEntry entries[kCacheEntriesPerBucket];
};

struct KvCachePool {
  uint32_t num_buckets;                 // power of two for mask
  uint32_t mask;
  std::atomic<uint64_t> global_epoch;   // for LRU; bump on every successful insert
  KvCacheBucket *buckets;               // backing memory passed in
};

// Bytes for cache pool of `num_buckets` (power of two).
inline std::size_t cache_pool_bytes(uint32_t num_buckets) {
  return sizeof(KvCacheBucket) * num_buckets;
}

// Init from caller-provided MAP_SHARED|MAP_ANONYMOUS region.
int cache_pool_init(KvCachePool *pool, void *backing_mem, uint32_t num_buckets);

// Tear down (destroy spinlocks).
void cache_pool_destroy(KvCachePool *pool);

// Lookup. Returns true if entry found AND not stale; copies value bytes
// into `out` (caller-provided buffer of `out_cap` bytes; written
// `value_size` is set on success). Fast path: plain loads + stale check.
//
// Note on staleness semantics: we deliberately treat "stale=1" as a
// MISS at the API level (returns false). Spec §I4 says fast-path
// reader does `if (entry && !entry.stale) return entry.value`. A stale
// entry remains physically present (lazy delete) until LRU eviction
// or a re-fill on the slow path.
bool cache_pool_lookup(KvCachePool *pool, uint64_t key, uint8_t *out,
                       uint32_t out_cap, uint32_t *value_size);

// Insert or update. Replaces stale flag (sets to 0). Returns 0 on
// success, -1 on bucket full + LRU eviction needed (caller decides).
// `value_size` must be ≤ kCacheValueMaxBytes.
int cache_pool_insert(KvCachePool *pool, uint64_t key, const uint8_t *value,
                      uint32_t value_size);

// Set stale flag (release-store). Idempotent. No-op if key not present.
void cache_pool_set_stale(KvCachePool *pool, uint64_t key);

// Evict by key (physical delete; tombstones the slot). Used by LRU
// background sweeper or explicit clear. Idempotent.
void cache_pool_evict(KvCachePool *pool, uint64_t key);

}  // namespace fusee

#endif  // FUSEE_CXL_CACHE_POOL_H_
