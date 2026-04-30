// iter-4A Phase 4 unit test: per-host blockpool free-list.
//
// Validates spec §I6 (CoW reuse) + §VI (host-local spinlock, NOT LFM).

#include "cxl_kv_blockpool_freelist.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <thread>
#include <vector>

using fusee::BlockFreeList;
using fusee::block_freelist_init;
using fusee::block_freelist_destroy;
using fusee::block_freelist_push;
using fusee::block_freelist_pop;
using fusee::block_freelist_size;

static uint64_t now_ns() {
  timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int test_basic() {
  BlockFreeList fl;
  if (block_freelist_init(&fl) != 0) return 1;

  // Empty pop -> 0.
  if (block_freelist_pop(&fl) != 0) {
    fprintf(stderr, "FAIL: empty pop returned non-zero\n"); return 1;
  }

  // Push then pop -> LIFO order.
  block_freelist_push(&fl, 100);
  block_freelist_push(&fl, 200);
  block_freelist_push(&fl, 300);
  if (block_freelist_size(&fl) != 3) {
    fprintf(stderr, "FAIL: size != 3\n"); return 1;
  }
  if (block_freelist_pop(&fl) != 300) { fprintf(stderr, "FAIL: pop 300\n"); return 1; }
  if (block_freelist_pop(&fl) != 200) { fprintf(stderr, "FAIL: pop 200\n"); return 1; }
  if (block_freelist_pop(&fl) != 100) { fprintf(stderr, "FAIL: pop 100\n"); return 1; }
  if (block_freelist_pop(&fl) != 0)   { fprintf(stderr, "FAIL: extra pop\n"); return 1; }

  block_freelist_destroy(&fl);
  printf("test_basic: PASS\n");
  return 0;
}

// Multi-thread stress: 8 threads × 10k push/pop cycles. Final size 0.
static int test_stress() {
  BlockFreeList fl;
  block_freelist_init(&fl);

  std::vector<std::thread> threads;
  for (int t = 0; t < 8; t++) {
    threads.emplace_back([&fl, t]() {
      for (int i = 0; i < 10000; i++) {
        uint64_t off = (uint64_t)t * 100000 + i;
        block_freelist_push(&fl, off);
        // Random pop (may steal another thread's push)
        block_freelist_pop(&fl);
      }
    });
  }
  for (auto &th : threads) th.join();

  // After 8x10k push/pop pairs, final size 0.
  if (block_freelist_size(&fl) != 0) {
    fprintf(stderr, "FAIL: final size = %zu (expected 0)\n",
            block_freelist_size(&fl));
    return 1;
  }
  block_freelist_destroy(&fl);
  printf("test_stress: PASS\n");
  return 0;
}

static int test_latency() {
  BlockFreeList fl;
  block_freelist_init(&fl);

  // Pre-populate.
  for (int i = 0; i < 1024; i++) block_freelist_push(&fl, (uint64_t)(i + 1) * 64);

  const int M = 1000000;
  uint64_t t0 = now_ns();
  for (int i = 0; i < M; i++) {
    uint64_t off = block_freelist_pop(&fl);
    block_freelist_push(&fl, off);  // round-trip
  }
  uint64_t t1 = now_ns();
  double ns = (double)(t1 - t0) / M;
  printf("freelist push+pop round-trip: %.1f ns/op\n", ns);
  if (ns > 200.0) {
    fprintf(stderr, "WARN: round-trip slower than 200 ns budget (target ~50)\n");
  }

  block_freelist_destroy(&fl);
  return 0;
}

int main() {
  if (test_basic() != 0) return 1;
  if (test_stress() != 0) return 1;
  if (test_latency() != 0) return 1;
  printf("ALL PASS\n");
  return 0;
}
