// Cross-host atomic fetch_add test. One counter on CXL; each host fork-spawns
// N workers and each worker does `iters` fetch_add'1 on the counter. At the
// end, host 0 reads the final value and checks it matches 2 × N × iters.
//
// If x86 LOCK prefix on CXL-backed memory without cache coherence actually
// works across hosts, final will match. If not, final < expected.
//
// Usage:
//   ssh g3: FUSEE_HOST_ID=0 FUSEE_RUN_COOKIE=<c> ./cxl_atomic_xhost /dev/dax0.0 <iters> <N>
//   ssh g4: FUSEE_HOST_ID=1 FUSEE_RUN_COOKIE=<c> ./cxl_atomic_xhost /dev/dax0.0 <iters> <N>

#include "cxl_mm.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
  if (argc < 4) {
    fprintf(stderr, "usage: %s <dev> <iters> <num_fork>\n", argv[0]);
    return 2;
  }
  const char *dev = argv[1];
  uint64_t iters = strtoull(argv[2], nullptr, 0);
  int N = atoi(argv[3]);
  int host_id = getenv("FUSEE_HOST_ID") ? atoi(getenv("FUSEE_HOST_ID")) : 0;
  uint64_t cookie = getenv("FUSEE_RUN_COOKIE") ?
                      strtoull(getenv("FUSEE_RUN_COOKIE"), nullptr, 0) : 0;

  const size_t sz = 2 * 1024 * 1024;
  CXLRegion r{};
  if (cxl_region_init(&r, dev, sz) < 0) {
    fprintf(stderr, "cxl_region_init failed\n"); return 1;
  }
  auto *counter = reinterpret_cast<std::atomic<uint64_t> *>(r.base);
  auto *cookie_slot = reinterpret_cast<cacheline_u64 *>(
      reinterpret_cast<char *>(r.base) + 64);
  auto *done_host0 = reinterpret_cast<cacheline_u64 *>(
      reinterpret_cast<char *>(r.base) + 128);
  auto *done_host1 = reinterpret_cast<cacheline_u64 *>(
      reinterpret_cast<char *>(r.base) + 192);

  // Host 0 resets counter. Host 1 waits for cookie.
  if (host_id == 0) {
    counter->store(0, std::memory_order_relaxed);
    CACHELINE_STORE(done_host0, 0ULL);
    CACHELINE_STORE(done_host1, 0ULL);
    flush_region(r.base, 256);
    store_fence();
    CACHELINE_STORE(cookie_slot, cookie);
  } else {
    while (CACHELINE_LOAD(cookie_slot) != cookie) __builtin_ia32_pause();
  }

  // Fork N children.
  std::vector<pid_t> children;
  int child_id = 0;
  for (int i = 1; i < N; i++) {
    pid_t p = fork();
    if (p < 0) { perror("fork"); return 1; }
    if (p == 0) { child_id = i; children.clear(); break; }
    children.push_back(p);
  }

  // All workers: fetch_add with explicit flushes.
  for (uint64_t i = 0; i < iters; i++) {
    flush_line(counter);
    full_fence();
    counter->fetch_add(1, std::memory_order_acq_rel);
    flush_line(counter);
    store_fence();
  }

  // Reap children.
  if (child_id != 0) _exit(0);
  for (pid_t p : children) { int st; waitpid(p, &st, 0); }

  // Signal done.
  auto *my_done = (host_id == 0) ? done_host0 : done_host1;
  auto *peer_done = (host_id == 0) ? done_host1 : done_host0;
  CACHELINE_STORE(my_done, 1ULL);

  // Host 0 waits for host 1's done, then reads final counter.
  if (host_id == 0) {
    while (CACHELINE_LOAD(peer_done) == 0) __builtin_ia32_pause();
    flush_line(counter);
    full_fence();
    uint64_t final = counter->load(std::memory_order_acquire);
    uint64_t expected = 2 * N * iters;
    printf("FINAL counter=%lu expected=%lu (2 hosts × %d fork × %lu iters) %s\n",
           final, expected, N, iters,
           final == expected ? "OK" : "MISMATCH");
  }

  cxl_region_destroy(&r);
  return 0;
}
