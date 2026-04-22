// Microbench: ns/op for atomic fetch_add on a single CXL cacheline.
// Purpose: confirm whether a ticket lock built on CXL atomics is cheap
// enough vs. LFM's spin-retry tail. Per Phase 2 of
// docs/ABC_throughput_improvement_plan.md, if CXL fetch_add > ~5 us,
// we fall back to MCS.
//
// Usage:
//   ./cxl_atomic_bench <dev> <iters> [num_contenders]

#include "cxl_mm.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include "common.h"
}

using fusee::CXLRegion;
using fusee::cxl_region_destroy;
using fusee::cxl_region_init;

static inline uint64_t now_ns() {
  timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <dev> <iters_per_proc> [num_contenders]\n",
            argv[0]);
    return 2;
  }
  const char *dev = argv[1];
  uint64_t iters = strtoull(argv[2], nullptr, 0);
  int N = (argc >= 4) ? atoi(argv[3]) : 1;
  if (N < 1 || N > 128) { fprintf(stderr, "N 1..128\n"); return 2; }

  const size_t sz = 2 * 1024 * 1024;  // 2 MiB dax-aligned
  CXLRegion r{};
  if (cxl_region_init(&r, dev, sz) < 0) {
    fprintf(stderr, "cxl_region_init failed\n"); return 1;
  }

  // Two counters on two SEPARATE cachelines to avoid false sharing.
  auto *counter = reinterpret_cast<std::atomic<uint64_t> *>(r.base);
  auto *start_flag = reinterpret_cast<std::atomic<uint64_t> *>(
      reinterpret_cast<char *>(r.base) + 64);
  counter->store(0, std::memory_order_relaxed);
  start_flag->store(0, std::memory_order_relaxed);
  flush_region(r.base, 128); store_fence();

  std::vector<pid_t> children;
  int id = 0;
  for (int i = 1; i < N; i++) {
    pid_t p = fork();
    if (p < 0) { perror("fork"); return 1; }
    if (p == 0) { id = i; children.clear(); break; }
    children.push_back(p);
  }

  // Barrier: all wait for start_flag == 1.
  if (id == 0) { start_flag->store(1, std::memory_order_release); }
  else         { while (start_flag->load(std::memory_order_acquire) == 0) __builtin_ia32_pause(); }

  // Hot loop: fetch_add on the CXL counter.
  uint64_t t0 = now_ns();
  for (uint64_t i = 0; i < iters; i++) {
    counter->fetch_add(1, std::memory_order_acq_rel);
  }
  uint64_t t1 = now_ns();
  uint64_t ns_per_op = (t1 - t0) / iters;

  printf("PROC id=%d iters=%lu wall=%.3fs ns/op=%lu\n",
         id, iters, (t1 - t0) / 1e9, ns_per_op);

  if (id == 0) {
    for (pid_t p : children) { int st; waitpid(p, &st, 0); }
    uint64_t final = counter->load(std::memory_order_relaxed);
    printf("AGG contenders=%d final=%lu expected=%lu %s\n",
           N, final, iters * N, final == iters * N ? "OK" : "MISMATCH");
  } else {
    _exit(0);
  }
  cxl_region_destroy(&r);
  return 0;
}
