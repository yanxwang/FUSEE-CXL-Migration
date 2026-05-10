#ifndef FUSEE_CXL_TLS_CACHE_H_
#define FUSEE_CXL_TLS_CACHE_H_

// Protocol A v2 — per-worker TLS (thread-local) cache layer (iter-10A
// Phase 1).
//
// Sits ABOVE the shared `KvCachePool`. Each worker process has its own
// private TlsCache (NOT MAP_SHARED, NOT mmap'd between workers, NOT in
// CXL). Hot Zipf keys cached locally → 0 cross-core MESI traffic, 0
// shared-bucket lock contention.
//
// Design rationale:
//   - iter-9A redo Phase 3 path_decomp on workload-A KV=1024 T=64
//     cache=on found R1 = 7.16 µs dominated by `cache_pool_lookup`'s
//     1024-B `memcpy(value_bytes)`. The 1048-B `KvCacheEntry` is 17
//     cachelines; under Zipf hot key + 32 concurrent inserters, every
//     reader fetches all 16 data cachelines cross-core (MESI bouncing).
//   - TLS cache puts each worker's hot keys in private DRAM. memcpy of
//     value_bytes is purely L1/L2 — no cross-core fetch. TLS HIT path
//     should be ~50-100 ns vs current 7160 ns.
//
// Coherence with shared cache_pool (§I9 strict-A INVARIANT):
//   - `KvCacheBucket` gets a NEW `std::atomic<uint64_t> epoch` field
//     (per-bucket, ~64K atomics across hashtable, 4 MiB total).
//   - cache_pool_insert / cache_pool_evict / cache_pool_set_stale all
//     bump `bucket_epoch[hash(key) & mask]`.
//   - TlsCache stores `(key, observed_epoch, value_size, value_bytes)`
//     per entry. On TLS hit, reader RELOADS the bucket epoch and
//     compares; mismatch → evict TLS entry + fall through.
//   - Result: any cross-host invalidate (via InvalReceiver →
//     cache_pool_set_stale) or local CoW publish (via
//     execute_write_local → cache_pool_insert) bumps the bucket epoch
//     → next TLS reader of any key in that bucket detects stale →
//     re-fetch from shared cache_pool → linearizability preserved.
//
// Single bucket_epoch atomic load per TLS check is ~5-50 ns (1
// cacheline cross-core in worst case) vs the current 16-cacheline
// memcpy bounce (~3-7 µs). Net win is substantial.
//
// Per-worker memory:
//   N=256 entries:    256 × 1088 B  ≈  272 KiB
//   N=1024 entries:  1024 × 1088 B  ≈  1.1 MiB
//   N=8192 entries:  8192 × 1088 B  ≈  8.9 MiB
// At T=64 worker:    N=8192 → ~570 MiB total DRAM (g3/g4 96 GB → 0.6%)
//
// Hashing: open addressing, linear probe up to kTlsProbeMax slots.
// Insert: scan probe slots → use existing-key match / first-empty /
// hash-position-replace (simple bump-cursor LRU surrogate).

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fusee {

constexpr int kTlsProbeMax           = 8;     // linear probe limit
constexpr uint32_t kTlsValueMaxBytes = 1024;  // matches kCacheValueMaxBytes
constexpr uint64_t kTlsKeyEmpty      = 0;

struct alignas(64) TlsCacheEntry {
  uint64_t key;                    // 0 = empty; non-zero = real key
  uint64_t observed_epoch;         // copy of bucket_epoch at insert time
  uint32_t value_size;
  uint32_t _pad_a;
  uint8_t  value_bytes[kTlsValueMaxBytes];
  // sizeof = 8+8+4+4+1024 = 1048 B; alignas(64) pads to 1088 B
};

struct TlsCache {
  uint32_t num_entries;     // power of 2; env-driven
  uint32_t mask;            // num_entries - 1
  TlsCacheEntry *entries;   // posix_memalign(64, num_entries * sizeof(TlsCacheEntry))
  // counters (DRAM-local, no atomic — accessed only by owning thread)
  uint64_t hits;
  uint64_t misses;
  uint64_t epoch_evictions;
  uint64_t replacements;
};

// Init: allocate `num_entries` slot region, zero, install in `t`.
// num_entries must be power of 2; minimum 64; maximum bounded by malloc.
int tls_cache_init(TlsCache *t, uint32_t num_entries);

// Tear down: free entries.
void tls_cache_destroy(TlsCache *t);

// Lookup. Returns true on HIT (epoch matches current bucket_epoch);
// false on MISS or stale-entry-evict. On HIT, copies up to out_cap
// bytes into out and writes value_size to *out_size_p. The bucket
// epoch is loaded from `cur_bucket_epoch` (the caller already has the
// `KvCachePool *` and looked up the bucket; we pass the atomic load
// in directly so the caller can decide cache_pool_lookup branching).
bool tls_lookup(TlsCache *t, uint64_t key, uint64_t cur_bucket_epoch,
                uint8_t *out, uint32_t out_cap, uint32_t *out_size_p);

// Insert. `cur_bucket_epoch` is the bucket epoch AT THE TIME of the
// fetch from CXL / shared cache_pool. We store this so a subsequent
// reader can compare its observation to the current value.
void tls_insert(TlsCache *t, uint64_t key, const uint8_t *value,
                uint32_t value_size, uint64_t cur_bucket_epoch);

// Evict (e.g. on local DELETE). Idempotent.
void tls_evict(TlsCache *t, uint64_t key);

// Diagnostic dump (called at worker exit if FUSEE_TLS_DIAG=1).
void tls_cache_dump(const TlsCache *t, int worker_id);

}  // namespace fusee

#endif  // FUSEE_CXL_TLS_CACHE_H_
