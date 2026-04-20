// Unified latency / throughput micro-benchmark for the CXL-FUSEE KV store.
//
// Build three binaries — cxl_kv_bench_A, cxl_kv_bench_B, cxl_kv_bench_C —
// by linking the same source with different -DCONSENSUS_OPT values. Run
// each side-by-side to compare protocols on the same hardware.
//
// Single-process + single-thread for now. Multi-proc contention bench is
// a later follow-up; this measures the per-op cost of each protocol in the
// absence of writer-writer contention, which is the interesting baseline
// for comparing A vs B vs C.
//
// Usage:
//   ./cxl_kv_bench_X <dev_path> <ops> <wratio> [num_buckets]
//
//     wratio in [0.0, 1.0]: fraction of ops that are writes (updates of
//     previously inserted keys; mimics YCSB workload A at 0.5).

#include "cxl_kv_store.h"
#include "cxl_mm.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <vector>

extern "C" {
#include "common.h"
}

using fusee::CxlKvStore;
using fusee::CXLRegion;
using fusee::cxl_region_destroy;
using fusee::cxl_region_init;
using fusee::kConsensusOpt;

static uint64_t now_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr,
            "usage: %s <dev_path> <ops> <wratio> [num_buckets]\n"
            "   wratio in [0.0, 1.0]: fraction of ops that are writes.\n",
            argv[0]);
    return 2;
  }
  const char *dev = argv[1];
  uint64_t ops = strtoull(argv[2], nullptr, 0);
  double wratio = atof(argv[3]);
  uint32_t num_buckets = (argc >= 5) ? (uint32_t)strtoul(argv[4], nullptr, 0) : 8192U;

  if (wratio < 0.0) wratio = 0.0;
  if (wratio > 1.0) wratio = 1.0;

  // Size the region big enough for buckets + pending rings + slack.
  size_t store_bytes = CxlKvStore::bytes_for(num_buckets);
  size_t needed = ((store_bytes + fusee::kCxlDevdaxAlign - 1) /
                   fusee::kCxlDevdaxAlign) *
                  fusee::kCxlDevdaxAlign;

  CXLRegion r{};
  if (cxl_region_init(&r, dev, needed) < 0) {
    fprintf(stderr, "cxl_region_init failed\n");
    return 1;
  }

  CxlKvStore store;
  // Single-process: host_id=0 of num_hosts=1. Options A/B still spin up a
  // replicator thread that will sit idle (no (src!=self) rings to consume).
  if (store.attach(r.base, r.size, num_buckets, /*host_id=*/0,
                   /*num_hosts=*/1, /*init_region=*/true) != 0) {
    fprintf(stderr, "attach failed\n");
    cxl_region_destroy(&r);
    return 1;
  }

  // Populate: insert a base set of keys we can later update / search.
  constexpr uint64_t kBaseCount = 10000;
  std::vector<uint64_t> keys;
  keys.reserve(kBaseCount);
  for (uint64_t i = 0; i < kBaseCount; i++) {
    uint64_t k = (0xABCDULL << 48) | (i + 1);
    if (store.insert(k, k ^ 0x1234ULL) == 0) keys.push_back(k);
  }
  if (keys.empty()) {
    fprintf(stderr, "populate produced 0 keys; bucket table too small\n");
    store.stop();
    cxl_region_destroy(&r);
    return 1;
  }

  std::mt19937_64 rng(42);
  std::uniform_int_distribution<size_t> pick(0, keys.size() - 1);

  std::vector<uint64_t> write_lats_ns;
  std::vector<uint64_t> read_lats_ns;
  write_lats_ns.reserve((size_t)(ops * wratio) + 16);
  read_lats_ns.reserve((size_t)(ops * (1.0 - wratio)) + 16);

  uint64_t t0 = now_ns();
  for (uint64_t i = 0; i < ops; i++) {
    uint64_t k = keys[pick(rng)];
    uint64_t t_op = now_ns();
    if ((double)rng() / (double)rng.max() < wratio) {
      uint64_t v = k ^ (uint64_t)i;
      (void)store.update(k, v);
      write_lats_ns.push_back(now_ns() - t_op);
    } else {
      uint64_t out = 0;
      (void)store.search(k, &out);
      read_lats_ns.push_back(now_ns() - t_op);
    }
  }
  uint64_t t1 = now_ns();

  auto quantile = [](std::vector<uint64_t> &v, double q) -> double {
    if (v.empty()) return 0.0 / 0.0; // NaN
    std::sort(v.begin(), v.end());
    size_t idx = (size_t)(q * (v.size() - 1));
    return v[idx] / 1000.0; // return μs
  };
  auto mean = [](const std::vector<uint64_t> &v) -> double {
    if (v.empty()) return 0.0 / 0.0;
    uint64_t s = 0;
    for (auto x : v) s += x;
    return (double)s / v.size() / 1000.0;
  };

  double wall_s = (t1 - t0) / 1e9;
  double thpt = ops / wall_s;

  printf("RESULT opt=%c wratio=%.2f ops=%lu thpt=%.0f "
         "w_avg=%.2f w_p50=%.2f w_p99=%.2f "
         "r_avg=%.2f r_p50=%.2f r_p99=%.2f "
         "replicated=%lu\n",
         kConsensusOpt, wratio, ops, thpt,
         mean(write_lats_ns), quantile(write_lats_ns, 0.50),
         quantile(write_lats_ns, 0.99),
         mean(read_lats_ns),  quantile(read_lats_ns,  0.50),
         quantile(read_lats_ns,  0.99),
         store.replicated_ops());

  store.stop();
  cxl_region_destroy(&r);
  return 0;
}
