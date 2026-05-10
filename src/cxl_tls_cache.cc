#include "cxl_tls_cache.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cxl_sharding.h"  // for sharding_hash_u64

namespace fusee {

static inline uint32_t tls_bucket_idx(const TlsCache *t, uint64_t key) {
  return (uint32_t)(sharding_hash_u64(key) & t->mask);
}

int tls_cache_init(TlsCache *t, uint32_t num_entries) {
  if (!t || num_entries == 0) return -1;
  if ((num_entries & (num_entries - 1)) != 0) return -1;  // power of 2
  if (num_entries < 64) num_entries = 64;

  size_t bytes = (size_t)num_entries * sizeof(TlsCacheEntry);
  void *mem = nullptr;
  if (posix_memalign(&mem, 64, bytes) != 0 || !mem) return -2;
  std::memset(mem, 0, bytes);  // key=0 = empty everywhere

  t->num_entries = num_entries;
  t->mask = num_entries - 1;
  t->entries = static_cast<TlsCacheEntry *>(mem);
  t->hits = 0;
  t->misses = 0;
  t->epoch_evictions = 0;
  t->replacements = 0;
  return 0;
}

void tls_cache_destroy(TlsCache *t) {
  if (!t) return;
  if (t->entries) {
    free(t->entries);
    t->entries = nullptr;
  }
  t->num_entries = 0;
  t->mask = 0;
}

bool tls_lookup(TlsCache *t, uint64_t key, uint64_t cur_bucket_epoch,
                uint8_t *out, uint32_t out_cap, uint32_t *out_size_p) {
  if (!t || !t->entries || key == kTlsKeyEmpty) return false;
  uint32_t h = tls_bucket_idx(t, key);
  for (int probe = 0; probe < kTlsProbeMax; probe++) {
    uint32_t i = (h + probe) & t->mask;
    TlsCacheEntry *e = &t->entries[i];
    if (e->key == kTlsKeyEmpty) {
      // empty slot reached, key not in TLS
      t->misses++;
      return false;
    }
    if (e->key != key) continue;
    // key match — verify epoch
    if (e->observed_epoch != cur_bucket_epoch) {
      // stale: invalidated since insert. evict.
      e->key = kTlsKeyEmpty;
      t->epoch_evictions++;
      t->misses++;
      return false;
    }
    // HIT
    uint32_t sz = e->value_size;
    if (sz > out_cap) sz = out_cap;
    std::memcpy(out, e->value_bytes, sz);
    if (out_size_p) *out_size_p = sz;
    t->hits++;
    return true;
  }
  // probe limit reached
  t->misses++;
  return false;
}

void tls_insert(TlsCache *t, uint64_t key, const uint8_t *value,
                uint32_t value_size, uint64_t cur_bucket_epoch) {
  if (!t || !t->entries || key == kTlsKeyEmpty) return;
  if (value_size > kTlsValueMaxBytes) return;
  uint32_t h = tls_bucket_idx(t, key);
  // Strategy: prefer matching key (update); else first empty; else
  // replace position h (simple LRU surrogate — bump-cursor at hash slot
  // — hot Zipf keys self-stabilize because they're inserted constantly).
  TlsCacheEntry *first_empty = nullptr;
  for (int probe = 0; probe < kTlsProbeMax; probe++) {
    uint32_t i = (h + probe) & t->mask;
    TlsCacheEntry *e = &t->entries[i];
    if (e->key == key) {
      // update in place
      e->value_size = value_size;
      std::memcpy(e->value_bytes, value, value_size);
      e->observed_epoch = cur_bucket_epoch;
      return;
    }
    if (e->key == kTlsKeyEmpty && !first_empty) first_empty = e;
  }
  // No match in probe range. Use first empty if any; else replace h.
  TlsCacheEntry *target = first_empty ? first_empty : &t->entries[h];
  if (target->key != kTlsKeyEmpty) t->replacements++;
  target->key = key;
  target->value_size = value_size;
  std::memcpy(target->value_bytes, value, value_size);
  target->observed_epoch = cur_bucket_epoch;
}

void tls_evict(TlsCache *t, uint64_t key) {
  if (!t || !t->entries || key == kTlsKeyEmpty) return;
  uint32_t h = tls_bucket_idx(t, key);
  for (int probe = 0; probe < kTlsProbeMax; probe++) {
    uint32_t i = (h + probe) & t->mask;
    TlsCacheEntry *e = &t->entries[i];
    if (e->key == kTlsKeyEmpty) return;  // not present
    if (e->key == key) {
      e->key = kTlsKeyEmpty;
      return;
    }
  }
}

void tls_cache_dump(const TlsCache *t, int worker_id) {
  if (!t || !t->entries) return;
  uint64_t total = t->hits + t->misses;
  double hit_pct = total ? 100.0 * t->hits / total : 0.0;
  fprintf(stderr,
          "[A:tls w%d] entries=%u hits=%lu misses=%lu hit_rate=%.1f%% "
          "epoch_evict=%lu replacements=%lu\n",
          worker_id, t->num_entries, t->hits, t->misses, hit_pct,
          t->epoch_evictions, t->replacements);
}

}  // namespace fusee
