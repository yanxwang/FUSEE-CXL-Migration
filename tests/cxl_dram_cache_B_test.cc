// Option B DRAM cache with ring-driven invalidation — correctness + perf.
//
// Critical difference from the C version: the B reader fast path does NOT
// load from CXL at all. Invalidation reaches host 1's cache only via host 0's
// ring push being consumed by host 1's replicator thread.

#include "cxl_kv_ops_B.h"
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

using fusee::CxlKvStoreB;
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
  if (argc < 2) { fprintf(stderr, "usage: %s <dev>\n", argv[0]); return 2; }
  const char *dev = argv[1];

  constexpr uint32_t kNumBuckets = 1024;
  constexpr int kNumHosts = 2;
  size_t store_bytes = CxlKvStoreB::bytes_for(kNumBuckets);
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
  if (is_host0) { CACHELINE_STORE(&bar->phase[0], 0ULL); CACHELINE_STORE(&bar->phase[1], 0ULL); }

  CxlKvStoreB store;
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
    for (int i = 0; i < 200; i++) {
      if (store.insert(1000 + i, 42000 + i) != 0) fail++;
    }
  }
  barrier_wait(bar, host_id, other, 2);

  if (!is_host0) {
    for (int i = 0; i < 200; i++) {
      uint64_t got = 0;
      if (store.search(1000 + i, &got) != 0 || got != (uint64_t)(42000 + i)) {
        fprintf(stderr, "FAIL: initial search %d got %lu\n", i, got);
        fail++;
      }
    }
    // Cache hits.
    for (int i = 0; i < 200; i++) {
      uint64_t got = 0;
      if (store.search(1000 + i, &got) != 0 || got != (uint64_t)(42000 + i)) {
        fprintf(stderr, "FAIL: cached search\n"); fail++;
      }
    }
  }
  barrier_wait(bar, host_id, other, 3);

  if (is_host0) {
    for (int i = 0; i < 200; i++) (void)store.update(1000 + i, 99000 + i);
  }
  barrier_wait(bar, host_id, other, 4);

  // Give host 1's replicator a moment to drain invalidations.
  if (!is_host0) usleep(100 * 1000);

  if (!is_host0) {
    uint64_t stale = 0, new_cnt = 0, miss = 0;
    for (int i = 0; i < 200; i++) {
      uint64_t got = 0;
      if (store.search(1000 + i, &got) != 0) { miss++; continue; }
      if (got == (uint64_t)(42000 + i)) stale++;
      else if (got == (uint64_t)(99000 + i)) new_cnt++;
    }
    printf("[host 1] post-update: new=%lu stale=%lu miss=%lu\n",
           new_cnt, stale, miss);
    if (stale > 0) { fprintf(stderr, "FAIL: B cache served %lu stale\n", stale); fail++; }
  }
  barrier_wait(bar, host_id, other, 5);

  if (is_host0) {
    const uint64_t iters = 50000;
    uint64_t sink = 0;
    store.enable_dram_cache(false);
    uint64_t t0 = now_ns();
    for (uint64_t i = 0; i < iters; i++) {
      uint64_t out = 0; store.search(1000 + (i % 200), &out); sink ^= out;
    }
    uint64_t no_c = now_ns() - t0;

    store.enable_dram_cache(true);
    for (int i = 0; i < 200; i++) { uint64_t out; store.search(1000 + i, &out); sink ^= out; }
    t0 = now_ns();
    for (uint64_t i = 0; i < iters; i++) {
      uint64_t out = 0; store.search(1000 + (i % 200), &out); sink ^= out;
    }
    uint64_t ca = now_ns() - t0;
    printf("[host 0] B search: no_cache=%lu ns/op, cache=%lu ns/op, speedup=%.2fx\n",
           no_c/iters, ca/iters, (double)no_c/(double)ca);
    printf("[host 0] sink=%lx\n", sink);
  }

  store.stop();
  if (!is_host0) { cxl_region_destroy(&r); _exit(fail == 0 ? 0 : 1); }
  int status = 0; waitpid(pid, &status, 0);
  int child_rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
  cxl_region_destroy(&r);
  if (fail == 0 && child_rc == 0) {
    printf("OK: Option B DRAM cache + ring-driven invalidation on %s\n", dev);
    return 0;
  }
  return 1;
}
