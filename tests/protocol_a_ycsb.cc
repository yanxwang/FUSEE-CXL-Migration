// iter-4A-redo: Protocol A YCSB sweep runner.
//
// 2-host workload-spec-driven runner for the new directory-based
// Protocol A code path. Cross-host coordination via run_cookie + 2-bit
// barrier in CXL header. Each host forks NUM_THREADS workers; each
// worker takes a global slice of trans_ops via i mod (2*T) == global_id.
//
// Output: one SUMMARY-format line per run on stdout (per spec §8):
//   YCSB opt=A cache=<0|1> num_hosts=2 threads=<T> threads_eff=<T> rep=<r>
//        load_ops=<N_load> load_thpt=<kops/s>
//        trans_ops=<N_trans> trans_wall_max=<seconds> trans_agg_thpt=<kops/s>
//        w_avg_ns=<...> w_p50_ns=<...> w_p99_ns=<...>
//        r_avg_ns=<...> r_p50_ns=<...> r_p99_ns=<...>
//        # <workload>_optA_t<T>_cache<on|off>_rep<r>
//
// Usage: FUSEE_NUM_HOSTS=2 FUSEE_HOST_ID=<0|1> FUSEE_NUM_THREADS=<T>
//        FUSEE_RUN_COOKIE=<ns> FUSEE_CACHE=<0|1> FUSEE_REP=<r>
//        FUSEE_WORKLOAD_NAME=<wl>
//        ./protocol_a_ycsb <dev> <load_file> <trans_file> <num_buckets> <max_ops>

#define _GNU_SOURCE
#include <sched.h>
#include <pthread.h>

#include "cxl_cache_pool.h"
#include "cxl_directory.h"
#include "cxl_forward_staging.h"
#include "cxl_hashtable.h"
#include "cxl_inval_ring.h"
#include "cxl_op_aggregator.h"
#include "cxl_read_ring.h"
#include "cxl_write_ring.h"
#include "cxl_kv_blockpool.h"
#include "cxl_kv_blockpool_freelist.h"
#include "cxl_kv_ops_A.h"
#include "cxl_mm.h"
#include "cxl_probe.h"
#include "cxl_sharding.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <random>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include "common.h"
}

using namespace fusee;

namespace {

uint64_t now_ns() {
  timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t hash_str(const std::string &s) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (unsigned char c : s) { h ^= c; h *= 0x100000001b3ULL; }
  if (h == 0) h = 1;
  return h;
}

enum OpKind : uint8_t { OP_INSERT, OP_READ, OP_UPDATE, OP_DELETE, OP_SKIP };
OpKind parse_op(const std::string &s) {
  if (s == "INSERT") return OP_INSERT;
  if (s == "READ") return OP_READ;
  if (s == "UPDATE") return OP_UPDATE;
  if (s == "DELETE") return OP_DELETE;
  return OP_SKIP;
}
struct Op { OpKind kind; uint64_t key; };

std::vector<Op> load_ops_from_file(const std::string &path) {
  std::vector<Op> out;
  std::ifstream f(path);
  if (!f) {
    fprintf(stderr, "open %s: %s\n", path.c_str(), strerror(errno));
    return out;
  }
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    size_t p1 = line.find_first_of(" \t"); if (p1 == std::string::npos) continue;
    size_t p2 = line.find_first_not_of(" \t", p1); if (p2 == std::string::npos) continue;
    size_t p3 = line.find_first_of(" \t", p2);
    std::string op_tok = line.substr(0, p1), key_tok;
    if (p3 == std::string::npos) key_tok = line.substr(p2);
    else {
      size_t p4 = line.find_first_not_of(" \t", p3);
      if (p4 == std::string::npos) continue;
      key_tok = line.substr(p4);
    }
    while (!key_tok.empty() &&
           (key_tok.back()==' ' || key_tok.back()=='\t' ||
            key_tok.back()=='\n' || key_tok.back()=='\r')) {
      key_tok.pop_back();
    }
    OpKind k = parse_op(op_tok);
    if (k == OP_SKIP) continue;
    out.push_back({k, hash_str(key_tok)});
  }
  return out;
}

uint64_t quantile_ns(std::vector<uint64_t> &v, double q) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[(size_t)(q * (v.size() - 1))];
}

constexpr int kMaxClients = 256;

struct alignas(64) WorkerStats {
  cacheline_u64 done;
  cacheline_u64 trans_ops;
  cacheline_u64 trans_wall_ns;
  cacheline_u64 w_count;
  cacheline_u64 w_sum_ns;
  cacheline_u64 w_p50_ns;
  cacheline_u64 w_p99_ns;
  cacheline_u64 r_count;
  cacheline_u64 r_sum_ns;
  cacheline_u64 r_p50_ns;
  cacheline_u64 r_p99_ns;
  // iter-11A Phase 0: bimodal-cell investigation probe
  cacheline_u64 first_op_ns;       // duration of the first successful op (attach-warmup signal)
  cacheline_u64 ops_to_first_ms;   // wall ns between t_start and first op completion
};

}  // anonymous namespace

int main(int argc, char **argv) {
  if (argc < 6) {
    fprintf(stderr,
      "usage: %s <dev> <load_file> <trans_file> <num_buckets> <max_ops>\n",
      argv[0]);
    return 2;
  }
  const char *dev = argv[1];
  const char *load_path = argv[2];
  const char *trans_path = argv[3];
  uint32_t num_buckets = (uint32_t)strtoul(argv[4], nullptr, 0);
  uint64_t max_ops = strtoull(argv[5], nullptr, 0);

  int num_hosts = 1, host_id = 0, num_threads = 1, rep = 1;
  uint64_t cookie = 0;
  bool cache_on = false;
  std::string wl_name = "wl";
  int ring_shards_factor = 0;  // iter-17A: 0 = disabled (single-shard baseline).
                               // Set to 4 or 8 to enable multi-ring scaling.
  if (const char *e = getenv("FUSEE_NUM_HOSTS")) num_hosts = atoi(e);
  if (const char *e = getenv("FUSEE_HOST_ID")) host_id = atoi(e);
  if (const char *e = getenv("FUSEE_NUM_THREADS")) num_threads = atoi(e);
  if (const char *e = getenv("FUSEE_RUN_COOKIE")) cookie = strtoull(e, nullptr, 0);
  if (const char *e = getenv("FUSEE_REP")) rep = atoi(e);
  if (const char *e = getenv("FUSEE_CACHE")) cache_on = (e[0] == '1');
  if (const char *e = getenv("FUSEE_WORKLOAD_NAME")) wl_name = e;
  if (const char *e = getenv("FUSEE_RING_SHARDS_FACTOR")) ring_shards_factor = atoi(e);
  if (ring_shards_factor < 0) ring_shards_factor = 0;

  // iter-17A Plan B opt-in: FUSEE_RING_ROUTING=key_hash → ring_idx = fnv1a(key) % shards
  // Default (any other value / unset) = Plan A (ring_idx = client_id / block_size)
  int routing_mode = 0;
  if (const char *e = getenv("FUSEE_RING_ROUTING")) {
    if (std::string(e) == "key_hash") routing_mode = 1;
  }

  if (num_threads > kMaxClients) num_threads = kMaxClients;

  // iter-17A: configure ring sharding + routing mode before any
  // enable_*_ring spawns. Both set process-wide statics inherited by forks.
  fusee::CxlKvStoreA::set_ring_routing_mode(routing_mode);
  fusee::CxlKvStoreA::configure_ring_sharding(num_threads, ring_shards_factor);

  // iter-21A: FUSEE_BENCH_MODE switches the workload generator.
  //   "ycsb" (default)  → existing trans-file replay (Fig 13).
  //   "fig10"           → single-client × N×4 op types serial, per-op µs dump.
  //   "fig11"           → multi-client × 4 timed phases (paper §6.2 Fig 11).
  std::string bench_mode = "ycsb";
  if (const char *e = getenv("FUSEE_BENCH_MODE")) bench_mode = e;
  bool is_fig10 = (bench_mode == "fig10");
  bool is_fig11 = (bench_mode == "fig11");
  bool is_microbench = is_fig10 || is_fig11;

  // Fig 10/11 procedural workload params (no trans file needed).
  uint64_t fig_keys_per_client =
      (uint64_t)(getenv("FUSEE_F_KEYS_PER_CLIENT")
                     ? strtoull(getenv("FUSEE_F_KEYS_PER_CLIENT"), nullptr, 0)
                     : (is_fig10 ? 100000ULL : 100ULL));
  int fig_ins_ms  = getenv("FUSEE_F_INS_MS")  ? atoi(getenv("FUSEE_F_INS_MS"))  : 500;
  int fig_read_ms = getenv("FUSEE_F_READ_MS") ? atoi(getenv("FUSEE_F_READ_MS")) : 5000;
  int fig_upd_ms  = getenv("FUSEE_F_UPD_MS")  ? atoi(getenv("FUSEE_F_UPD_MS"))  : 5000;
  int fig_del_ms  = getenv("FUSEE_F_DEL_MS")  ? atoi(getenv("FUSEE_F_DEL_MS"))  : 500;

  // Parse workload traces (only for ycsb mode; fig10/11 generate procedurally).
  std::vector<Op> load_ops, trans_ops;
  if (!is_microbench) {
    load_ops  = load_ops_from_file(load_path);
    trans_ops = load_ops_from_file(trans_path);
    if (max_ops > 0) {
      if (load_ops.size() > max_ops) load_ops.resize(max_ops);
      if (trans_ops.size() > max_ops) trans_ops.resize(max_ops);
    }
  }

  // CXL region layout (iter-9A redo Phase 2):
  //   [4 KB header][bucket array][KvBlockPool region]
  //   [WriteRingMatrix][ReadRingMatrix][InvalRingMatrix]
  //   [ForwardStagingMatrix][stats]
  std::size_t bucket_bytes = sizeof(CxlKvBucket) * num_buckets;
  // iter-5A Phase 7: KV_SIZE (= block_size) is per-cell, env-driven.
  uint32_t kBlockSize = 256;
  if (const char *e = getenv("FUSEE_KV_SIZE")) {
    int v = atoi(e);
    // iter-16A V-sweep: allow V=64 in addition to 256/512/1024.
    if (v == 64 || v == 256 || v == 512 || v == 1024) kBlockSize = (uint32_t)v;
  }
  uint64_t want_blocks;
  if (is_fig10) {
    // single client × N pre-load + N INSERT phase + 4× UPDATE churn
    want_blocks = std::max((uint64_t)64, fig_keys_per_client * 8);
  } else if (is_fig11) {
    // c=128 × kKeysPerClient pre-load + INSERT growth + 4× UPDATE churn
    want_blocks = std::max((uint64_t)64,
                           (uint64_t)num_hosts * (uint64_t)num_threads *
                               fig_keys_per_client * 8);
  } else {
    want_blocks = std::max((uint64_t)64,
                            (uint64_t)trans_ops.size() * 2 +
                            (uint64_t)load_ops.size());
  }
  if (want_blocks > 1ULL << 23) want_blocks = 1ULL << 23;
  std::size_t pool_bytes =
      CxlKvBlockPool::bytes_for((uint32_t)want_blocks, kBlockSize, num_hosts);
  std::size_t wr_bytes = write_ring_matrix_bytes();
  std::size_t rr_bytes = read_ring_matrix_bytes();
  std::size_t ir_bytes = inval_ring_matrix_bytes();
  std::size_t fs_bytes = forward_staging_matrix_bytes();
  std::size_t rs_bytes = read_staging_matrix_bytes();  // iter-11A Phase 1
  // iter-13A Phase 1: RCU + Hazard domains (always laid out, used per
  // FUSEE_READ_GUARD build flag — CXL offsets must be identical across
  // STAGING/RCU/HAZARD builds).
  std::size_t rcu_bytes = rcu_domain_bytes();
  std::size_t haz_bytes = hazard_domain_bytes();
  // iter-13A Phase 2 W3: reservation ring (always laid out).
  std::size_t rsv_bytes = reservation_ring_matrix_bytes();
  std::size_t stats_bytes = sizeof(WorkerStats) * 2 * kMaxClients;
  std::size_t header_bytes = 4096;
  std::size_t total = header_bytes + bucket_bytes + pool_bytes
                    + wr_bytes + rr_bytes + ir_bytes + fs_bytes + rs_bytes
                    + rcu_bytes + haz_bytes + rsv_bytes
                    + stats_bytes + 4096;
  total = ((total + kCxlDevdaxAlign - 1) / kCxlDevdaxAlign) * kCxlDevdaxAlign;

  CXLRegion r{};
  if (cxl_region_init(&r, dev, total) < 0) {
    fprintf(stderr, "cxl_region_init failed (need %zu B)\n", total);
    return 1;
  }

  struct alignas(64) Header {
    cacheline_u64 run_cookie;
    cacheline_u64 init_done;
    cacheline_u64 trans_go;
    // iter-21A Fig 11: per-phase per-host arrival counters. Each worker
    // atomic-fetch-adds its host's slot when it finishes phase P;
    // primary spins until all hosts' slots == num_workers_per_host.
    // Layout: fig11_phase_arrived[P][H] = # workers on host H done with phase P.
    // 6 phases (preload + 4 timed + end), 2 hosts max.
    cacheline_u64 fig11_phase_arrived[6 * 2];
  };
  Header *hdr = reinterpret_cast<Header *>(r.base);
  CxlKvBucket *buckets = reinterpret_cast<CxlKvBucket *>(
      reinterpret_cast<char *>(r.base) + header_bytes);
  void *pool_mem = reinterpret_cast<char *>(buckets) + bucket_bytes;
  void *wr_mem = reinterpret_cast<char *>(pool_mem) + pool_bytes;
  void *rr_mem = reinterpret_cast<char *>(wr_mem) + wr_bytes;
  void *ir_mem = reinterpret_cast<char *>(rr_mem) + rr_bytes;
  void *fs_mem = reinterpret_cast<char *>(ir_mem) + ir_bytes;
  void *rs_mem = reinterpret_cast<char *>(fs_mem) + fs_bytes;  // iter-11A Phase 1
  void *rcu_mem = reinterpret_cast<char *>(rs_mem) + rs_bytes;  // iter-13A
  void *haz_mem = reinterpret_cast<char *>(rcu_mem) + rcu_bytes;  // iter-13A
  void *rsv_mem = reinterpret_cast<char *>(haz_mem) + haz_bytes;  // iter-13A W3
  WorkerStats *stats = reinterpret_cast<WorkerStats *>(
      reinterpret_cast<char *>(rsv_mem) + rsv_bytes);

  bool is_host_primary = (host_id == 0);
  if (is_host_primary) {
    CACHELINE_STORE(&hdr->init_done, 0ULL);
    CACHELINE_STORE(&hdr->trans_go, 0ULL);
    CACHELINE_STORE(&hdr->run_cookie, cookie);
    flush_line(hdr); store_fence();
    std::memset(stats, 0, stats_bytes);
    flush_region(stats, stats_bytes); store_fence();
  } else {
    while (true) {
      flush_line(hdr); full_fence();
      if (CACHELINE_LOAD(&hdr->run_cookie) == cookie) break;
      __builtin_ia32_pause();
    }
  }

  // DRAM regions PRE-FORK.
  ShardingTable st;
  sharding_init(&st, (uint32_t)num_hosts);

  void *dir_mem = mmap(nullptr,
                       slot_directory_bytes(num_buckets, kCxlKvSlotsPerBucket),
                       PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (dir_mem == MAP_FAILED) { fprintf(stderr, "mmap dir failed\n"); return 1; }
  SlotDirectory dir;
  slot_directory_init(&dir, dir_mem, num_buckets, kCxlKvSlotsPerBucket);

  // iter-15A microbench plan: FUSEE_CACHE_BUCKETS env var lets cache_pool
  // size be set INDEPENDENTLY of the data-plane hash table num_buckets.
  // Must be power-of-2 (cache_pool_init enforces this).
  //
  // iter-19A Phase 1d FINAL: cap cache_buckets so cache_pool fits L3.
  // Anomaly A root cause: when cache_pool footprint exceeds L3 capacity,
  // 64 concurrent workers cause L3 thrashing → throughput drops monotonically
  // with cache_pool size. Measured monotonic decline: 143 Mops at 35 MB
  // (cb=8192) → 32 Mops at 8.8 GB (cb=2097152), 4.5× slowdown. The knee is
  // at cache_pool ≈ L3 capacity (~150 MB on Sapphire Rapids).
  //
  // FIX: default cache_buckets = min(num_buckets, FIT_L3). Override via env
  // for benchmarking only. FUSEE_CACHE_BUCKETS_RAW=1 disables the cap.
  uint32_t cache_buckets;
  {
    // Estimate L3 capacity (Sapphire Rapids 8-port = 150 MB; conservative
    // default 128 MB so we leave room for other L3-resident state). Override
    // via FUSEE_L3_CAP_MB env if measuring on a different platform.
    size_t l3_cap_bytes = 128ULL * 1024 * 1024;
    if (const char *e = getenv("FUSEE_L3_CAP_MB")) {
      long mb = atol(e);
      if (mb > 0) l3_cap_bytes = (size_t)mb * 1024 * 1024;
    }
    // bucket_bytes = sizeof(KvCacheBucket) — actual value depends on
    // FUSEE_CACHE_VALUE_MAX (build flag). Use the runtime cache_pool_bytes()
    // function: bytes_for(N) = N × sizeof(KvCacheBucket).
    size_t per_bucket_bytes = cache_pool_bytes(1);
    uint32_t fit_l3 = (uint32_t)(l3_cap_bytes / per_bucket_bytes);
    // round DOWN to nearest power of 2
    uint32_t p2 = 1;
    while ((p2 << 1) > 0 && (p2 << 1) <= fit_l3) p2 <<= 1;
    fit_l3 = p2;
    cache_buckets = std::min(num_buckets, fit_l3);
    if (const char *e = getenv("FUSEE_CACHE_BUCKETS")) {
      uint32_t cb = (uint32_t)strtoul(e, nullptr, 0);
      if (cb > 0 && (cb & (cb - 1)) == 0) {
        // Explicit override (for benchmarking / sweeps). Warn if exceeding
        // fit_l3 — this is the documented Anomaly A regime.
        cache_buckets = cb;
        if (cb > fit_l3) {
          fprintf(stderr, "[cache] WARN: FUSEE_CACHE_BUCKETS=%u exceeds "
                          "L3-fit (%u buckets / %zu MB). Anomaly A territory; "
                          "throughput will drop ~%.0fx vs L3-fit default.\n",
                  cb, fit_l3, l3_cap_bytes / (1024 * 1024),
                  (double)cb / fit_l3);
        }
      } else {
        fprintf(stderr, "[WARN] FUSEE_CACHE_BUCKETS=%s ignored (not pow-of-2); "
                        "using L3-fit default %u\n", e, cache_buckets);
      }
    }
    fprintf(stderr, "[cache] cache_buckets=%u (cache_pool=%.1f MB), "
                    "L3-fit cap=%u (%zu MB)\n",
            cache_buckets,
            cache_pool_bytes(cache_buckets) / (1024.0 * 1024.0),
            fit_l3, l3_cap_bytes / (1024 * 1024));
  }
  size_t cache_bytes = cache_pool_bytes(cache_buckets);
  void *cache_mem = mmap(nullptr, cache_bytes,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (cache_mem == MAP_FAILED) { fprintf(stderr, "mmap cache failed\n"); return 1; }
  // iter-19A Phase 1b M2: FUSEE_CACHE_HUGEPAGE=1 → madvise hugepage on
  // cache_pool. Reduces PTE count from N×4KB pages to N/512 × 2MB hugepages.
  // Synthetic showed no effect in single-process; FUSEE has 64 forked
  // workers each with own page table, so HP could help.
  if (const char *e = getenv("FUSEE_CACHE_HUGEPAGE"); e && atoi(e) != 0) {
    if (madvise(cache_mem, cache_bytes, MADV_HUGEPAGE) != 0) {
      fprintf(stderr, "[cache] madvise HUGEPAGE failed: %s\n", strerror(errno));
    } else {
      fprintf(stderr, "[cache] madvise HUGEPAGE ok (%.1f MiB)\n",
              cache_bytes / (1024.0 * 1024.0));
    }
  }
  KvCachePool cache;
  cache_pool_init(&cache, cache_mem, cache_buckets);
  // iter-19A Phase 1b M1: FUSEE_CACHE_PRETOUCH=1 → write 1 byte to every
  // 4KB page of cache_pool BEFORE fork. Without this, child processes
  // page-fault on first touch of value_bytes during TRANS, scattering
  // 19.5k minor faults / worker at c100. Pre-touching forces parent to
  // allocate all pages; fork shares them via inherited PTEs.
  if (const char *e = getenv("FUSEE_CACHE_PRETOUCH"); e && atoi(e) != 0) {
    uint64_t t0 = now_ns();
    volatile uint8_t *p = (volatile uint8_t *)cache_mem;
    uint64_t step = 4096;
    for (size_t off = 0; off < cache_bytes; off += step) p[off] = 0;
    uint64_t t1 = now_ns();
    fprintf(stderr, "[cache] PRETOUCH %.1f MiB in %.3f s\n",
            cache_bytes / (1024.0 * 1024.0), (t1 - t0) / 1e9);
  }

  BlockFreeList fl;
  block_freelist_init(&fl);

  CxlKvBlockPool pool;
  if (pool.attach(pool_mem, pool_bytes,
                  (uint32_t)want_blocks, kBlockSize,
                  host_id, num_hosts, is_host_primary) != 0) {
    fprintf(stderr, "pool.attach failed\n");
    return 1;
  }

  // iter-9A Phase 2.C: per-host aggregator region, mmap'd PRE-FORK
  // (MAP_SHARED|MAP_ANONYMOUS) so all workers + the 3 sender threads
  // see the same DRAM. Zeroed once by the host primary.
  void *aggr_mem = mmap(nullptr, aggregator_region_bytes(),
                        PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (aggr_mem == MAP_FAILED) {
    fprintf(stderr, "mmap aggregator failed\n");
    return 1;
  }
  AggregatorRegion *aggr = reinterpret_cast<AggregatorRegion *>(aggr_mem);
  if (host_id == 0) {
    std::memset(aggr, 0, aggregator_region_bytes());
  }

  // Fork num_threads-1 children. Parent has client_id=0.
  std::vector<pid_t> children;
  int client_id = 0;
  for (int i = 1; i < num_threads; i++) {
    pid_t p = fork();
    if (p < 0) { perror("fork"); return 1; }
    if (p == 0) { client_id = i; children.clear(); break; }
    children.push_back(p);
  }

  bool is_primary_client = (host_id == 0) && (client_id == 0);
  bool is_host_primary_client = (client_id == 0);

  // iter-9A C3: pin worker process to cpu = client_id (T=64 →
  // workers cpu 0..63; system threads cpu 64..69 inside the
  // protocol library; spare cpu 70..85). Each worker is a separate
  // forked process with one main thread; pinning the process pins
  // that thread.
  {
    cpu_set_t cs;
    CPU_ZERO(&cs);
    int target_cpu = client_id;  // 0..(T-1)
    if (target_cpu >= 64) {
      // System-thread region; refuse.
      fprintf(stderr,
        "[A:thread] FATAL worker client_id=%d would land on cpu>=64 (system area)\n",
        client_id);
      _exit(1);
    }
    CPU_SET(target_cpu, &cs);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
    if (rc != 0) {
      fprintf(stderr,
        "[A:thread] worker pin FAILED rc=%d host=%d client=%d cpu=%d\n",
        rc, host_id, client_id, target_cpu);
    }
    fprintf(stderr,
      "[A:thread] Worker pinned host=%d client=%d -> cpu=%d (pid=%d)\n",
      host_id, client_id, target_cpu, getpid());
  }

  CxlKvStoreA store;
  if (is_primary_client) {
    if (store.attach(buckets, num_buckets, host_id, num_hosts,
                     /*init_region=*/true,
                     &st, &dir, &cache, &fl, &pool) != 0) {
      fprintf(stderr, "primary attach failed\n"); return 1;
    }
    WriteRingMatrix *wr = reinterpret_cast<WriteRingMatrix *>(wr_mem);
    ReadRingMatrix  *rr = reinterpret_cast<ReadRingMatrix  *>(rr_mem);
    InvalRingMatrix *ir = reinterpret_cast<InvalRingMatrix *>(ir_mem);
    ForwardStagingMatrix *fs =
        reinterpret_cast<ForwardStagingMatrix *>(fs_mem);
    ReadStagingMatrix *rs =
        reinterpret_cast<ReadStagingMatrix *>(rs_mem);  // iter-11A Phase 1
    // iter-19A Phase 1d H17: FUSEE_DISABLE_RECEIVERS=1 → skip spawning the
    // 4 receiver/handler threads (Write/Read/Inval/Reserv). For local_read
    // workload these threads have no work to do but constantly poll CXL
    // rings → 40 % of process-level L1 misses go to them per perf
    // attribution. Spawning still inits the rings (for correctness of any
    // cross-host op that might fire) but disables the polling loops.
    bool spawn_recv = true;
    if (const char *e = getenv("FUSEE_DISABLE_RECEIVERS"); e && atoi(e) != 0) {
      spawn_recv = false;
      fprintf(stderr, "[H17] receivers DISABLED (no polling threads)\n");
    }
    if (store.enable_write_ring(wr, fs, /*init=*/true, /*spawn=*/spawn_recv) != 0) {
      fprintf(stderr, "primary enable_write_ring failed\n"); return 1;
    }
    if (store.enable_read_ring(rr, rs, /*init=*/true, /*spawn=*/spawn_recv) != 0) {
      fprintf(stderr, "primary enable_read_ring failed\n"); return 1;
    }
    if (store.enable_invalidate(ir, /*init=*/true, /*spawn=*/spawn_recv) != 0) {
      fprintf(stderr, "primary enable_invalidate failed\n"); return 1;
    }
    // iter-13A Phase 1: wire read-guard CXL domains.
    {
      RcuDomain *rcu_d = reinterpret_cast<RcuDomain *>(rcu_mem);
      HazardDomain *haz_d = reinterpret_cast<HazardDomain *>(haz_mem);
      if (store.enable_read_guard(rcu_d, haz_d, /*init=*/true) != 0) {
        fprintf(stderr, "primary enable_read_guard failed\n"); return 1;
      }
    }
    // iter-20A: ReservationRing wire-up removed (BATCHED mode deleted).
    if (store.enable_senders(aggr, num_threads, /*spawn=*/true) != 0) {
      fprintf(stderr, "primary enable_senders failed\n"); return 1;
    }
    if (store.assert_n_to_n_active() != 0) return 1;
    uint64_t cur = CACHELINE_LOAD(&hdr->init_done);
    CACHELINE_STORE(&hdr->init_done, cur | 0x1ULL);
    flush_line(&hdr->init_done); store_fence();
  } else {
    // EVERYONE except host 0 primary waits for bit 0 (host 0 primary
    // has finished init=true attach + ring matrix memset). This
    // includes host 1 primary AND all non-primary children — without
    // this barrier, host 1 races with host 0's memset of buckets +
    // forward ring matrix.
    while (true) {
      flush_line(&hdr->init_done); full_fence();
      if ((CACHELINE_LOAD(&hdr->init_done) & 0x1ULL) != 0) break;
      __builtin_ia32_pause();
    }
    if (store.attach(buckets, num_buckets, host_id, num_hosts,
                     /*init_region=*/false,
                     &st, &dir, &cache, &fl, &pool) != 0) {
      fprintf(stderr, "[h%d c%d] attach failed\n", host_id, client_id);
      return 1;
    }
    // iter-15A microbench HR-2: wire ring pointers in child workers so
    // their forward_*_direct calls don't silently fail with wr_=nullptr.
    // Host 1 primary still calls enable_*_ring(spawn=true) below, which
    // re-sets these pointers AND memsets/spawns — both safe (idempotent
    // pointer writes). Non-primary children only get the wiring here.
    {
      WriteRingMatrix *wr = reinterpret_cast<WriteRingMatrix *>(wr_mem);
      ReadRingMatrix  *rr = reinterpret_cast<ReadRingMatrix  *>(rr_mem);
      InvalRingMatrix *ir = reinterpret_cast<InvalRingMatrix *>(ir_mem);
      ForwardStagingMatrix *fs =
          reinterpret_cast<ForwardStagingMatrix *>(fs_mem);
      ReadStagingMatrix *rs =
          reinterpret_cast<ReadStagingMatrix *>(rs_mem);
      ReservationRingMatrix *rsv =
          reinterpret_cast<ReservationRingMatrix *>(rsv_mem);
      RcuDomain *rcu_d = reinterpret_cast<RcuDomain *>(rcu_mem);
      HazardDomain *haz_d = reinterpret_cast<HazardDomain *>(haz_mem);
      store.wire_rings_for_child(wr, fs, rr, rs, ir, rsv, rcu_d, haz_d);
    }
    // Every non-primary child attaches the aggregator (no spawn) so
    // worker threads route through it. Primary child calls
    // enable_senders(spawn=true) below.
    if (!(host_id == 1 && client_id == 0)) {
      if (store.enable_senders(aggr, num_threads, /*spawn=*/false) != 0) {
        fprintf(stderr, "[h%d c%d] enable_senders(noSpawn) failed\n",
                host_id, client_id);
        return 1;
      }
    }
    if (host_id == 1 && client_id == 0) {
      // host 1 primary: attach (no init) the 3-ring + staging mesh and
      // spawn its three named receivers.
      WriteRingMatrix *wr = reinterpret_cast<WriteRingMatrix *>(wr_mem);
      ReadRingMatrix  *rr = reinterpret_cast<ReadRingMatrix  *>(rr_mem);
      InvalRingMatrix *ir = reinterpret_cast<InvalRingMatrix *>(ir_mem);
      ForwardStagingMatrix *fs =
          reinterpret_cast<ForwardStagingMatrix *>(fs_mem);
      ReadStagingMatrix *rs =
          reinterpret_cast<ReadStagingMatrix *>(rs_mem);  // iter-11A Phase 1
      bool spawn_recv_h1 = true;
      if (const char *e = getenv("FUSEE_DISABLE_RECEIVERS"); e && atoi(e) != 0) {
        spawn_recv_h1 = false;
      }
      if (store.enable_write_ring(wr, fs, /*init=*/false, /*spawn=*/spawn_recv_h1) != 0) {
        fprintf(stderr, "[h1 primary] enable_write_ring failed\n"); return 1;
      }
      if (store.enable_read_ring(rr, rs, /*init=*/false, /*spawn=*/spawn_recv_h1) != 0) {
        fprintf(stderr, "[h1 primary] enable_read_ring failed\n"); return 1;
      }
      if (store.enable_invalidate(ir, /*init=*/false, /*spawn=*/spawn_recv_h1) != 0) {
        fprintf(stderr, "[h1 primary] enable_invalidate failed\n"); return 1;
      }
      // iter-13A Phase 1: host 1 primary also wires read-guard.
      {
        RcuDomain *rcu_d = reinterpret_cast<RcuDomain *>(rcu_mem);
        HazardDomain *haz_d = reinterpret_cast<HazardDomain *>(haz_mem);
        if (store.enable_read_guard(rcu_d, haz_d, /*init=*/false) != 0) {
          fprintf(stderr, "[h1 primary] enable_read_guard failed\n"); return 1;
        }
      }
      // iter-20A: ReservationRing wire-up removed (BATCHED mode deleted).
      if (store.enable_senders(aggr, num_threads, /*spawn=*/true) != 0) {
        fprintf(stderr, "[h1 primary] enable_senders failed\n"); return 1;
      }
      if (store.assert_n_to_n_active() != 0) return 1;
      uint64_t cur = CACHELINE_LOAD(&hdr->init_done);
      CACHELINE_STORE(&hdr->init_done, cur | 0x2ULL);
      flush_line(&hdr->init_done); store_fence();
    }
    if (host_id == 1 && client_id != 0) {
      while (true) {
        flush_line(&hdr->init_done); full_fence();
        if ((CACHELINE_LOAD(&hdr->init_done) & 0x2ULL) != 0) break;
        __builtin_ia32_pause();
      }
    }
  }
  (void)cache_on;  // Protocol A's cache is always-on; FUSEE_CACHE no-op
                   // here, retained only for SUMMARY.log compat.

  // iter-9A Phase 2.C: opt-in aggregator routing. Default OFF — the
  // single-sender-per-ring design without batching is the spec
  // implementation but bottlenecks at high T (T=64 path_decomp showed
  // workload-A KV=1024 cache=on at 0.5 Mops/s vs 9.89 Mops/s direct).
  // Enable with FUSEE_USE_AGGREGATOR=1 to exercise the spec path
  // (correctness equivalent — same hash-diff result — but slower
  // until iter-10A adds batching to the senders).
  //
  // iter-17A: set_worker_id MUST be called unconditionally so that
  // worker_ring_idx_helper() in cxl_kv_ops_A.cc can route to the right
  // ring shard (Plan A). When aggregator is off (default), aggregator-
  // routing code path falls back to *_direct via the `!aggr_` check, so
  // calling set_worker_id is a no-op for the aggregator gate.
  CxlKvStoreA::set_worker_id(client_id);
  if (const char *e = getenv("FUSEE_USE_AGGREGATOR"); e && e[0] == '1') {
    // (aggregator still requires aggr_ wired; set_worker_id above is
    // already in effect.)
  }

  // iter-20A: TLS L1 cache (iter-10A Phase 1.C) deleted; cache_pool L2
  // is now the only worker-side caching layer.

  // Cross-host primary barrier: both hosts inited.
  // (Skipped entirely when num_hosts == 1 — no peer to wait for.)
  if (is_host_primary_client && num_hosts > 1) {
    if (host_id == 0) {
      while (true) {
        flush_line(&hdr->init_done); full_fence();
        if ((CACHELINE_LOAD(&hdr->init_done) & 0x2ULL) != 0) break;
        __builtin_ia32_pause();
      }
      CACHELINE_STORE(&hdr->trans_go, cookie);
      flush_line(&hdr->trans_go); store_fence();
    } else {
      while (true) {
        flush_line(&hdr->trans_go); full_fence();
        if (CACHELINE_LOAD(&hdr->trans_go) == cookie) break;
        __builtin_ia32_pause();
      }
    }
  }

  // iter-9A Phase 1 C1: real N-byte payload, deterministic byte
  // pattern based on key (so search verifies exact bytes back).
  // payload[i] = (key ^ i) & 0xff. Buffer sized to FUSEE_KV_SIZE.
  std::vector<uint8_t> wbuf(kBlockSize, 0);
  std::vector<uint8_t> rbuf(kBlockSize, 0);
  auto fill_pattern = [&](uint64_t key, uint8_t *buf, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
      buf[i] = (uint8_t)((key >> (i & 7)) ^ i);
    }
  };
  // Effective payload size: 4B header + value bytes ≤ block_size.
  uint32_t value_bytes = (kBlockSize > 4) ? (kBlockSize - 4) : 8;

  // -------- LOAD phase: each host primary loads its own owned keys. --------
  uint64_t load_thpt_kops = 0;
  if (is_host_primary_client) {
    uint64_t t0 = now_ns();
    uint64_t loaded = 0;
    for (auto &op : load_ops) {
      if (op.kind != OP_INSERT) continue;
      if (host_of(&st, op.key) != (uint32_t)host_id) continue;
      fill_pattern(op.key, wbuf.data(), value_bytes);
      store.insert(op.key, wbuf.data(), value_bytes);
      loaded++;
    }
    uint64_t t1 = now_ns();
    if (t1 > t0) load_thpt_kops = loaded * 1000000ULL / ((t1 - t0) / 1000ULL + 1);
  }

  // Cross-host barrier: both hosts done loading.
  // (Skipped for primary cross-host wait when num_hosts == 1.)
  if (is_host_primary_client) {
    uint64_t my_bit = (host_id == 0) ? 0x10ULL : 0x20ULL;
    uint64_t peer_bit = (host_id == 0) ? 0x20ULL : 0x10ULL;
    uint64_t cur = CACHELINE_LOAD(&hdr->init_done);
    CACHELINE_STORE(&hdr->init_done, cur | my_bit);
    flush_line(&hdr->init_done); store_fence();
    if (num_hosts > 1) {
      while (true) {
        flush_line(&hdr->init_done); full_fence();
        if ((CACHELINE_LOAD(&hdr->init_done) & peer_bit) != 0) break;
        __builtin_ia32_pause();
      }
    }
    // iter-15A bimodal debug Step 1: dump path counters after LOAD phase
    // on primary. Distinguishes LOAD-time activity from TRANS-time.
    fusee::fusee_path_counters_dump(stdout, host_id, "after_LOAD");
  } else {
    // Children: wait for own host's primary load-done bit.
    uint64_t my_bit = (host_id == 0) ? 0x10ULL : 0x20ULL;
    while (true) {
      flush_line(&hdr->init_done); full_fence();
      if ((CACHELINE_LOAD(&hdr->init_done) & my_bit) != 0) break;
      __builtin_ia32_pause();
    }
  }

  const int global_id = host_id * num_threads + client_id;
  const int total_workers = num_hosts * num_threads;

  // ============================================================
  // iter-21A Fig 10 mode — single client × N ops × 4 op types serial
  // ============================================================
  if (is_fig10) {
    // Only client 0 on each host actually runs ops. Others (shouldn't exist
    // when num_threads=1) sit at the end barrier.
    if (host_id == 0 && client_id == 0) {
      const uint64_t N = fig_keys_per_client;
      mkdir("results", 0755);
      auto run_lat_test = [&](const char *op_name, OpKind op_kind,
                              const char *path) {
        // iter-21A: switch to clock_gettime(CLOCK_MONOTONIC) for ns
        // resolution. SEARCH local p50 was sub-µs (cache_pool hit ≈100 ns);
        // gettimeofday rounds to 0 µs and the CDF flat-lines. Dump as ns;
        // plotter divides by 1000.0 for float-µs display.
        std::vector<uint64_t> lat_ns(N, 0);
        std::vector<uint8_t> is_xhost(N, 0);
        uint64_t failed = 0;
        for (uint64_t i = 0; i < N; i++) {
          uint64_t k = i + 1;  // sequential keys 1..N
          is_xhost[i] = (host_of(&st, k) != (uint32_t)host_id) ? 1 : 0;
          struct timespec ta, tb;
          clock_gettime(CLOCK_MONOTONIC, &ta);
          int rc = 0;
          if (op_kind == OP_INSERT) {
            fill_pattern(k, wbuf.data(), value_bytes);
            rc = store.insert(k, wbuf.data(), value_bytes);
          } else if (op_kind == OP_READ) {
            uint32_t got = 0;
            rc = store.search(k, rbuf.data(), (uint32_t)rbuf.size(), &got);
          } else if (op_kind == OP_UPDATE) {
            fill_pattern(k, wbuf.data(), value_bytes);
            rc = store.update(k, wbuf.data(), value_bytes);
          } else {
            rc = store.remove(k);
          }
          clock_gettime(CLOCK_MONOTONIC, &tb);
          lat_ns[i] = (uint64_t)(tb.tv_sec - ta.tv_sec) * 1000000000ULL +
                      (uint64_t)(tb.tv_nsec - ta.tv_nsec);
          if (rc != 0) {
            failed++;
            if (failed <= 10) {
              fprintf(stderr,
                      "fig10: %s FAIL i=%lu key=%lu rc=%d is_xhost=%u\n",
                      op_name, i, k, rc, (unsigned)is_xhost[i]);
            }
          }
        }
        FILE *fp = fopen(path, "w");
        if (fp) {
          for (uint64_t i = 0; i < N; i++) {
            fprintf(fp, "%lu\n", lat_ns[i]);
          }
          fclose(fp);
        }
        // Also write owner/xhost split (per-op type).
        char split_path[512];
        snprintf(split_path, sizeof(split_path),
                 "results/%s_local_lat-Ap.txt", op_name);
        FILE *fp_l = fopen(split_path, "w");
        snprintf(split_path, sizeof(split_path),
                 "results/%s_xhost_lat-Ap.txt", op_name);
        FILE *fp_x = fopen(split_path, "w");
        if (fp_l && fp_x) {
          for (uint64_t i = 0; i < N; i++) {
            FILE *fp_out = is_xhost[i] ? fp_x : fp_l;
            fprintf(fp_out, "%lu\n", lat_ns[i]);
          }
        }
        if (fp_l) fclose(fp_l);
        if (fp_x) fclose(fp_x);
        fprintf(stderr, "fig10: %s done failed=%lu\n", op_name, failed);
      };
      fprintf(stderr, "fig10: starting INSERT/SEARCH/UPDATE/DELETE N=%lu\n", N);
      run_lat_test("insert", OP_INSERT, "results/insert_lat-Ap.txt");
      run_lat_test("search", OP_READ,   "results/search_lat-Ap.txt");
      run_lat_test("update", OP_UPDATE, "results/update_lat-Ap.txt");
      run_lat_test("delete", OP_DELETE, "results/delete_lat-Ap.txt");
    }
    // Cross-host end barrier so g1 doesn't exit before g2 is done writing.
    if (is_host_primary_client) {
      uint64_t end_my_bit = (host_id == 0) ? 0x100ULL : 0x200ULL;
      uint64_t end_peer_bit = (host_id == 0) ? 0x200ULL : 0x100ULL;
      uint64_t cur = CACHELINE_LOAD(&hdr->init_done);
      CACHELINE_STORE(&hdr->init_done, cur | end_my_bit);
      flush_line(&hdr->init_done); store_fence();
      if (num_hosts > 1) {
        while (true) {
          flush_line(&hdr->init_done); full_fence();
          if ((CACHELINE_LOAD(&hdr->init_done) & end_peer_bit) != 0) break;
          __builtin_ia32_pause();
        }
      }
    }
    if (is_primary_client) {
      printf("FIG10 done host=%d N=%lu results_in=results/\n", host_id,
             fig_keys_per_client);
    }
    // Stop receiver threads before falling out of scope — otherwise
    // ~CxlKvStoreA sees joinable std::thread members and triggers
    // std::terminate. Match the YCSB-path teardown at line ~1154.
    store.stop();
    return 0;
  }

  // ============================================================
  // iter-21A Fig 11 mode — multi-client × 4 timed phases
  // ============================================================
  if (is_fig11) {
    fprintf(stderr, "fig11: enter host=%d client=%d global_id=%d K=%lu\n",
            host_id, client_id, global_id, fig_keys_per_client);
    // Cross-host + intra-host fig11 phase barrier. Every worker on every
    // host arrives + spins until ALL workers on BOTH hosts arrive.
    auto fig11_phase_barrier = [&](int phase) {
      cacheline_u64 *my_slot = &hdr->fig11_phase_arrived[phase * 2 + host_id];
      __atomic_fetch_add(&my_slot->value, 1ULL, __ATOMIC_ACQ_REL);
      flush_line(my_slot); store_fence();
      uint64_t target = (uint64_t)num_threads;
      for (int h = 0; h < num_hosts; h++) {
        cacheline_u64 *slot = &hdr->fig11_phase_arrived[phase * 2 + h];
        while (true) {
          flush_line(slot); full_fence();
          if (__atomic_load_n(&slot->value, __ATOMIC_ACQUIRE) >= target) break;
          __builtin_ia32_pause(); __builtin_ia32_pause();
        }
      }
    };

    const uint64_t K   = fig_keys_per_client;
    const uint64_t SLAB = K;
    // Per-worker private key slab: [global_id*K + 1 .. (global_id+1)*K]
    auto mk_load_key = [&](uint64_t i) {
      return (uint64_t)global_id * SLAB + i + 1;
    };
    // Per-worker INSERT stride past pre-load range.
    const uint64_t pre_load_end =
        (uint64_t)num_hosts * (uint64_t)num_threads * SLAB + 1;
    const uint64_t kInsertStride = 4000000ULL;
    auto mk_insert_key = [&](uint64_t cnt) {
      return pre_load_end + (uint64_t)global_id * kInsertStride + cnt + 1;
    };

    // ---- Pre-load (each worker inserts its slab; no timer) ----
    uint64_t preload_failed = 0;
    fprintf(stderr, "fig11: preload start host=%d gid=%d K=%lu\n",
            host_id, global_id, K);
    for (uint64_t i = 0; i < K; i++) {
      uint64_t k = mk_load_key(i);
      fill_pattern(k, wbuf.data(), value_bytes);
      int rc = store.insert(k, wbuf.data(), value_bytes);
      if (rc != 0) preload_failed++;
    }
    fprintf(stderr, "fig11: preload done host=%d gid=%d failed=%lu\n",
            host_id, global_id, preload_failed);

    fig11_phase_barrier(0);
    fprintf(stderr, "fig11: barrier0 done host=%d gid=%d\n", host_id, global_id);

    auto run_phase = [&](OpKind op_kind, int ms, uint64_t cap,
                         int phase_idx) -> uint64_t {
      uint64_t deadline_ns = now_ns() + (uint64_t)ms * 1000000ULL;
      uint64_t cnt = 0;
      while (now_ns() < deadline_ns && (cap == 0 || cnt < cap)) {
        uint64_t k;
        if (op_kind == OP_INSERT) {
          k = mk_insert_key(cnt);
        } else {
          k = mk_load_key(cnt % K);
        }
        int rc = 0;
        if (op_kind == OP_INSERT) {
          fill_pattern(k, wbuf.data(), value_bytes);
          rc = store.insert(k, wbuf.data(), value_bytes);
        } else if (op_kind == OP_READ) {
          uint32_t got = 0;
          rc = store.search(k, rbuf.data(), (uint32_t)rbuf.size(), &got);
        } else if (op_kind == OP_UPDATE) {
          fill_pattern(k, wbuf.data(), value_bytes);
          rc = store.update(k, wbuf.data(), value_bytes);
        } else {  // DELETE
          rc = store.remove(k);
          if (cnt + 1 >= K) break;
        }
        cnt++;
        (void)rc;
      }
      fig11_phase_barrier(phase_idx);
      return cnt;
    };

    setvbuf(stderr, nullptr, _IONBF, 0);
    fprintf(stderr, "fig11: INSERT phase start host=%d gid=%d\n", host_id, global_id); fflush(stderr);
    uint64_t ins_cnt = run_phase(OP_INSERT, fig_ins_ms, 0, 1);
    fprintf(stderr, "fig11: INSERT done host=%d gid=%d cnt=%lu\n", host_id, global_id, ins_cnt); fflush(stderr);
    uint64_t sea_cnt = run_phase(OP_READ,   fig_read_ms, 0, 2);
    fprintf(stderr, "fig11: SEARCH done host=%d gid=%d cnt=%lu\n", host_id, global_id, sea_cnt); fflush(stderr);
    uint64_t upd_cnt = run_phase(OP_UPDATE, fig_upd_ms, 0, 3);
    fprintf(stderr, "fig11: UPDATE done host=%d gid=%d cnt=%lu\n", host_id, global_id, upd_cnt); fflush(stderr);
    uint64_t del_cnt = run_phase(OP_DELETE, fig_del_ms, K, 4);
    fprintf(stderr, "fig11: DELETE done host=%d gid=%d cnt=%lu\n", host_id, global_id, del_cnt); fflush(stderr);

    // Each worker publishes its 4-phase counts to the stats array.
    WorkerStats *me = &stats[host_id * kMaxClients + client_id];
    CACHELINE_STORE(&me->trans_ops, ins_cnt + sea_cnt + upd_cnt + del_cnt);
    CACHELINE_STORE(&me->w_count, ins_cnt);
    CACHELINE_STORE(&me->w_sum_ns, sea_cnt);
    CACHELINE_STORE(&me->r_count, upd_cnt);
    CACHELINE_STORE(&me->r_sum_ns, del_cnt);
    flush_line(&me->trans_ops);
    flush_line(&me->w_count);
    flush_line(&me->w_sum_ns);
    flush_line(&me->r_count);
    flush_line(&me->r_sum_ns);
    store_fence();
    CACHELINE_STORE(&me->done, 1);
    flush_line(&me->done); store_fence();

    // Primary client aggregates + prints SUMMARY line.
    if (is_primary_client) {
      // Wait for all workers (both hosts) to be done.
      for (int h = 0; h < num_hosts; h++) {
        for (int c = 0; c < num_threads; c++) {
          WorkerStats *w = &stats[h * kMaxClients + c];
          while (true) {
            flush_line(&w->done); full_fence();
            if (CACHELINE_LOAD(&w->done) != 0) break;
            __builtin_ia32_pause();
          }
        }
      }
      uint64_t tot_ins = 0, tot_sea = 0, tot_upd = 0, tot_del = 0;
      for (int h = 0; h < num_hosts; h++) {
        for (int c = 0; c < num_threads; c++) {
          WorkerStats *w = &stats[h * kMaxClients + c];
          tot_ins += CACHELINE_LOAD(&w->w_count);
          tot_sea += CACHELINE_LOAD(&w->w_sum_ns);
          tot_upd += CACHELINE_LOAD(&w->r_count);
          tot_del += CACHELINE_LOAD(&w->r_sum_ns);
        }
      }
      uint64_t ins_tpt = tot_ins * 1000ULL / (uint64_t)fig_ins_ms;
      uint64_t sea_tpt = tot_sea * 1000ULL / (uint64_t)fig_read_ms;
      uint64_t upd_tpt = tot_upd * 1000ULL / (uint64_t)fig_upd_ms;
      uint64_t del_tpt = tot_del * 1000ULL / (uint64_t)fig_del_ms;
      fprintf(stderr,
             "SUMMARY A_micro_tpt host=%d num_hosts=%d num_clients_per_host=%d "
             "total_clients=%d insert_tpt=%lu search_tpt=%lu update_tpt=%lu "
             "delete_tpt=%lu # micro_optA_h%d_c%d\n",
             host_id, num_hosts, num_threads, num_hosts * num_threads,
             ins_tpt, sea_tpt, upd_tpt, del_tpt, host_id, num_threads);
      fflush(stderr);
    }
    // See Fig 10 path: must stop receiver threads before scope exit.
    store.stop();
    return 0;
  }

  // -------- TRANS phase --------

  std::vector<uint64_t> w_lat, r_lat;
  w_lat.reserve(trans_ops.size() / total_workers + 16);
  r_lat.reserve(trans_ops.size() / total_workers + 16);

  // iter-19A Phase 1c H11: pre-slice trans_ops to this worker's ops.
  // Prior loop walked the full 5M-entry vector per worker, contributing
  // 80 MB of L3 pressure × 64 workers = 5 GB working set competing with
  // the 9 GB cache_pool, causing L3 thrash → cache_pool entries refetched
  // from DRAM per op (~1.7 µs/op overhead at c100). Pre-slicing reduces
  // per-worker walk footprint from 80 MB to ~624 KB (39 k × 16 B), which
  // fits comfortably in L2. Gated by FUSEE_TRANS_PRESLICE (default 1) so
  // we can A/B the change.
  std::vector<Op> my_ops;
  bool preslice = true;
  if (const char *e = getenv("FUSEE_TRANS_PRESLICE"); e && atoi(e) == 0) {
    preslice = false;
  }
  if (preslice) {
    my_ops.reserve(trans_ops.size() / total_workers + 16);
    for (size_t i = 0; i < trans_ops.size(); i++) {
      if ((int)(i % (size_t)total_workers) == global_id) my_ops.push_back(trans_ops[i]);
    }
  }

  uint64_t t_start = now_ns();
  uint64_t my_count = 0;
  // iter-11A Phase 0 bimodal probe: capture (a) wall-ns from t_start to first
  // successful op completion, and (b) duration of first successful op.
  uint64_t first_op_b = 0;
  uint64_t first_op_dur_ns = 0;
  // Two loop variants — full walk (legacy, default off) vs pre-sliced.
  const size_t loop_end = preslice ? my_ops.size() : trans_ops.size();
  // iter-19A Phase 1d H15: skip per-op now_ns + r_lat.push_back when
  // FUSEE_NO_LAT=1. Used to attribute the L1 miss gap to worker-loop
  // infrastructure (timing + latency record) vs the search() call.
  bool no_lat = false;
  if (const char *e = getenv("FUSEE_NO_LAT"); e && atoi(e) != 0) no_lat = true;
  for (size_t i = 0; i < loop_end; i++) {
    const Op *op_ptr;
    if (preslice) {
      op_ptr = &my_ops[i];
    } else {
      if ((int)(i % (size_t)total_workers) != global_id) continue;
      op_ptr = &trans_ops[i];
    }
    const Op &op = *op_ptr;
    uint64_t a, b;
    if (!no_lat) a = now_ns();
    else a = 0;
    int rc;
    uint32_t got_len = 0;
    if (op.kind == OP_READ) {
      rc = store.search(op.key, rbuf.data(), (uint32_t)rbuf.size(), &got_len);
      if (!no_lat) { b = now_ns(); if (rc == 0) r_lat.push_back(b - a); }
      else b = 0;
    } else if (op.kind == OP_UPDATE) {
      fill_pattern(op.key, wbuf.data(), value_bytes);
      rc = store.update(op.key, wbuf.data(), value_bytes);
      if (!no_lat) { b = now_ns(); if (rc == 0) w_lat.push_back(b - a); }
      else b = 0;
    } else if (op.kind == OP_INSERT) {
      fill_pattern(op.key, wbuf.data(), value_bytes);
      rc = store.insert(op.key, wbuf.data(), value_bytes);
      if (!no_lat) { b = now_ns(); if (rc == 0) w_lat.push_back(b - a); }
      else b = 0;
    } else if (op.kind == OP_DELETE) {
      rc = store.remove(op.key);
      if (!no_lat) { b = now_ns(); if (rc == 0) w_lat.push_back(b - a); }
      else b = 0;
    } else {
      continue;
    }
    if (rc == 0 && first_op_b == 0) {
      first_op_b = b;
      first_op_dur_ns = b - a;
    }
    (void)rc;
    my_count++;
  }
  uint64_t t_end = now_ns();
  uint64_t my_wall_ns = t_end - t_start;
  uint64_t ops_to_first_ns = first_op_b ? (first_op_b - t_start) : 0;

  // Publish stats.
  WorkerStats *me = &stats[host_id * kMaxClients + client_id];
  uint64_t w_count = w_lat.size();
  uint64_t r_count = r_lat.size();
  uint64_t w_sum_ns = 0; for (auto x : w_lat) w_sum_ns += x;
  uint64_t r_sum_ns = 0; for (auto x : r_lat) r_sum_ns += x;
  uint64_t w_p50 = quantile_ns(w_lat, 0.50);
  uint64_t w_p99 = quantile_ns(w_lat, 0.99);
  uint64_t r_p50 = quantile_ns(r_lat, 0.50);
  uint64_t r_p99 = quantile_ns(r_lat, 0.99);
  CACHELINE_STORE(&me->trans_ops, my_count);
  CACHELINE_STORE(&me->trans_wall_ns, my_wall_ns);
  CACHELINE_STORE(&me->w_count, w_count);
  CACHELINE_STORE(&me->w_sum_ns, w_sum_ns);
  CACHELINE_STORE(&me->w_p50_ns, w_p50);
  CACHELINE_STORE(&me->w_p99_ns, w_p99);
  CACHELINE_STORE(&me->r_count, r_count);
  CACHELINE_STORE(&me->r_sum_ns, r_sum_ns);
  CACHELINE_STORE(&me->r_p50_ns, r_p50);
  CACHELINE_STORE(&me->r_p99_ns, r_p99);
  CACHELINE_STORE(&me->first_op_ns, first_op_dur_ns);
  CACHELINE_STORE(&me->ops_to_first_ms, ops_to_first_ns);
  CACHELINE_STORE(&me->done, 1ULL);
  flush_line(me); store_fence();

  if (client_id != 0) {
    probe_flush();
    _exit(0);
  }
  probe_flush();
  for (auto p : children) waitpid(p, nullptr, 0);

  // Cross-host barrier 3: aggregation. Skipped when num_hosts == 1.
  if (is_host_primary_client && num_hosts > 1) {
    uint64_t my_bit = (host_id == 0) ? 0x40ULL : 0x80ULL;
    uint64_t peer_bit = (host_id == 0) ? 0x80ULL : 0x40ULL;
    uint64_t cur = CACHELINE_LOAD(&hdr->init_done);
    CACHELINE_STORE(&hdr->init_done, cur | my_bit);
    flush_line(&hdr->init_done); store_fence();
    while (true) {
      flush_line(&hdr->init_done); full_fence();
      if ((CACHELINE_LOAD(&hdr->init_done) & peer_bit) != 0) break;
      __builtin_ia32_pause();
    }
  }

  // iter-9A C5 hash-diff (Phase 1+2 verification): optional bucket-array
  // dump for cross-host byte comparison. Only after cross-host barrier 3
  // synced both hosts so all writes are CXL-visible. Slot values encode
  // owner-host blk_off; under §I9 strict-A linearizability, both hosts'
  // CXL view of the bucket array MUST be byte-identical after barrier 3.
  if (is_host_primary_client) {
    const char *dump_path = getenv("FUSEE_FINAL_STATE_DUMP");
    if (dump_path && dump_path[0]) {
      for (uint32_t b = 0; b < num_buckets; b++) {
        flush_line(&buckets[b]);
        flush_line(reinterpret_cast<char *>(&buckets[b]) + 64);
      }
      full_fence();
      FILE *fp = fopen(dump_path, "wb");
      if (fp) {
        size_t bytes = sizeof(CxlKvBucket) * (size_t)num_buckets;
        fwrite(buckets, 1, bytes, fp);
        fclose(fp);
        fprintf(stderr,
                "[h%d] FUSEE_FINAL_STATE_DUMP wrote %zu bytes to %s\n",
                host_id, bytes, dump_path);
      } else {
        fprintf(stderr,
                "[h%d] FUSEE_FINAL_STATE_DUMP fopen %s failed: %s\n",
                host_id, dump_path, strerror(errno));
      }
    }
  }

  // iter-9A redo Phase 2.D: stop all three named receivers
  // (WriteReceiver, ReadReceiver, InvalReceiver). The legacy
  // stop_responder/stop_dispatcher pair only joined two threads —
  // the new ReadReceiver would leak and trigger terminate() on
  // CxlKvStoreA destruction.
  store.stop();

  if (is_primary_client) {
    flush_region(stats, stats_bytes); full_fence();
    uint64_t total_trans_ops = 0, max_wall_ns = 0;
    uint64_t total_w_count = 0, total_w_sum_ns = 0;
    uint64_t total_r_count = 0, total_r_sum_ns = 0;
    // iter-11A Phase 0 bimodal probe aggregates
    uint64_t max_first_op_ns = 0, max_ops_to_first_ns = 0;
    uint64_t sum_first_op_ns = 0, sum_ops_to_first_ns = 0;
    int seen_workers = 0;
    std::vector<uint64_t> all_w_p50, all_w_p99, all_r_p50, all_r_p99;
    for (int h = 0; h < num_hosts; h++) {
      for (int c = 0; c < num_threads; c++) {
        WorkerStats *ws = &stats[h * kMaxClients + c];
        total_trans_ops += CACHELINE_LOAD(&ws->trans_ops);
        uint64_t w = CACHELINE_LOAD(&ws->trans_wall_ns);
        if (w > max_wall_ns) max_wall_ns = w;
        total_w_count += CACHELINE_LOAD(&ws->w_count);
        total_w_sum_ns += CACHELINE_LOAD(&ws->w_sum_ns);
        total_r_count += CACHELINE_LOAD(&ws->r_count);
        total_r_sum_ns += CACHELINE_LOAD(&ws->r_sum_ns);
        all_w_p50.push_back(CACHELINE_LOAD(&ws->w_p50_ns));
        all_w_p99.push_back(CACHELINE_LOAD(&ws->w_p99_ns));
        all_r_p50.push_back(CACHELINE_LOAD(&ws->r_p50_ns));
        all_r_p99.push_back(CACHELINE_LOAD(&ws->r_p99_ns));
        uint64_t fo = CACHELINE_LOAD(&ws->first_op_ns);
        uint64_t of = CACHELINE_LOAD(&ws->ops_to_first_ms);
        if (fo > max_first_op_ns) max_first_op_ns = fo;
        if (of > max_ops_to_first_ns) max_ops_to_first_ns = of;
        sum_first_op_ns += fo;
        sum_ops_to_first_ns += of;
        seen_workers++;
      }
    }
    uint64_t avg_first_op_ns = seen_workers ? (sum_first_op_ns / seen_workers) : 0;
    uint64_t avg_ops_to_first_ns = seen_workers ? (sum_ops_to_first_ns / seen_workers) : 0;
    auto agg_p50 = [](std::vector<uint64_t> &v) {
      std::sort(v.begin(), v.end());
      return v.empty() ? 0 : v[v.size() / 2];
    };
    auto agg_p99 = [](std::vector<uint64_t> &v) {
      uint64_t m = 0; for (auto x : v) if (x > m) m = x; return m;
    };
    uint64_t w_avg = total_w_count ? (total_w_sum_ns / total_w_count) : 0;
    uint64_t r_avg = total_r_count ? (total_r_sum_ns / total_r_count) : 0;
    uint64_t w_p50_a = agg_p50(all_w_p50);
    uint64_t w_p99_a = agg_p99(all_w_p99);
    uint64_t r_p50_a = agg_p50(all_r_p50);
    uint64_t r_p99_a = agg_p99(all_r_p99);
    double trans_wall_s = max_wall_ns / 1e9;
    uint64_t trans_agg_kops = max_wall_ns ?
        (total_trans_ops * 1000000ULL / (max_wall_ns / 1000ULL + 1)) : 0;

    fprintf(stdout,
        "YCSB opt=A cache=%d num_hosts=%d threads=%d threads_eff=%d rep=%d "
        "load_ops=%zu load_thpt=%lu "
        "trans_ops=%lu trans_wall_max=%.6f trans_agg_thpt=%lu "
        "w_avg_ns=%lu w_p50_ns=%lu w_p99_ns=%lu "
        "r_avg_ns=%lu r_p50_ns=%lu r_p99_ns=%lu "
        "first_op_ns_max=%lu first_op_ns_avg=%lu "
        "ops_to_first_ns_max=%lu ops_to_first_ns_avg=%lu "
        "# %s_optA_t%d_cache%s_rep%d\n",
        cache_on ? 1 : 0, num_hosts, num_threads, num_threads, rep,
        load_ops.size(), load_thpt_kops,
        total_trans_ops, trans_wall_s, trans_agg_kops,
        w_avg, w_p50_a, w_p99_a,
        r_avg, r_p50_a, r_p99_a,
        max_first_op_ns, avg_first_op_ns,
        max_ops_to_first_ns, avg_ops_to_first_ns,
        wl_name.c_str(), num_threads, cache_on ? "on" : "off", rep);
    fflush(stdout);
  }

  // iter-15A Layer A: dump path counters (no-op if FUSEE_PATH_COUNTERS=0)
  // Step 1 bimodal debug: this dump is "after_TRANS"; the earlier dump
  // post-LOAD is "after_LOAD". Delta = TRANS-only contribution per counter.
  fusee::fusee_path_counters_dump(stdout, host_id, "after_TRANS");

  // iter-15A microbench C.3: cache_pool fill_level_end. Scan all buckets,
  // count non-empty + non-stale entries. Dump on host_id 0 only (cache_pool
  // is per-host but layout is symmetric — counted on primary).
  if (host_id == 0) {
    uint64_t filled = 0, stale = 0, tomb = 0;
    uint64_t total = (uint64_t)cache_buckets * fusee::kCacheEntriesPerBucket;
    for (uint32_t b = 0; b < cache_buckets; b++) {
      auto *bk = &cache.buckets[b];
      for (int i = 0; i < fusee::kCacheEntriesPerBucket; i++) {
        auto *e = &bk->entries[i];
        uint64_t k = e->key.load(std::memory_order_relaxed);
        uint8_t  s = e->stale.load(std::memory_order_relaxed);
        if (k == fusee::kCacheKeyEmpty) continue;
        if (k == fusee::kCacheKeyTomb)  { tomb++; continue; }
        if (s) stale++;
        filled++;
      }
    }
    fprintf(stdout,
      "# CACHE_FILL host=%d cache_buckets=%u capacity=%lu filled=%lu stale=%lu tomb=%lu fill_pct=%.3f\n",
      host_id, cache_buckets, total, filled, stale, tomb,
      (double)filled * 100.0 / (double)total);
    fflush(stdout);
  }

  cxl_region_destroy(&r);
  return 0;
}
