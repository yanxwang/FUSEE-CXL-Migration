// Verification + perf micro-check for Option C's DRAM bucket cache.
//
// 1. Single process, cache enabled: after insert(K, V), search(K) returns V
//    (cache miss first time, fills the cache; second search is a cache hit).
// 2. Two processes: host 0 inserts/updates; host 1 searches with cache
//    enabled. Cross-read must still see every host 0 write — the CXL
//    write_epoch bump invalidates host 1's cache correctly.
// 3. Micro-timing: N searches of a hot key, with and without cache. Cache
//    should be meaningfully faster (fewer CXL flushes).

#include "cxl_kv_ops_C.h"
#include "cxl_mm.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" {
#include "common.h"
}

using fusee::CxlKvStoreC;
using fusee::CXLRegion;
using fusee::cxl_region_destroy;
using fusee::cxl_region_init;

namespace {
struct Barrier { cacheline_u64 phase[2]; };
constexpr size_t kBarrierOffsetFromEnd = 4096;

void barrier_wait(Barrier *bar, int me, int other, uint64_t phase) {
  CACHELINE_STORE(&bar->phase[me], phase);
  for (;;) {
    if (CACHELINE_LOAD(&bar->phase[other]) >= phase) return;
    usleep(200);
  }
}

uint64_t now_ns() {
  timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
} // namespace

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <dev_path>\n", argv[0]); return 2; }
  const char *dev = argv[1];

  constexpr uint32_t kNumBuckets = 1024;
  constexpr int kNumHosts = 2;
  size_t store_bytes = CxlKvStoreC::bytes_for(kNumBuckets);
  size_t needed = ((store_bytes + kBarrierOffsetFromEnd + fusee::kCxlDevdaxAlign - 1) /
                   fusee::kCxlDevdaxAlign) *
                  fusee::kCxlDevdaxAlign;

  pid_t pid = fork();
  if (pid < 0) { perror("fork"); return 1; }

  CXLRegion r{};
  if (cxl_region_init(&r, dev, needed) < 0) return 1;

  int host_id = (pid == 0) ? 1 : 0;
  bool is_host0 = (host_id == 0);
  int other = 1 - host_id;
  auto *bar = reinterpret_cast<Barrier *>(
      reinterpret_cast<char *>(r.base) + r.size - kBarrierOffsetFromEnd);
  if (is_host0) {
    CACHELINE_STORE(&bar->phase[0], 0ULL);
    CACHELINE_STORE(&bar->phase[1], 0ULL);
  }

  CxlKvStoreC store;
  if (store.attach(r.base, r.size - kBarrierOffsetFromEnd, kNumBuckets,
                   host_id, kNumHosts, is_host0) != 0) {
    cxl_region_destroy(&r);
    if (!is_host0) _exit(1);
    return 1;
  }
  store.enable_dram_cache(true);

  barrier_wait(bar, host_id, other, 1);

  int fail = 0;

  if (is_host0) {
    // Host 0: insert a range and update it once.
    for (int i = 0; i < 200; i++) {
      if (store.insert(1000 + i, 42000 + i) != 0) { fprintf(stderr, "FAIL: insert %d\n", i); fail++; }
    }
  }
  barrier_wait(bar, host_id, other, 2);

  if (!is_host0) {
    // Host 1: cross-read with cache enabled. First reads miss, fill cache;
    // second reads should hit.
    for (int i = 0; i < 200; i++) {
      uint64_t got = 0;
      if (store.search(1000 + i, &got) != 0 || got != (uint64_t)(42000 + i)) {
        fprintf(stderr, "FAIL: search %d got %lu\n", i, got);
        fail++;
      }
    }
    // Second pass should be cache hit and still correct.
    for (int i = 0; i < 200; i++) {
      uint64_t got = 0;
      if (store.search(1000 + i, &got) != 0 || got != (uint64_t)(42000 + i)) {
        fprintf(stderr, "FAIL: cached search %d got %lu\n", i, got);
        fail++;
      }
    }
  }
  barrier_wait(bar, host_id, other, 3);

  // Host 0 updates: must invalidate host 1's cache via CXL epoch.
  if (is_host0) {
    for (int i = 0; i < 200; i++) {
      if (store.update(1000 + i, 99000 + i) != 0) { fprintf(stderr, "FAIL: update\n"); fail++; }
    }
  }
  barrier_wait(bar, host_id, other, 4);

  if (!is_host0) {
    uint64_t stale = 0;
    for (int i = 0; i < 200; i++) {
      uint64_t got = 0;
      if (store.search(1000 + i, &got) != 0) { fprintf(stderr, "FAIL: miss after update\n"); fail++; continue; }
      if (got == (uint64_t)(42000 + i)) stale++;
      else if (got != (uint64_t)(99000 + i)) { fprintf(stderr, "FAIL: bad val %lu\n", got); fail++; }
    }
    if (stale > 0) {
      fprintf(stderr, "FAIL: cache served %lu stale reads after peer update\n", stale);
      fail++;
    }
  }
  barrier_wait(bar, host_id, other, 5);

  // Micro-timing on host 0 only.
  if (is_host0) {
    const uint64_t iters = 50000;
    uint64_t sink = 0;

    // No cache path — reuse a fresh store instance with cache off.
    CxlKvStoreC cold;
    // Fresh attach is not possible here (region already init'd), so just
    // disable cache on the existing store and measure; then enable and
    // remeasure.
    store.enable_dram_cache(false);
    uint64_t t0 = now_ns();
    for (uint64_t i = 0; i < iters; i++) {
      uint64_t out = 0;
      store.search(1000 + (i % 200), &out);
      sink ^= out;
    }
    uint64_t t_nocache = now_ns() - t0;

    store.enable_dram_cache(true);
    // Warm the cache.
    for (int i = 0; i < 200; i++) {
      uint64_t out = 0;
      store.search(1000 + i, &out);
      sink ^= out;
    }
    t0 = now_ns();
    for (uint64_t i = 0; i < iters; i++) {
      uint64_t out = 0;
      store.search(1000 + (i % 200), &out);
      sink ^= out;
    }
    uint64_t t_cache = now_ns() - t0;

    printf("[host 0] search x %lu: no_cache=%lu ns/op, cache=%lu ns/op, speedup=%.2fx\n",
           iters, t_nocache / iters, t_cache / iters,
           (double)t_nocache / (double)t_cache);
    printf("[host 0] sink=%lx (prevents opt-out)\n", sink);
  }

  if (!is_host0) {
    cxl_region_destroy(&r);
    _exit(fail == 0 ? 0 : 1);
  }
  int status = 0; waitpid(pid, &status, 0);
  int child_rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
  cxl_region_destroy(&r);
  if (fail == 0 && child_rc == 0) {
    printf("OK: DRAM cache correctness + perf on %s\n", dev);
    return 0;
  }
  fprintf(stderr, "FAIL: host0=%d child=%d\n", fail, child_rc);
  return 1;
}
