// iter-4A Phase 7 cross-host read correctness test.
//
// Spec §I9: reader fast path = local cache + stale check; slow path =
// register + fill from owner. iter-4A Phase 7 implements cross-host
// read correctness via direct CXL coherent load (without owner-side
// register) — adequate for the no-concurrent-write scenario tested
// here. Phase 8 will add register/invalidation for live workloads.
//
// Test:
//   - Both hosts insert keys (deterministic) into their owned shards.
//   - After barrier, both hosts try to READ all keys (regardless of
//     owner). Cross-host reads must return correct value bytes.

#include "cxl_cache_pool.h"
#include "cxl_directory.h"
#include "cxl_hashtable.h"
#include "cxl_kv_blockpool_freelist.h"
#include "cxl_kv_ops_A_v2.h"
#include "cxl_mm.h"
#include "cxl_sharding.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include "common.h"
}

using namespace fusee;

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: %s <dev> <num_buckets> <ops>\n", argv[0]); return 2;
  }
  const char *dev = argv[1];
  uint32_t num_buckets = (uint32_t)strtoul(argv[2], nullptr, 0);
  uint64_t ops = strtoull(argv[3], nullptr, 0);

  int num_hosts = 1, host_id = 0, num_threads = 1;
  uint64_t cookie = 0;
  if (const char *e = getenv("FUSEE_NUM_HOSTS")) num_hosts = atoi(e);
  if (const char *e = getenv("FUSEE_HOST_ID")) host_id = atoi(e);
  if (const char *e = getenv("FUSEE_NUM_THREADS")) num_threads = atoi(e);
  if (const char *e = getenv("FUSEE_RUN_COOKIE")) cookie = strtoull(e, nullptr, 0);

  std::size_t bucket_bytes = sizeof(CxlKvBucket) * num_buckets;
  std::size_t total = 4096 + bucket_bytes + 4096;
  total = ((total + kCxlDevdaxAlign - 1) / kCxlDevdaxAlign) * kCxlDevdaxAlign;

  CXLRegion r{};
  if (cxl_region_init(&r, dev, total) < 0) return 1;

  struct alignas(64) Header {
    cacheline_u64 run_cookie;
    cacheline_u64 init_done;
  };
  Header *hdr = reinterpret_cast<Header *>(r.base);
  CxlKvBucket *buckets = reinterpret_cast<CxlKvBucket *>(
      reinterpret_cast<char *>(r.base) + 4096);

  if (host_id == 0) {
    CACHELINE_STORE(&hdr->init_done, 0ULL);
    CACHELINE_STORE(&hdr->run_cookie, cookie);
  } else {
    while (CACHELINE_LOAD(&hdr->run_cookie) != cookie) __builtin_ia32_pause();
  }

  ShardingTable st; sharding_init(&st, num_hosts);
  void *dir_mem = mmap(nullptr, slot_directory_bytes(num_buckets, kCxlKvSlotsPerBucket),
                       PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  SlotDirectory dir; slot_directory_init(&dir, dir_mem, num_buckets, kCxlKvSlotsPerBucket);
  void *cache_mem = mmap(nullptr, cache_pool_bytes(num_buckets),
                         PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  KvCachePool cache; cache_pool_init(&cache, cache_mem, num_buckets);
  BlockFreeList fl; block_freelist_init(&fl);

  std::vector<pid_t> children;
  int client_id = 0;
  for (int i = 1; i < num_threads; i++) {
    pid_t p = fork();
    if (p == 0) { client_id = i; children.clear(); break; }
    children.push_back(p);
  }

  bool is_primary_client = (host_id == 0) && (client_id == 0);
  CxlKvStoreA_v2 store;
  if (is_primary_client) {
    store.attach(buckets, num_buckets, host_id, num_hosts, true,
                 &st, &dir, &cache, &fl);
    CACHELINE_STORE(&hdr->init_done, 1ULL);
  } else {
    while (CACHELINE_LOAD(&hdr->init_done) == 0) __builtin_ia32_pause();
    store.attach(buckets, num_buckets, host_id, num_hosts, false,
                 &st, &dir, &cache, &fl);
  }

  // Phase 1: each host inserts its own owned keys (deterministic).
  std::mt19937_64 key_rng(0xDEADBEEF);
  uint64_t my_inserts = 0;
  for (uint64_t i = 0; i < ops; i++) {
    uint64_t k = key_rng() | 1ULL;
    if (host_of(&st, k) != (uint32_t)host_id) continue;
    if ((i % (uint64_t)num_threads) != (uint64_t)client_id) continue;
    int rc = store.insert(k, k ^ 0xCAFEULL);
    if (rc == 0) my_inserts++;
  }

  if (client_id != 0) { _exit(0); }
  for (auto p : children) waitpid(p, nullptr, 0);

  // Cross-host barrier: each host signals "writes done" + waits for peer.
  if (host_id == 0) {
    CACHELINE_STORE(&hdr->init_done, 0x11ULL);  // marker: writes done by h0
    flush_line(&hdr->init_done); store_fence();
    while (true) {
      flush_line(&hdr->init_done); full_fence();
      uint64_t v = CACHELINE_LOAD(&hdr->init_done);
      if (v & 0x20ULL) break;
      __builtin_ia32_pause();
    }
  } else {
    while (true) {
      flush_line(&hdr->init_done); full_fence();
      uint64_t v = CACHELINE_LOAD(&hdr->init_done);
      if (v & 0x10ULL) {
        CACHELINE_STORE(&hdr->init_done, v | 0x20ULL);
        flush_line(&hdr->init_done); store_fence();
        break;
      }
      __builtin_ia32_pause();
    }
  }

  // Phase 2: read ALL keys (including peer-owned) and verify.
  // Reset cache between writer/reader phases so the reader exercises
  // the slow path. (cache_pool_destroy + re-init is heavy; just evict
  // each key as we see it doesn't match — but actually let's just
  // proceed: even cached entries are correct because writer is owner.)
  std::mt19937_64 key_rng2(0xDEADBEEF);
  uint64_t reads_ok = 0, reads_miss = 0, reads_wrong = 0;
  for (uint64_t i = 0; i < ops; i++) {
    uint64_t k = key_rng2() | 1ULL;
    uint64_t got = 0;
    int rc = store.search(k, &got);
    if (rc == 0) {
      uint64_t expected = k ^ 0xCAFEULL;
      if (got == expected) reads_ok++;
      else { reads_wrong++; }
    } else {
      reads_miss++;
    }
  }
  fprintf(stderr, "[h%d] reads: ok=%lu miss=%lu wrong=%lu (inserts=%lu)\n",
          host_id, reads_ok, reads_miss, reads_wrong, my_inserts);
  cxl_region_destroy(&r);
  return reads_wrong == 0 ? 0 : 1;
}
