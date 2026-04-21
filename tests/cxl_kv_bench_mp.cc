// Multi-process latency/throughput micro-bench for the CXL-FUSEE KV store.
//
// Forks N-1 children so N total processes share one CXL region, each with a
// distinct host_id. Each process populates a disjoint key range, then runs
// `ops_per_host` timed operations at the requested write ratio. Parent
// aggregates results from all children via a shared stats page at the end
// of the region.
//
// Build three variants — cxl_kv_bench_mp_A/B/C — via -DCONSENSUS_OPT=1/2/3.
//
// Usage:
//   ./cxl_kv_bench_mp_X <dev> <num_hosts> <ops_per_host> <wratio> [num_buckets]

#include "cxl_kv_store.h"
#include "cxl_mm.h"
#include "cxl_pending_ring.h"  // kMaxHosts

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
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

using fusee::CxlKvStore;
using fusee::CXLRegion;
using fusee::cxl_region_destroy;
using fusee::cxl_region_init;
using fusee::kConsensusOpt;
using fusee::kMaxHosts;

namespace {

struct HostStats {
  cacheline_u64 attached;     // set to 1 by host after attach succeeds
  cacheline_u64 ready;        // set to 1 by host when populate is done
  cacheline_u64 start_ack;    // set by host when it sees go signal
  cacheline_u64 done;         // set to 1 after the host finishes its ops
  cacheline_u64 thpt_ops;     // ops it executed
  cacheline_u64 wall_ns;      // wall ns it took
  cacheline_u64 w_avg_ns;
  cacheline_u64 w_p50_ns;
  cacheline_u64 w_p99_ns;
  cacheline_u64 r_avg_ns;
  cacheline_u64 r_p50_ns;
  cacheline_u64 r_p99_ns;
};

struct SharedStats {
  cacheline_u64 init_done;   // set by host 0 after its init_region memset
  cacheline_u64 go;
  HostStats hosts[kMaxHosts];
};

constexpr size_t kStatsOffsetFromEnd = 8192; // well above kCxlDevdaxAlign pad

uint64_t now_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t quantile_ns(std::vector<uint64_t> &v, double q) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[(size_t)(q * (v.size() - 1))];
}

uint64_t mean_ns(const std::vector<uint64_t> &v) {
  if (v.empty()) return 0;
  uint64_t s = 0;
  for (auto x : v) s += x;
  return s / v.size();
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 5) {
    fprintf(stderr,
            "usage: %s <dev> <num_hosts> <ops_per_host> <wratio> [num_buckets]\n",
            argv[0]);
    return 2;
  }
  const char *dev = argv[1];
  int num_hosts = atoi(argv[2]);
  uint64_t ops_per_host = strtoull(argv[3], nullptr, 0);
  double wratio = atof(argv[4]);
  uint32_t num_buckets = (argc >= 6) ? (uint32_t)strtoul(argv[5], nullptr, 0) : 16384U;
  if (num_hosts < 1 || num_hosts > kMaxHosts) {
    fprintf(stderr, "num_hosts must be 1..%d\n", kMaxHosts);
    return 2;
  }
  if (wratio < 0.0) wratio = 0.0;
  if (wratio > 1.0) wratio = 1.0;

  size_t store_bytes = CxlKvStore::bytes_for(num_buckets);
  size_t needed = ((store_bytes + kStatsOffsetFromEnd + fusee::kCxlDevdaxAlign - 1) /
                   fusee::kCxlDevdaxAlign) *
                  fusee::kCxlDevdaxAlign;

  // Fork num_hosts-1 children.
  // Two ways to spawn hosts:
  //   (a) fork mode (default): this invocation forks num_hosts-1 children,
  //       used for single-machine tests where every host is on the same
  //       physical box sharing one /dev/dax0.0.
  //   (b) role mode: set FUSEE_HOST_ID=N to make THIS invocation play host
  //       N, and do NOT fork. Use when every host is on a separate machine
  //       and gets a separate ssh invocation. Host 0 still prints the
  //       final HOST/AGG summary; other hosts publish their row into the
  //       shared stats page and exit.
  std::vector<pid_t> children;
  int host_id = 0;
  bool role_mode = false;
  {
    const char *role_env = getenv("FUSEE_HOST_ID");
    if (role_env && role_env[0] != '\0') {
      role_mode = true;
      host_id = atoi(role_env);
      if (host_id < 0 || host_id >= num_hosts) {
        fprintf(stderr, "FUSEE_HOST_ID=%d out of range [0,%d)\n",
                host_id, num_hosts);
        return 2;
      }
    }
  }
  if (!role_mode) {
    for (int i = 1; i < num_hosts; i++) {
      pid_t p = fork();
      if (p < 0) { perror("fork"); return 1; }
      if (p == 0) { host_id = i; children.clear(); break; }
      children.push_back(p);
    }
  }

  CXLRegion r{};
  if (cxl_region_init(&r, dev, needed) < 0) {
    fprintf(stderr, "[host %d] cxl_region_init failed\n", host_id);
    return 1;
  }

  auto *stats = reinterpret_cast<SharedStats *>(
      reinterpret_cast<char *>(r.base) + r.size - kStatsOffsetFromEnd);

  bool is_primary = (host_id == 0);
  if (is_primary) {
    // Zero stats upfront.
    std::memset(stats, 0, sizeof(*stats));
    flush_region(stats, sizeof(*stats));
    store_fence();
  }

  CxlKvStore store;
  if (is_primary) {
    // Primary attaches first with init_region=true. Only AFTER that does it
    // publish init_done, so non-primaries can safely attach without racing
    // against the init_region memset (which zeroes the pending-ring matrix).
    if (store.attach(r.base, r.size - kStatsOffsetFromEnd, num_buckets,
                     host_id, num_hosts, true) != 0) {
      fprintf(stderr, "[host 0] attach failed\n");
      cxl_region_destroy(&r);
      return 1;
    }
    CACHELINE_STORE(&stats->init_done, 1ULL);
  } else {
    // Non-primaries spin on init_done before attaching.
    while (CACHELINE_LOAD(&stats->init_done) == 0) __builtin_ia32_pause();
    if (store.attach(r.base, r.size - kStatsOffsetFromEnd, num_buckets,
                     host_id, num_hosts, false) != 0) {
      fprintf(stderr, "[host %d] attach failed\n", host_id);
      cxl_region_destroy(&r);
      _exit(1);
    }
  }

  // DRAM cache opt-in via FUSEE_CACHE=1 env var. Protocol semantics diverge:
  //  - C: reader checks CXL epoch, serves from DRAM on match.
  //  - B: reader checks DRAM invalidation flag set by peer ring push.
  //  - A: same as B plus writer waits for all replicators to invalidate.
  const char *cache_env = getenv("FUSEE_CACHE");
  if (cache_env && cache_env[0] == '1') {
    store.enable_dram_cache(true);
  }

  // "Attached" barrier: do not start inserting until every host has completed
  // attach() (so every replicator thread is running before anyone enqueues).
  CACHELINE_STORE(&stats->hosts[host_id].attached, 1ULL);
  for (int h = 0; h < num_hosts; h++) {
    while (CACHELINE_LOAD(&stats->hosts[h].attached) == 0) __builtin_ia32_pause();
  }

  // Populate: each host fills its own key range (disjoint).
  auto make_key = [&](int h, uint64_t i) -> uint64_t {
    return (static_cast<uint64_t>(h + 1) << 40) | (i + 1);
  };
  std::vector<uint64_t> my_keys;
  my_keys.reserve(ops_per_host);
  for (uint64_t i = 0; i < ops_per_host; i++) {
    uint64_t k = make_key(host_id, i);
    if (store.insert(k, k ^ 0x1234ULL) == 0) my_keys.push_back(k);
  }

  // Signal ready, wait for primary's go.
  CACHELINE_STORE(&stats->hosts[host_id].ready, 1ULL);

  if (is_primary) {
    for (int h = 0; h < num_hosts; h++) {
      while (CACHELINE_LOAD(&stats->hosts[h].ready) == 0) __builtin_ia32_pause();
    }
    CACHELINE_STORE(&stats->go, 1ULL);
  } else {
    while (CACHELINE_LOAD(&stats->go) == 0) __builtin_ia32_pause();
  }

  // Timed loop.
  std::mt19937_64 rng(0x1234u + host_id);
  std::uniform_int_distribution<size_t> pick(0, my_keys.empty() ? 0 : my_keys.size() - 1);
  std::vector<uint64_t> wlat, rlat;
  wlat.reserve((size_t)(ops_per_host * wratio) + 16);
  rlat.reserve((size_t)(ops_per_host * (1.0 - wratio)) + 16);

  uint64_t t0 = now_ns();
  for (uint64_t i = 0; i < ops_per_host; i++) {
    if (my_keys.empty()) break;
    uint64_t k = my_keys[pick(rng)];
    uint64_t ts = now_ns();
    if ((double)rng() / (double)rng.max() < wratio) {
      (void)store.update(k, k ^ (uint64_t)i);
      wlat.push_back(now_ns() - ts);
    } else {
      uint64_t out = 0;
      (void)store.search(k, &out);
      rlat.push_back(now_ns() - ts);
    }
  }
  uint64_t t1 = now_ns();

  // Dump Option A per-dst ACK-timeout counters. Stderr so stdout grep stays clean.
#if CONSENSUS_OPT == FUSEE_OPT_A
  fprintf(stderr, "[host %d] A ack_timeouts: ", host_id);
  for (int d = 0; d < num_hosts; d++) {
    if (d == host_id) continue;
    fprintf(stderr, "to%d=%lu ", d, store.ack_timeouts_to(d));
  }
  fprintf(stderr, " replicated_ops=%lu\n", store.replicated_ops());
#endif

  // Record per-host stats.
  CACHELINE_STORE(&stats->hosts[host_id].wall_ns, t1 - t0);
  CACHELINE_STORE(&stats->hosts[host_id].thpt_ops, ops_per_host);
  CACHELINE_STORE(&stats->hosts[host_id].w_avg_ns, mean_ns(wlat));
  CACHELINE_STORE(&stats->hosts[host_id].w_p50_ns, quantile_ns(wlat, 0.50));
  CACHELINE_STORE(&stats->hosts[host_id].w_p99_ns, quantile_ns(wlat, 0.99));
  CACHELINE_STORE(&stats->hosts[host_id].r_avg_ns, mean_ns(rlat));
  CACHELINE_STORE(&stats->hosts[host_id].r_p50_ns, quantile_ns(rlat, 0.50));
  CACHELINE_STORE(&stats->hosts[host_id].r_p99_ns, quantile_ns(rlat, 0.99));
  CACHELINE_STORE(&stats->hosts[host_id].done, 1ULL);

  // CRITICAL: wait for every host to finish its main loop BEFORE stopping our
  // replicator. Otherwise a host that finishes early kills its replicator
  // while a slower host is still writing — its last writes then see ACK
  // timeouts that are purely a bench-setup artifact, not a protocol issue.
  for (int h = 0; h < num_hosts; h++) {
    while (CACHELINE_LOAD(&stats->hosts[h].done) == 0) __builtin_ia32_pause();
  }
  store.stop();

  if (!is_primary) {
    cxl_region_destroy(&r);
    _exit(0);
  }
  for (pid_t p : children) {
    int status = 0;
    waitpid(p, &status, 0);
  }

  uint64_t total_ops = 0;
  double max_wall = 0.0;
  for (int h = 0; h < num_hosts; h++) {
    uint64_t w = CACHELINE_LOAD(&stats->hosts[h].wall_ns);
    uint64_t o = CACHELINE_LOAD(&stats->hosts[h].thpt_ops);
    total_ops += o;
    double ws = w / 1e9;
    if (ws > max_wall) max_wall = ws;

    printf("HOST opt=%c host=%d ops=%lu wall=%.3fs thpt=%.0f "
           "w_avg=%.2f w_p50=%.2f w_p99=%.2f "
           "r_avg=%.2f r_p50=%.2f r_p99=%.2f\n",
           kConsensusOpt, h, o, ws, (double)o / ws,
           CACHELINE_LOAD(&stats->hosts[h].w_avg_ns) / 1000.0,
           CACHELINE_LOAD(&stats->hosts[h].w_p50_ns) / 1000.0,
           CACHELINE_LOAD(&stats->hosts[h].w_p99_ns) / 1000.0,
           CACHELINE_LOAD(&stats->hosts[h].r_avg_ns) / 1000.0,
           CACHELINE_LOAD(&stats->hosts[h].r_p50_ns) / 1000.0,
           CACHELINE_LOAD(&stats->hosts[h].r_p99_ns) / 1000.0);
  }
  printf("AGG opt=%c num_hosts=%d wratio=%.2f total_ops=%lu "
         "wall_max=%.3fs agg_thpt=%.0f\n",
         kConsensusOpt, num_hosts, wratio, total_ops, max_wall,
         total_ops / max_wall);

  cxl_region_destroy(&r);
  return 0;
}
