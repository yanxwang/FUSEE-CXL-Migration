// iter-4A Phase 1 unit test for sharding table.
//
// Validates spec §I2 (sharding rule) + §XII O1 (hash function default):
// - Same key always maps to same host (deterministic)
// - Uniform key distribution produces ~50/50 split for H=2 within 5 %
// - host_of() latency is small (advisory check)

#include "cxl_sharding.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <random>
#include <vector>

using fusee::ShardingTable;
using fusee::sharding_init;
using fusee::host_of;

static uint64_t now_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main() {
  // Bad-input rejection
  ShardingTable st;
  if (sharding_init(&st, 0) == 0) { fprintf(stderr, "FAIL: H=0 accepted\n"); return 1; }
  if (sharding_init(&st, 3) == 0) { fprintf(stderr, "FAIL: H=3 accepted\n"); return 1; }
  if (sharding_init(&st, 8) == 0) { fprintf(stderr, "FAIL: H=8 accepted\n"); return 1; }

  // Valid configs
  for (uint32_t h : {1u, 2u, 4u}) {
    if (sharding_init(&st, h) != 0) {
      fprintf(stderr, "FAIL: sharding_init(H=%u)\n", h); return 1;
    }
    if (st.num_hosts != h || st.mask != h - 1) {
      fprintf(stderr, "FAIL: H=%u num/mask wrong\n", h); return 1;
    }
  }

  // H=2 distribution test
  sharding_init(&st, 2);
  const uint32_t N = 100000;
  uint32_t counts[2] = {0, 0};
  std::mt19937_64 rng(0xCAFEBABE);
  for (uint32_t i = 0; i < N; i++) {
    uint64_t k = rng();
    uint32_t h = host_of(&st, k);
    if (h >= 2) { fprintf(stderr, "FAIL: host_of out of range\n"); return 1; }
    counts[h]++;
  }
  double frac0 = (double)counts[0] / N;
  printf("H=2 split: host0=%u (%.3f), host1=%u (%.3f)\n",
         counts[0], frac0, counts[1], 1.0 - frac0);
  if (frac0 < 0.45 || frac0 > 0.55) {
    fprintf(stderr, "FAIL: H=2 distribution outside [0.45, 0.55]\n");
    return 1;
  }

  // Determinism test
  for (int i = 0; i < 100; i++) {
    uint64_t k = rng();
    uint32_t h1 = host_of(&st, k);
    uint32_t h2 = host_of(&st, k);
    uint32_t h3 = host_of(&st, k);
    if (h1 != h2 || h2 != h3) {
      fprintf(stderr, "FAIL: host_of non-deterministic\n");
      return 1;
    }
  }

  // Latency check
  const int M = 1000000;
  uint64_t acc = 0;
  uint64_t t0 = now_ns();
  for (int i = 0; i < M; i++) {
    acc ^= host_of(&st, (uint64_t)i * 0x9E3779B97F4A7C15ULL);
  }
  uint64_t t1 = now_ns();
  double ns_per_op = (double)(t1 - t0) / M;
  printf("host_of latency: %.1f ns/op (acc=%lu)\n", ns_per_op, acc);
  if (ns_per_op > 50.0) {
    // Bottleneck check from plan: ≤ 10 ns. Allow 5x for noisy CI.
    fprintf(stderr, "WARN: host_of slower than 50 ns/op (budget 10 ns)\n");
  }

  printf("ALL PASS\n");
  return 0;
}
