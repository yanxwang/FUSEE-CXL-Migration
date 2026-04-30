// iter-4A Phase 6 cross-host correctness test for CxlKvStoreA_v2.
//
// Both hosts run workers that each operate ONLY on their own owned keys
// (sharding routes; non-owner returns -ENOTSUP). Final bucket array on
// CXL must be byte-identical between the two hosts (per spec §IX G1).
//
// Usage:
//   FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=<0|1> FUSEE_RUN_COOKIE=<ns> \
//   FUSEE_NUM_THREADS=<T> FUSEE_FINAL_STATE_DUMP=/tmp/h<id>.bin \
//   ./protocol_a_v2_2host_test <dev_path> <num_buckets> <ops>
//
// One process per host; each forks NUM_THREADS workers; primary client
// (host 0 client 0) initializes the CXL region.

#include "cxl_cache_pool.h"
#include "cxl_directory.h"
#include "cxl_hashtable.h"
#include "cxl_kv_blockpool_freelist.h"
#include "cxl_kv_ops_A_v2.h"
#include "cxl_mm.h"
#include "cxl_sharding.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <random>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include "common.h"
}

using namespace fusee;

static uint64_t now_ns() {
  timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

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
  const char *dump_path = getenv("FUSEE_FINAL_STATE_DUMP");

  // Compute region size.
  std::size_t bucket_bytes = sizeof(CxlKvBucket) * num_buckets;
  // Header (4 KB) + bucket array + 4 KB run_cookie cell at end.
  std::size_t total = 4096 + bucket_bytes + 4096;
  total = ((total + kCxlDevdaxAlign - 1) / kCxlDevdaxAlign) * kCxlDevdaxAlign;

  CXLRegion r{};
  if (cxl_region_init(&r, dev, total) < 0) {
    fprintf(stderr, "cxl_region_init failed\n"); return 1;
  }

  // Layout: [4 KB header (run_cookie + init_done)][bucket array]
  struct alignas(64) Header {
    cacheline_u64 run_cookie;
    cacheline_u64 init_done;
  };
  Header *hdr = reinterpret_cast<Header *>(r.base);
  CxlKvBucket *buckets = reinterpret_cast<CxlKvBucket *>(
      reinterpret_cast<char *>(r.base) + 4096);

  bool is_host_primary = (host_id == 0);
  if (is_host_primary) {
    CACHELINE_STORE(&hdr->init_done, 0ULL);
    CACHELINE_STORE(&hdr->run_cookie, cookie);
  } else {
    while (CACHELINE_LOAD(&hdr->run_cookie) != cookie) __builtin_ia32_pause();
  }

  // DRAM regions for sharding/directory/cache_pool/freelist (per-host,
  // mmap MAP_SHARED so forked workers share within host).
  ShardingTable st;
  sharding_init(&st, (uint32_t)num_hosts);

  void *dir_mem = mmap(nullptr, slot_directory_bytes(num_buckets, kCxlKvSlotsPerBucket),
                       PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  SlotDirectory dir;
  slot_directory_init(&dir, dir_mem, num_buckets, kCxlKvSlotsPerBucket);

  uint32_t cache_buckets = num_buckets;  // 4 entries each
  void *cache_mem = mmap(nullptr, cache_pool_bytes(cache_buckets),
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  KvCachePool cache;
  cache_pool_init(&cache, cache_mem, cache_buckets);

  BlockFreeList fl;
  block_freelist_init(&fl);

  // Fork num_threads-1 children. Parent has client_id=0.
  std::vector<pid_t> children;
  int client_id = 0;
  for (int i = 1; i < num_threads; i++) {
    pid_t p = fork();
    if (p < 0) { perror("fork"); return 1; }
    if (p == 0) { client_id = i; children.clear(); break; }
    children.push_back(p);
  }

  // Primary inits CXL bucket array; others wait on init_done.
  bool is_primary_client = (host_id == 0) && (client_id == 0);
  CxlKvStoreA_v2 store;
  if (is_primary_client) {
    if (store.attach(buckets, num_buckets, host_id, num_hosts,
                     /*init_region=*/true, &st, &dir, &cache, &fl) != 0) {
      return 1;
    }
    CACHELINE_STORE(&hdr->init_done, 1ULL);
  } else {
    while (CACHELINE_LOAD(&hdr->init_done) == 0) __builtin_ia32_pause();
    if (store.attach(buckets, num_buckets, host_id, num_hosts,
                     /*init_region=*/false, &st, &dir, &cache, &fl) != 0) {
      return 1;
    }
  }

  // Workload: each worker iterates through ops; for each op generates
  // a random key. If owner_host(key) != self_host, skip (Phase 6
  // owner-self only). Else do an UPDATE with random value.
  // We use a rep-deterministic seed so both hosts cover the same key
  // universe; sharding routes each key to its owner.
  std::mt19937_64 rng(0xCAFEBABE ^ ((uint64_t)cookie + (uint64_t)host_id) ^
                     (uint64_t)client_id);
  uint64_t my_inserts = 0, my_updates = 0, skipped = 0, errors = 0;

  // First pass: insert keys (each host inserts its own owned keys).
  // Use deterministic key sequence so both hosts cover the same key
  // universe; each host inserts only the keys whose owner is itself.
  // Work is sharded across this host's workers by global op index `i`.
  std::mt19937_64 key_rng(0xDEADBEEF);
  for (uint64_t i = 0; i < ops; i++) {
    uint64_t k = key_rng() | 1ULL;  // avoid 0
    uint32_t owner = host_of(&st, k);
    if (owner != (uint32_t)host_id) { skipped++; continue; }
    if ((i % (uint64_t)num_threads) != (uint64_t)client_id) continue;
    // Deterministic value = key xor 0xCAFE so both hosts agree on
    // value bytes for keys they own (cross-host test bytes parity).
    int rc = store.insert(k, k ^ 0xCAFEULL);
    if (rc == 0) my_inserts++;
    else if (rc == -2) my_updates++;  // already exists -> count as no-op
    else errors++;
  }

  // Second pass: update half the inserted keys (deterministic).
  std::mt19937_64 key_rng2(0xDEADBEEF);
  for (uint64_t i = 0; i < ops / 2; i++) {
    uint64_t k = key_rng2() | 1ULL;
    uint32_t owner = host_of(&st, k);
    if (owner != (uint32_t)host_id) continue;
    if ((i % (uint64_t)num_threads) != (uint64_t)client_id) continue;
    int rc = store.update(k, k ^ 0xBEEFULL);
    if (rc == 0) my_updates++;
    else errors++;
  }

  fprintf(stderr, "[h%d c%d] inserts=%lu updates=%lu skipped=%lu errors=%lu\n",
          host_id, client_id, my_inserts, my_updates, skipped, errors);

  // Per-host children exit; per-host primary (client 0) waits + dumps.
  if (client_id != 0) {
    _exit(errors == 0 ? 0 : 1);
  }
  for (auto p : children) {
    int st;
    waitpid(p, &st, 0);
    if (WEXITSTATUS(st) != 0) errors++;
  }

  // Cross-host barrier: each host signals "done" and waits for peer.
  // Use Header.init_done as a 2-bit "host_id done" flag.
  if (host_id == 0) {
    CACHELINE_STORE(&hdr->init_done, 0x1ULL);
    flush_line(&hdr->init_done);
    store_fence();
    while ((CACHELINE_LOAD(&hdr->init_done) & 0x2ULL) == 0) {
      flush_line(&hdr->init_done);
      full_fence();
      __builtin_ia32_pause();
    }
  } else {
    // host 1: wait for host 0 then mark.
    while ((CACHELINE_LOAD(&hdr->init_done) & 0x1ULL) == 0) {
      flush_line(&hdr->init_done);
      full_fence();
      __builtin_ia32_pause();
    }
    uint64_t cur = CACHELINE_LOAD(&hdr->init_done);
    CACHELINE_STORE(&hdr->init_done, cur | 0x2ULL);
    flush_line(&hdr->init_done);
    store_fence();
  }

  // Dump bucket array bytes for cross-host hash-diff comparison.
  // BOTH hosts dump (each host's per-host primary_client), so we have
  // h0.bin and h1.bin to cmp.
  bool is_host_primary_client = (client_id == 0);
  if (dump_path && is_host_primary_client) {
    fprintf(stderr, "[h%d c%d] opening dump at %s\n", host_id, client_id, dump_path);
    int fd = open(dump_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror(dump_path); return 1; }
    // Read each bucket from CXL (clflushopt + mfence) before dumping.
    for (uint32_t b = 0; b < num_buckets; b++) {
      flush_line(&buckets[b]);
      flush_line((char *)&buckets[b] + 64);
    }
    full_fence();
    // Write a 16 B header so cmp -i 16 in the harness works (matches
    // iter3A_hash_diff_battery.sh skip).
    char header[16] = {'F','U','S','D','I','F','F','A',
                       (char)host_id, 0, 0, 0,
                       (char)(num_buckets & 0xff),
                       (char)((num_buckets >> 8) & 0xff),
                       (char)((num_buckets >> 16) & 0xff),
                       (char)((num_buckets >> 24) & 0xff)};
    if (write(fd, header, 16) != 16) { perror("write hdr"); }
    if ((std::size_t)write(fd, buckets, bucket_bytes) != bucket_bytes) {
      perror("write");
    }
    close(fd);
    fprintf(stderr, "[h%d c%d] dumped %zu bytes -> %s\n",
            host_id, client_id, bucket_bytes, dump_path);
  }

  cxl_region_destroy(&r);
  return (errors == 0) ? 0 : 1;
}
