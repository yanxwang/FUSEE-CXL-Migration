// iter-4A Phase 3 cache pool unit + cross-process test.
//
// Validates spec §I3 (MAP_SHARED across same-host workers), §I4
// (lazy stale flag), AP3 (no per-worker private cache), AP4 (no
// physical delete on invalidate).

#include "cxl_cache_pool.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using fusee::KvCachePool;
using fusee::cache_pool_init;
using fusee::cache_pool_destroy;
using fusee::cache_pool_lookup;
using fusee::cache_pool_insert;
using fusee::cache_pool_set_stale;
using fusee::cache_pool_evict;
using fusee::cache_pool_bytes;
using fusee::kCacheValueMaxBytes;

static uint64_t now_ns() {
  timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Test 1: insert + lookup + stale-as-miss.
static int test_basic() {
  const uint32_t B = 1024;
  void *mem = mmap(nullptr, cache_pool_bytes(B), PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) return 1;

  KvCachePool pool;
  if (cache_pool_init(&pool, mem, B) != 0) return 1;

  uint8_t buf[256];
  uint32_t sz = 0;

  // Lookup miss on empty cache.
  if (cache_pool_lookup(&pool, 42, buf, sizeof(buf), &sz)) {
    fprintf(stderr, "FAIL: lookup hit on empty cache\n"); return 1;
  }

  // Insert + lookup.
  uint8_t v1[8] = {1,2,3,4,5,6,7,8};
  if (cache_pool_insert(&pool, 42, v1, 8) != 0) return 1;
  if (!cache_pool_lookup(&pool, 42, buf, sizeof(buf), &sz)) {
    fprintf(stderr, "FAIL: lookup miss after insert\n"); return 1;
  }
  if (sz != 8 || memcmp(buf, v1, 8) != 0) {
    fprintf(stderr, "FAIL: lookup payload mismatch\n"); return 1;
  }

  // Set stale -> lookup miss but entry physically present.
  cache_pool_set_stale(&pool, 42);
  if (cache_pool_lookup(&pool, 42, buf, sizeof(buf), &sz)) {
    fprintf(stderr, "FAIL: stale entry returned hit\n"); return 1;
  }

  // Re-insert -> stale flag cleared.
  uint8_t v2[16] = {0xa,0xb,0xc,0xd,0xe,0xf,0x10,0x11,0,0,0,0,0,0,0,0};
  if (cache_pool_insert(&pool, 42, v2, 16) != 0) return 1;
  if (!cache_pool_lookup(&pool, 42, buf, sizeof(buf), &sz)) {
    fprintf(stderr, "FAIL: lookup miss after re-insert\n"); return 1;
  }
  if (sz != 16 || memcmp(buf, v2, 16) != 0) {
    fprintf(stderr, "FAIL: re-insert payload mismatch\n"); return 1;
  }

  // Evict -> miss.
  cache_pool_evict(&pool, 42);
  if (cache_pool_lookup(&pool, 42, buf, sizeof(buf), &sz)) {
    fprintf(stderr, "FAIL: evicted entry returned hit\n"); return 1;
  }

  cache_pool_destroy(&pool);
  munmap(mem, cache_pool_bytes(B));
  printf("test_basic: PASS\n");
  return 0;
}

// Test 2: 1M random KV insert + lookup hit rate (before LRU eviction).
static int test_hit_rate() {
  const uint32_t B = 65536;
  void *mem = mmap(nullptr, cache_pool_bytes(B), PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) return 1;

  KvCachePool pool;
  cache_pool_init(&pool, mem, B);

  // Insert N items at ~25% capacity (1 expected entry per bucket).
  // Birthday-paradox collisions still happen but >4-deep collisions are
  // rare → hit rate should be > 95 %.
  const uint32_t N = B;  // 25 % capacity
  std::mt19937_64 rng(0xCAFEBABE);
  std::vector<uint64_t> keys;
  keys.reserve(N);
  for (uint32_t i = 0; i < N; i++) {
    uint64_t k = rng() | 1ULL;  // avoid kCacheKeyEmpty=0
    keys.push_back(k);
    uint8_t v[16];
    for (int j = 0; j < 16; j++) v[j] = (uint8_t)(k >> (j * 4));
    cache_pool_insert(&pool, k, v, 16);
  }

  uint8_t buf[16];
  uint32_t sz;
  uint32_t hits = 0;
  for (uint32_t i = 0; i < N; i++) {
    if (cache_pool_lookup(&pool, keys[i], buf, sizeof(buf), &sz)) hits++;
  }
  double hit_rate = (double)hits / N;
  printf("hit rate after %u inserts (75%% cap): %u/%u = %.3f\n",
         N, hits, N, hit_rate);
  if (hit_rate < 0.95) {
    fprintf(stderr, "FAIL: hit rate < 95%%\n"); return 1;
  }

  cache_pool_destroy(&pool);
  munmap(mem, cache_pool_bytes(B));
  printf("test_hit_rate: PASS\n");
  return 0;
}

// Test 3: cross-process: forked workers share cache via MAP_SHARED.
static int test_fork_share() {
  const uint32_t B = 1024;
  void *mem = mmap(nullptr, cache_pool_bytes(B), PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) return 1;

  KvCachePool pool;
  cache_pool_init(&pool, mem, B);

  // Parent inserts a value, then forks; children should see it.
  uint8_t v[8] = {1,2,3,4,5,6,7,8};
  cache_pool_insert(&pool, 12345, v, 8);

  std::vector<pid_t> kids;
  for (int c = 0; c < 4; c++) {
    pid_t p = fork();
    if (p == 0) {
      uint8_t buf[8]; uint32_t sz;
      if (!cache_pool_lookup(&pool, 12345, buf, sizeof(buf), &sz) ||
          sz != 8 || memcmp(buf, v, 8) != 0) {
        _exit(1);  // child failure
      }
      _exit(0);
    }
    kids.push_back(p);
  }
  for (auto p : kids) {
    int st;
    waitpid(p, &st, 0);
    if (WEXITSTATUS(st) != 0) {
      fprintf(stderr, "FAIL: child saw mismatched value (siblings\
 read parent-inserted entry via MAP_SHARED)\n"); return 1;
    }
  }

  // Parent sets stale; spawn one more child to verify staleness propagates.
  cache_pool_set_stale(&pool, 12345);
  pid_t pverify = fork();
  if (pverify == 0) {
    uint8_t buf[8]; uint32_t sz;
    if (cache_pool_lookup(&pool, 12345, buf, sizeof(buf), &sz)) {
      _exit(1);  // child should not see stale entry as hit
    }
    _exit(0);
  }
  int st; waitpid(pverify, &st, 0);
  if (WEXITSTATUS(st) != 0) {
    fprintf(stderr, "FAIL: child saw stale entry as hit\n"); return 1;
  }

  cache_pool_destroy(&pool);
  munmap(mem, cache_pool_bytes(B));
  printf("test_fork_share: PASS\n");
  return 0;
}

// Test 4: latency.
static int test_latency() {
  const uint32_t B = 65536;
  void *mem = mmap(nullptr, cache_pool_bytes(B), PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) return 1;
  KvCachePool pool;
  cache_pool_init(&pool, mem, B);

  // Pre-populate
  std::mt19937_64 rng(0xDEADBEEF);
  std::vector<uint64_t> keys;
  for (int i = 0; i < 1000; i++) {
    uint64_t k = rng() | 1ULL;
    keys.push_back(k);
    uint8_t v[16] = {0};
    cache_pool_insert(&pool, k, v, 16);
  }

  // Hot loop: cycle through keys (cache hits).
  const int M = 1000000;
  uint8_t buf[16]; uint32_t sz;
  uint64_t t0 = now_ns();
  uint32_t hits = 0;
  for (int i = 0; i < M; i++) {
    if (cache_pool_lookup(&pool, keys[i % 1000], buf, sizeof(buf), &sz)) hits++;
  }
  uint64_t t1 = now_ns();
  double ns = (double)(t1 - t0) / M;
  printf("cache hit fast-path: %.1f ns/op (hits=%u)\n", ns, hits);
  if (ns > 200.0) {
    fprintf(stderr, "WARN: lookup slower than 200 ns (plan budget 100 ns)\n");
  }

  // Stale flag set latency.
  uint64_t t2 = now_ns();
  for (int i = 0; i < M; i++) {
    cache_pool_set_stale(&pool, keys[i % 1000]);
  }
  uint64_t t3 = now_ns();
  double ns2 = (double)(t3 - t2) / M;
  printf("set_stale: %.1f ns/op\n", ns2);

  cache_pool_destroy(&pool);
  munmap(mem, cache_pool_bytes(B));
  return 0;
}

int main() {
  if (test_basic() != 0) return 1;
  if (test_hit_rate() != 0) return 1;
  if (test_fork_share() != 0) return 1;
  if (test_latency() != 0) return 1;
  printf("ALL PASS\n");
  return 0;
}
