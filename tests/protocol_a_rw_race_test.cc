// iter-5A Phase 4: §I9 strict-A linearizability gate (G6).
//
// Setup: host 0 worker continuously updates key K with a monotonically
// increasing counter (1, 2, 3, ...). Host 1 worker continuously reads
// K and asserts that successive read values never decrease (i.e., no
// stale cache hit returns an old value AFTER a newer one was already
// observed).
//
// Without P2 fix (iter-4A-redo): host 1 caches K=1 from initial read,
// host 0 updates to K=2 with no invalidate broadcast → host 1 re-reads,
// gets K=1 from stale cache → monotonicity violated.
//
// With P2 fix (iter-5A InvalRing+dispatcher): host 0's UPDATE broadcasts
// OP_INVALIDATE to host 1, dispatcher marks K stale, host 1's next read
// triggers OP_CACHE_REGISTER → fresh value. Strict-A holds.

#include "cxl_cache_pool.h"
#include "cxl_directory.h"
#include "cxl_forward_ring.h"
#include "cxl_hashtable.h"
#include "cxl_inval_ring.h"
#include "cxl_kv_blockpool.h"
#include "cxl_kv_blockpool_freelist.h"
#include "cxl_kv_ops_A.h"
#include "cxl_mm.h"
#include "cxl_sharding.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" {
#include "common.h"
}

using namespace fusee;

static uint64_t now_ns() {
  timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(int argc, char **argv) {
  const uint32_t kNumBuckets = 65536;
  const uint64_t kIters = (argc > 1) ? strtoull(argv[1], nullptr, 0) : 100000;
  const char *dev = (argc > 2) ? argv[2] : "/dev/dax0.0";

  int num_hosts = 2, host_id = 0;
  uint64_t cookie = 0;
  if (const char *e = getenv("FUSEE_NUM_HOSTS")) num_hosts = atoi(e);
  if (const char *e = getenv("FUSEE_HOST_ID")) host_id = atoi(e);
  if (const char *e = getenv("FUSEE_RUN_COOKIE")) cookie = strtoull(e, nullptr, 0);

  // CXL region.
  std::size_t bucket_bytes = sizeof(CxlKvBucket) * kNumBuckets;
  const uint32_t kBlockSize = 256;
  const uint32_t kBlocksPerHost = 1024;
  std::size_t pool_bytes =
      CxlKvBlockPool::bytes_for(kBlocksPerHost, kBlockSize, num_hosts);
  std::size_t fr_bytes = forward_ring_matrix_bytes();
  std::size_t ir_bytes = inval_ring_matrix_bytes();
  std::size_t header_bytes = 4096;
  std::size_t total = header_bytes + bucket_bytes + pool_bytes + fr_bytes
                    + ir_bytes + 4096;
  total = ((total + kCxlDevdaxAlign - 1) / kCxlDevdaxAlign) * kCxlDevdaxAlign;

  CXLRegion r{};
  if (cxl_region_init(&r, dev, total) < 0) { fprintf(stderr, "init fail\n"); return 1; }

  struct alignas(64) Header {
    cacheline_u64 run_cookie;
    cacheline_u64 init_done;
  };
  Header *hdr = reinterpret_cast<Header *>(r.base);
  CxlKvBucket *buckets = reinterpret_cast<CxlKvBucket *>(
      reinterpret_cast<char *>(r.base) + header_bytes);
  void *pool_mem = reinterpret_cast<char *>(buckets) + bucket_bytes;
  void *fr_mem = reinterpret_cast<char *>(pool_mem) + pool_bytes;
  void *ir_mem = reinterpret_cast<char *>(fr_mem) + fr_bytes;

  bool is_host_primary = (host_id == 0);
  if (is_host_primary) {
    CACHELINE_STORE(&hdr->init_done, 0ULL);
    CACHELINE_STORE(&hdr->run_cookie, cookie);
    flush_line(hdr); store_fence();
  } else {
    while (true) {
      flush_line(hdr); full_fence();
      if (CACHELINE_LOAD(&hdr->run_cookie) == cookie) break;
      __builtin_ia32_pause();
    }
  }

  // DRAM regions.
  ShardingTable st; sharding_init(&st, (uint32_t)num_hosts);
  void *dir_mem = mmap(nullptr,
                       slot_directory_bytes(kNumBuckets, kCxlKvSlotsPerBucket),
                       PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  SlotDirectory dir;
  slot_directory_init(&dir, dir_mem, kNumBuckets, kCxlKvSlotsPerBucket);
  void *cache_mem = mmap(nullptr, cache_pool_bytes(kNumBuckets),
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  KvCachePool cache; cache_pool_init(&cache, cache_mem, kNumBuckets);
  BlockFreeList fl; block_freelist_init(&fl);
  CxlKvBlockPool pool;
  if (pool.attach(pool_mem, pool_bytes, kBlocksPerHost, kBlockSize,
                  host_id, num_hosts, is_host_primary) != 0) return 1;

  CxlKvStoreA store;
  if (is_host_primary) {
    if (store.attach(buckets, kNumBuckets, host_id, num_hosts, true,
                     &st, &dir, &cache, &fl, &pool) != 0) return 1;
    ForwardRingMatrix *fr = reinterpret_cast<ForwardRingMatrix *>(fr_mem);
    if (store.enable_forward(fr, true, true) != 0) return 1;
    InvalRingMatrix *ir = reinterpret_cast<InvalRingMatrix *>(ir_mem);
    if (store.enable_invalidate(ir, true, true) != 0) return 1;
    uint64_t cur = CACHELINE_LOAD(&hdr->init_done);
    CACHELINE_STORE(&hdr->init_done, cur | 0x1ULL);
    flush_line(&hdr->init_done); store_fence();
  } else {
    while (true) {
      flush_line(&hdr->init_done); full_fence();
      if ((CACHELINE_LOAD(&hdr->init_done) & 0x1ULL) != 0) break;
      __builtin_ia32_pause();
    }
    if (store.attach(buckets, kNumBuckets, host_id, num_hosts, false,
                     &st, &dir, &cache, &fl, &pool) != 0) return 1;
    ForwardRingMatrix *fr = reinterpret_cast<ForwardRingMatrix *>(fr_mem);
    if (store.enable_forward(fr, false, true) != 0) return 1;
    InvalRingMatrix *ir = reinterpret_cast<InvalRingMatrix *>(ir_mem);
    if (store.enable_invalidate(ir, false, true) != 0) return 1;
    uint64_t cur = CACHELINE_LOAD(&hdr->init_done);
    CACHELINE_STORE(&hdr->init_done, cur | 0x2ULL);
    flush_line(&hdr->init_done); store_fence();
  }

  // Both hosts ready barrier.
  if (host_id == 0) {
    while (true) {
      flush_line(&hdr->init_done); full_fence();
      if ((CACHELINE_LOAD(&hdr->init_done) & 0x2ULL) != 0) break;
      __builtin_ia32_pause();
    }
  }

  // Pick a single key K. To make sure it's owner=host 0, brute-force.
  uint64_t K = 0;
  for (uint64_t k = 1; k < 1000; k++) {
    if (host_of(&st, k) == 0) { K = k; break; }
  }
  if (K == 0) { fprintf(stderr, "no host-0-owned key found\n"); return 1; }

  if (host_id == 0) {
    // Writer: insert K=1, then UPDATE K to 2, 3, ... up to kIters.
    if (store.insert(K, 1) != 0) {
      fprintf(stderr, "insert failed\n"); return 1;
    }
    for (uint64_t i = 2; i <= kIters; i++) {
      int rc = store.update(K, i);
      if (rc != 0) {
        fprintf(stderr, "update failed at i=%lu rc=%d\n", i, rc);
        return 1;
      }
    }
    fprintf(stderr, "[h0] wrote %lu values to K\n", kIters);
  } else {
    // Reader: continuously read K, assert monotonic non-decreasing.
    uint64_t prev = 0;
    uint64_t reads = 0, violations = 0, misses = 0;
    uint64_t deadline_ns = now_ns() + 60ULL * 1000 * 1000 * 1000;  // 60 s budget
    // First wait for writer to insert K (so first read succeeds).
    while (now_ns() < deadline_ns) {
      uint64_t v = 0;
      if (store.search(K, &v) == 0) { prev = v; reads++; break; }
      __builtin_ia32_pause();
    }
    if (reads == 0) {
      fprintf(stderr, "[h1] never observed K (timeout)\n"); return 1;
    }
    while (now_ns() < deadline_ns) {
      uint64_t v = 0;
      int rc = store.search(K, &v);
      if (rc != 0) { misses++; continue; }
      reads++;
      if (v < prev) violations++;
      prev = v;
      if (prev >= kIters) break;
    }
    fprintf(stderr,
            "[h1] reads=%lu violations=%lu misses=%lu final_v=%lu (target %lu)\n",
            reads, violations, misses, prev, kIters);
    if (violations > 0) return 1;
    if (prev < kIters / 2) {
      // Reader didn't make enough progress; treat as fail.
      fprintf(stderr, "[h1] insufficient progress\n");
      return 2;
    }
  }
  store.stop_responder();
  store.stop_dispatcher();
  cxl_region_destroy(&r);
  return 0;
}
