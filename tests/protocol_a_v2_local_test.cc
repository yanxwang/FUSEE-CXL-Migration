// iter-4A Phase 6 local test: CxlKvStoreA_v2 insert/update/search/remove
// against a single-host CXL region.

#include "cxl_cache_pool.h"
#include "cxl_directory.h"
#include "cxl_hashtable.h"
#include "cxl_kv_blockpool_freelist.h"
#include "cxl_kv_ops_A_v2.h"
#include "cxl_sharding.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <random>
#include <sys/mman.h>
#include <vector>

using namespace fusee;

int main() {
  const uint32_t B = 1024;
  // Allocate CXL bucket array via plain shared mmap (test doesn't need
  // dax — we just want CxlKvStoreA_v2's CoW publish + cache+directory
  // logic exercised on real shared memory).
  std::size_t bucket_bytes = sizeof(CxlKvBucket) * B;
  void *bucket_mem = mmap(nullptr, bucket_bytes, PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (bucket_mem == MAP_FAILED) return 1;

  // ShardingTable for H=1 (everything is owner-self).
  ShardingTable st;
  if (sharding_init(&st, 1) != 0) return 1;

  // SlotDirectory.
  void *dir_mem = mmap(nullptr, slot_directory_bytes(B, kCxlKvSlotsPerBucket),
                       PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (dir_mem == MAP_FAILED) return 1;
  SlotDirectory dir;
  if (slot_directory_init(&dir, dir_mem, B, kCxlKvSlotsPerBucket) != 0) return 1;

  // KvCachePool.
  void *cache_mem = mmap(nullptr, cache_pool_bytes(B),
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (cache_mem == MAP_FAILED) return 1;
  KvCachePool cache;
  if (cache_pool_init(&cache, cache_mem, B) != 0) return 1;

  // BlockFreeList.
  BlockFreeList fl;
  block_freelist_init(&fl);

  // Attach store.
  CxlKvStoreA_v2 store;
  if (store.attach(bucket_mem, B, /*host_id=*/0, /*num_hosts=*/1,
                   /*init_region=*/true, &st, &dir, &cache, &fl) != 0) {
    return 1;
  }

  // Test: insert N keys, search them, update half, search again, remove half.
  const uint32_t N = 1000;
  std::mt19937_64 rng(0xCAFEBABE);
  std::vector<uint64_t> keys;
  std::vector<uint64_t> vals;
  for (uint32_t i = 0; i < N; i++) {
    uint64_t k = rng() | 1ULL;  // avoid 0
    uint64_t v = rng();
    keys.push_back(k); vals.push_back(v);
    int rc = store.insert(k, v);
    if (rc != 0) {
      // Bucket may be full -> retry with another key
      if (rc == -3 || rc == -2) { i--; keys.pop_back(); vals.pop_back(); continue; }
      fprintf(stderr, "FAIL: insert(%lu) rc=%d\n", k, rc); return 1;
    }
  }

  // Search all
  for (uint32_t i = 0; i < keys.size(); i++) {
    uint64_t got;
    if (store.search(keys[i], &got) != 0 || got != vals[i]) {
      fprintf(stderr, "FAIL: search(%lu) miss/mismatch\n", keys[i]);
      return 1;
    }
  }

  // Update half
  for (uint32_t i = 0; i < keys.size(); i += 2) {
    vals[i] = ~vals[i];
    if (store.update(keys[i], vals[i]) != 0) {
      fprintf(stderr, "FAIL: update(%lu)\n", keys[i]); return 1;
    }
  }

  // Search again
  for (uint32_t i = 0; i < keys.size(); i++) {
    uint64_t got;
    if (store.search(keys[i], &got) != 0 || got != vals[i]) {
      fprintf(stderr, "FAIL: post-update search(%lu) got=%lx exp=%lx\n",
              keys[i], got, vals[i]);
      return 1;
    }
  }

  // Remove half
  for (uint32_t i = 1; i < keys.size(); i += 2) {
    if (store.remove(keys[i]) != 0) {
      fprintf(stderr, "FAIL: remove(%lu)\n", keys[i]); return 1;
    }
  }
  for (uint32_t i = 1; i < keys.size(); i += 2) {
    uint64_t got;
    if (store.search(keys[i], &got) == 0) {
      fprintf(stderr, "FAIL: removed key %lu still found\n", keys[i]);
      return 1;
    }
  }
  // Even-index keys still present
  for (uint32_t i = 0; i < keys.size(); i += 2) {
    uint64_t got;
    if (store.search(keys[i], &got) != 0 || got != vals[i]) {
      fprintf(stderr, "FAIL: post-remove search(%lu)\n", keys[i]);
      return 1;
    }
  }

  printf("ALL PASS — %u keys insert+search+update+remove on CxlKvStoreA_v2\n",
         (uint32_t)keys.size());

  cache_pool_destroy(&cache);
  slot_directory_destroy(&dir);
  block_freelist_destroy(&fl);
  munmap(bucket_mem, bucket_bytes);
  munmap(dir_mem, slot_directory_bytes(B, kCxlKvSlotsPerBucket));
  munmap(cache_mem, cache_pool_bytes(B));
  return 0;
}
