// YCSB workload runner with fork-based client scaling.
//
// Client model: each "client" is a forked process with its own CxlKvStore
// instance and unique LFM slot. This matches the FUSEE concept of "client
// threads" (separate execution contexts operating on the shared hash table)
// without the intra-process serialization a pthread_mutex would impose.
//
// Layouts
// -------
//   FUSEE_NUM_HOSTS=H        cross-host (machines) count, 1..4
//   FUSEE_HOST_ID=h          0..H-1 for THIS invocation
//   FUSEE_NUM_THREADS=N      clients per host (we still call the env
//                            "THREADS" for compat; implementation is fork)
//   FUSEE_RUN_COOKIE=...     unique u64, orchestrator-supplied, required
//                            when H > 1
//   FUSEE_CACHE=1            enable DRAM cache
//
// Each client gets:
//   global_id = h * N + client_index       (0 .. H*N-1, used as LFM id)
//   total_workers = H * N
// and executes trans_ops[k] where k % total_workers == global_id.
//
// Only host 0's clients run the LOAD phase, partitioned similarly across
// their N clients (i % N == client_index).
//
// Output (host 0 primary client prints a single line):
//   YCSB opt=X cache=C num_hosts=H threads=N load_ops=... load_thpt=...
//        trans_ops=... trans_wall_max=...s trans_agg_thpt=...
//        w_avg_ns=... w_p50_ns=... w_p99_ns=...
//        r_avg_ns=... r_p50_ns=... r_p99_ns=...

#include "cxl_kv_store.h"
#include "cxl_mm.h"
#include "cxl_hashtable.h"
#include "cxl_same_host_queue.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include "common.h"
}

namespace {

constexpr int kMaxClientsPerHost = 128;
constexpr int kMaxHostsLoc = 4;
constexpr size_t kYcsbStatsOffsetFromEnd = 2UL * 1024 * 1024;  // 2 MB

struct YcsbWorker {
  cacheline_u64 started;
  cacheline_u64 done;
  cacheline_u64 ops;
  cacheline_u64 wall_ns;
  cacheline_u64 w_count;
  cacheline_u64 w_sum_ns;
  cacheline_u64 w_p50_ns;
  cacheline_u64 w_p99_ns;
  cacheline_u64 r_count;
  cacheline_u64 r_sum_ns;
  cacheline_u64 r_p50_ns;
  cacheline_u64 r_p99_ns;
};

struct YcsbHostRow {
  cacheline_u64 load_done_all;
  YcsbWorker workers[kMaxClientsPerHost];
};

struct YcsbShared {
  cacheline_u64 run_cookie;
  cacheline_u64 init_done;
  cacheline_u64 trans_go;
  YcsbHostRow hosts[kMaxHostsLoc];
};

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

enum OpKind { OP_INSERT, OP_READ, OP_UPDATE, OP_DELETE, OP_SKIP };
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
  if (!f) { fprintf(stderr, "open %s: %s\n", path.c_str(), strerror(errno)); return out; }
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
    while (!key_tok.empty() && (key_tok.back()==' '||key_tok.back()=='\t'
          ||key_tok.back()=='\n'||key_tok.back()=='\r')) key_tok.pop_back();
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

} // namespace

using fusee::CxlKvStore;
using fusee::CXLRegion;
using fusee::cxl_region_destroy;
using fusee::cxl_region_init;
using fusee::kConsensusOpt;

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr,
      "usage: %s <dev> <load_file> <trans_file> [num_buckets] [max_ops_cap]\n",
      argv[0]);
    return 2;
  }
  const char *dev = argv[1];
  std::string load_path = argv[2];
  std::string trans_path = argv[3];
  uint32_t num_buckets =
      (argc >= 5) ? (uint32_t)strtoul(argv[4], nullptr, 0) : 65536U;
  size_t max_ops =
      (argc >= 6) ? (size_t)strtoull(argv[5], nullptr, 0) : 0;

  // Env.
  int num_hosts = 1, host_id = 0, num_clients = 1;
  uint64_t run_cookie = 0;
  {
    const char *nh = getenv("FUSEE_NUM_HOSTS");
    const char *hid = getenv("FUSEE_HOST_ID");
    const char *nc = getenv("FUSEE_NUM_THREADS"); // semantic: clients
    const char *rc = getenv("FUSEE_RUN_COOKIE");
    if (nh && nh[0]) num_hosts = atoi(nh);
    if (hid && hid[0]) host_id = atoi(hid);
    if (nc && nc[0]) num_clients = atoi(nc);
    if (rc && rc[0]) run_cookie = strtoull(rc, nullptr, 0);
    if (num_hosts < 1 || num_hosts > kMaxHostsLoc ||
        host_id < 0 || host_id >= num_hosts) {
      fprintf(stderr, "bad FUSEE_NUM_HOSTS=%d FUSEE_HOST_ID=%d\n",
              num_hosts, host_id);
      return 2;
    }
    if (num_clients < 1 || num_clients > kMaxClientsPerHost) {
      fprintf(stderr, "bad FUSEE_NUM_THREADS=%d\n", num_clients);
      return 2;
    }
    if (num_hosts > 1 && run_cookie == 0) {
      fprintf(stderr, "role-mode requires FUSEE_RUN_COOKIE\n");
      return 2;
    }
  }

  // Options A and B keep per-host replication state (PendingRing matrix,
  // one replicator per process). Intra-host client scaling is unsafe for
  // WRITES (multiple clients push to same ring, multiple replicators race
  // on head cursor), but the SEARCH path is lock-free and purely local,
  // so pure-read workloads (workloadc) are safe. For the main sweep we
  // clamp to 1 to avoid any corruption risk on mixed workloads; set
  // FUSEE_UNSAFE_UNCLAMP=1 to override (e.g. for pure-read A/B scaling
  // validation). Behavior unchanged for opt=C (never clamped).
  // Phase 4 (2026-04-22): PendingRingMatrix widened to
  // rings[kMaxWorkers=200][kMaxWorkers=200] so A/B can have per-client
  // rings. The old A/B clamp is no longer needed — every global_id is a
  // valid ring src/dst. Keep FUSEE_READ_ONLY=1 for the Phase-1 read-only
  // mode and FUSEE_UNSAFE_UNCLAMP for debugging; neither is required now.
  const int requested_clients = num_clients;
  (void)requested_clients;
#if CONSENSUS_OPT != FUSEE_OPT_C
  {
    const char *ro = getenv("FUSEE_READ_ONLY");
    if (ro && ro[0] == '1' && num_clients > 1) {
      fprintf(stderr,
        "[h%d] opt %c: FUSEE_READ_ONLY=1, %d clients/host, non-primary "
        "clients attach read-only.\n", host_id, kConsensusOpt, num_clients);
    }
  }
#endif
  const int total_workers = num_hosts * num_clients;
  const bool role_mode = (num_hosts > 1);

  std::vector<Op> load_ops_v = load_ops_from_file(load_path);
  std::vector<Op> trans_ops_v = load_ops_from_file(trans_path);
  if (max_ops > 0) {
    if (load_ops_v.size() > max_ops) load_ops_v.resize(max_ops);
    if (trans_ops_v.size() > max_ops) trans_ops_v.resize(max_ops);
  }
  if (load_ops_v.empty() && trans_ops_v.empty()) {
    fprintf(stderr, "no valid ops parsed\n"); return 1;
  }

  // Region sizing.
  size_t store_bytes = CxlKvStore::bytes_for(num_buckets);
  size_t needed_total = store_bytes + kYcsbStatsOffsetFromEnd;
  size_t needed = ((needed_total + fusee::kCxlDevdaxAlign - 1) /
                   fusee::kCxlDevdaxAlign) * fusee::kCxlDevdaxAlign;

  // ========================================================================
  // Parent (per-host): open region, memset stats, fork num_clients-1
  // children BEFORE any further setup. Children inherit the zeroed mmap.
  // This is the same fix applied to cxl_kv_bench_mp (see
  // docs/g34_bench/g34_livelock_root_cause.md).
  // ========================================================================
  CXLRegion r{};
  if (cxl_region_init(&r, dev, needed) < 0) {
    fprintf(stderr, "cxl_region_init failed\n"); return 1;
  }
  YcsbShared *shared = reinterpret_cast<YcsbShared *>(
      reinterpret_cast<char *>(r.base) + r.size - kYcsbStatsOffsetFromEnd);
  bool is_host_primary = (host_id == 0);
  const bool trace = (getenv("FUSEE_TRACE") && getenv("FUSEE_TRACE")[0]=='1');
#define T(fmt, ...) do { if (trace) { fprintf(stderr, "[h%d c%d t=%.3f] " fmt "\n", host_id, -1, (double)now_ns()/1e9, ##__VA_ARGS__); fflush(stderr); } } while (0)
#define TC(fmt, ...) do { if (trace) { fprintf(stderr, "[h%d c%d t=%.3f] " fmt "\n", host_id, client_id, (double)now_ns()/1e9, ##__VA_ARGS__); fflush(stderr); } } while (0)
  T("start num_hosts=%d num_clients=%d", num_hosts, num_clients);
  if (is_host_primary) {
    T("primary memsetting shared");
    std::memset(shared, 0, sizeof(*shared));
    flush_region(shared, sizeof(*shared));
    store_fence();
  }

  // Phase-3 micro-batching: per-host DRAM ring (MAP_SHARED|MAP_ANONYMOUS
  // pre-fork so all children inherit the address). Active only when
  // FUSEE_BATCH_K > 0 and we are on protocol C.
#if CONSENSUS_OPT == FUSEE_OPT_C
  void *batch_shm = nullptr;
  size_t batch_shm_bytes = 0;
  uint32_t batch_K = 0;
  uint32_t batch_T_us = 100;  // default 100 µs per plan §3.7 starting point
  {
    const char *bk_env = getenv("FUSEE_BATCH_K");
    if (bk_env && bk_env[0]) batch_K = (uint32_t)atoi(bk_env);
    const char *bt_env = getenv("FUSEE_BATCH_T_US");
    if (bt_env && bt_env[0]) batch_T_us = (uint32_t)atoi(bt_env);
    if (batch_K > 0) {
      batch_shm_bytes = fusee::MicroBatchRing::bytes_for(num_buckets, batch_K);
      void *mm = mmap(nullptr, batch_shm_bytes, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
      if (mm == MAP_FAILED) {
        fprintf(stderr, "mmap MicroBatchRing (%zu bytes, K=%u) failed: %s\n",
                batch_shm_bytes, batch_K, strerror(errno));
        return 1;
      }
      batch_shm = mm;
    }
  }
#endif

  // Same-host DRAM bypass (2a): allocate a MAP_SHARED anonymous region
  // sized for the per-host DramInvalMatrix BEFORE fork so all children
  // share the same virtual address. Toggle off via FUSEE_SAME_HOST_BYPASS=0.
  fusee::DramInvalMatrix *dram_mat = nullptr;
  size_t dram_mat_bytes = 0;
  {
    const char *bp_env = getenv("FUSEE_SAME_HOST_BYPASS");
    bool bypass_enabled = !(bp_env && bp_env[0] == '0');
    if (bypass_enabled && num_clients > 1) {
      dram_mat_bytes = fusee::dram_inval_matrix_bytes();
      void *mm = mmap(nullptr, dram_mat_bytes, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
      if (mm == MAP_FAILED) {
        fprintf(stderr, "mmap DramInvalMatrix (%zu bytes) failed: %s\n",
                dram_mat_bytes, strerror(errno));
        return 1;
      }
      // Zero; children will see all-zero entries.
      std::memset(mm, 0, dram_mat_bytes);
      dram_mat = reinterpret_cast<fusee::DramInvalMatrix *>(mm);
    }
  }

  // Fork num_clients-1 children. Parent has client_id=0.
  std::vector<pid_t> children;
  int client_id = 0;
  for (int i = 1; i < num_clients; i++) {
    pid_t p = fork();
    if (p < 0) { perror("fork"); return 1; }
    if (p == 0) { client_id = i; children.clear(); break; }
    children.push_back(p);
  }

  const int global_id = host_id * num_clients + client_id;
  const bool is_primary_client = (host_id == 0) && (client_id == 0);

  // Phase 1 read-only path (A/B pure-read workloads):
  // FUSEE_READ_ONLY=1 → non-primary fork children attach read-only,
  // skipping the replicator thread and trip-wiring on any write.
  // Phase 4 made the clamp-around unnecessary for correctness because
  // PendingRingMatrix is wide enough for every client to have its own
  // src/dst pair; we keep READ_ONLY as an explicit opt-in for the
  // pure-read sub-sweep that still wants to save the replicator cost.
  const char *ro_env = getenv("FUSEE_READ_ONLY");
  const bool read_only_mode = (ro_env && ro_env[0] == '1');

  // All three protocols now attach with (global_id, total_workers):
  // unique LFM slot per client AND (Phase 4) unique ring src/dst per
  // client for A/B.
  const int attach_id = global_id;
  const int attach_n  = total_workers;

  // Cross-host init ordering: host 0 client 0 publishes init_done + cookie
  // AFTER its KV-store attach+init; other hosts wait on cookie.
  TC("after fork global_id=%d total_workers=%d", global_id, total_workers);
  CxlKvStore store;
  size_t kv_bytes = r.size - kYcsbStatsOffsetFromEnd;
  if (is_primary_client) {
    // Primary client attaches with init_region=true and total_workers as the
    // LFM num_hosts so every client gets its own LFM slot.
    // Primary always attaches write-capable (it runs the LOAD phase).
    if (store.attach(r.base, kv_bytes, num_buckets, attach_id, attach_n,
                     /*init_region=*/true, /*read_only=*/false) != 0) {
      fprintf(stderr, "[h%d c%d] attach failed\n", host_id, client_id);
      cxl_region_destroy(&r); return 1;
    }
    // init_done must be set even in single-host mode if num_clients > 1,
    // because same-host fork children also wait on it. Cookie is only
    // needed for cross-host runs (to defeat stale CXL memory).
    if (num_clients > 1 || role_mode) {
      CACHELINE_STORE(&shared->init_done, 1ULL);
    }
    if (role_mode) {
      CACHELINE_STORE(&shared->run_cookie, run_cookie);
    }
    TC("primary attach + init_done + cookie=%lu region=%zu shared_off=%zu",
       run_cookie, r.size, r.size - kYcsbStatsOffsetFromEnd);
  } else {
    // Non-primary clients wait for the primary client's init_done + cookie
    // to match. (Applies to both same-host fork children AND cross-host.)
    TC("non-primary pre-wait region size=%zu shared_off=%zu expected_cookie=%lu",
       r.size, r.size - kYcsbStatsOffsetFromEnd, run_cookie);
    if (role_mode) {
      while (CACHELINE_LOAD(&shared->run_cookie) != run_cookie)
        __builtin_ia32_pause();
      TC("saw cookie");
    }
    while (CACHELINE_LOAD(&shared->init_done) == 0)
      __builtin_ia32_pause();
    TC("saw init_done");
    // Read-only mode: client_id==0 on each host still attaches write-capable
    // so it can run its local replicator (host 0 c0 also does load). Fork
    // children client_id>0 attach read-only — they skip the replicator and
    // trip on any write attempt.
    const bool this_client_read_only = (read_only_mode && client_id > 0);
    if (store.attach(r.base, kv_bytes, num_buckets, attach_id, attach_n,
                     /*init_region=*/false,
                     /*read_only=*/this_client_read_only) != 0) {
      fprintf(stderr, "[h%d c%d] attach failed\n", host_id, client_id);
      cxl_region_destroy(&r);
      if (client_id > 0) _exit(1);
      return 1;
    }
  }
  bool cache_on = getenv("FUSEE_CACHE") && getenv("FUSEE_CACHE")[0] == '1';
  if (cache_on) store.enable_dram_cache(true);

#if CONSENSUS_OPT == FUSEE_OPT_C
  // Enable batching on every client (writers all use the same ring); start
  // the flusher on the host's primary client only. init_region=true on the
  // local host's primary, false elsewhere.
  if (batch_shm && batch_K > 0) {
    bool is_host_primary_client = (client_id == 0);
    if (store.enable_batching(batch_shm, batch_shm_bytes, batch_K, batch_T_us,
                              /*init_region=*/is_host_primary_client) != 0) {
      fprintf(stderr, "[h%d c%d] enable_batching failed\n", host_id, client_id);
      return 1;
    }
    if (is_host_primary_client) {
      int num_flushers = 1;
      const char *nfe = getenv("FUSEE_BATCH_NUM_FLUSHERS");
      if (nfe && nfe[0]) num_flushers = atoi(nfe);
      if (num_flushers < 1) num_flushers = 1;
      store.start_flusher(num_flushers);
    }
  }
#endif

  // Same-host bypass wire-up (2a). Only meaningful for A/B (C has no
  // replicator, never pushes to rings). The C attach silently ignores
  // unused API parts.
#if CONSENSUS_OPT != FUSEE_OPT_C
  if (dram_mat && num_clients > 1) {
    store.enable_same_host_bypass(dram_mat, num_clients);
  }
#else
  (void)dram_mat;
#endif

  // Helper: run one op, return latency in ns.
  auto do_op = [&](const Op &o, uint64_t *dt_out) {
    uint64_t v = o.key ^ 0xCAFEBABEULL;
    uint64_t out = 0;
    int rc = 0;
    uint64_t t0 = now_ns();
    switch (o.kind) {
      case OP_INSERT: rc = store.insert(o.key, v); break;
      case OP_UPDATE: rc = store.update(o.key, v); break;
      case OP_DELETE: rc = store.remove(o.key); break;
      case OP_READ:   rc = store.search(o.key, &out); break;
      default: break;
    }
    if (dt_out) *dt_out = now_ns() - t0;
    return rc;
  };

  // LOAD (only host 0's PRIMARY CLIENT does inserts). Originally host 0's
  // clients partitioned the load phase round-robin, but with unclamp that
  // produces concurrent inserts from multiple host-0 clients into the
  // same PendingRing → race. Safer to keep load single-threaded on
  // client 0; the load throughput is then a separate measurement anyway.
  uint64_t t_load_start = 0;
  if (is_primary_client) {
    t_load_start = now_ns();
    for (const Op &o : load_ops_v) {
      uint64_t dt; do_op(o, &dt);
    }
  }
  YcsbWorker *my_row = &shared->hosts[host_id].workers[client_id];

  // Signal "started" (arrival at trans barrier).
  TC("load done (if primary); setting started=1");
  CACHELINE_STORE(&my_row->started, 1ULL);

  // Primary client waits for all 2N workers to reach barrier; publishes trans_go.
  if (is_primary_client) {
    for (int h = 0; h < num_hosts; h++) {
      for (int c = 0; c < num_clients; c++) {
        while (CACHELINE_LOAD(&shared->hosts[h].workers[c].started) == 0)
          __builtin_ia32_pause();
      }
    }
    CACHELINE_STORE(&shared->hosts[0].load_done_all, 1ULL);
    CACHELINE_STORE(&shared->trans_go, 1ULL);
    TC("primary published trans_go");
  } else {
    TC("waiting for trans_go");
    while (CACHELINE_LOAD(&shared->trans_go) == 0) __builtin_ia32_pause();
    TC("saw trans_go");
  }

  // TRANS: each client executes trans[i] where i % total_workers == global_id.
  std::vector<uint64_t> wlat, rlat;
  size_t NT = trans_ops_v.size();
  wlat.reserve(NT / total_workers + 16);
  rlat.reserve(NT / total_workers + 16);

  uint64_t t_start = now_ns();
  uint64_t my_ops = 0;
  for (size_t i = (size_t)global_id; i < NT; i += (size_t)total_workers) {
    uint64_t dt;
    const Op &o = trans_ops_v[i];
    do_op(o, &dt);
    if (o.kind == OP_READ) rlat.push_back(dt);
    else                   wlat.push_back(dt);
    my_ops++;
  }
  uint64_t t_end = now_ns();

  // Publish per-client stats.
  uint64_t w_sum = 0; for (auto x : wlat) w_sum += x;
  uint64_t r_sum = 0; for (auto x : rlat) r_sum += x;
  CACHELINE_STORE(&my_row->ops, my_ops);
  CACHELINE_STORE(&my_row->wall_ns, t_end - t_start);
  CACHELINE_STORE(&my_row->w_count, (uint64_t)wlat.size());
  CACHELINE_STORE(&my_row->w_sum_ns, w_sum);
  CACHELINE_STORE(&my_row->w_p50_ns, quantile_ns(wlat, 0.50));
  CACHELINE_STORE(&my_row->w_p99_ns, quantile_ns(wlat, 0.99));
  CACHELINE_STORE(&my_row->r_count, (uint64_t)rlat.size());
  CACHELINE_STORE(&my_row->r_sum_ns, r_sum);
  CACHELINE_STORE(&my_row->r_p50_ns, quantile_ns(rlat, 0.50));
  CACHELINE_STORE(&my_row->r_p99_ns, quantile_ns(rlat, 0.99));
  CACHELINE_STORE(&my_row->done, 1ULL);
  TC("trans done; ops=%lu wall=%.3fs", my_ops, (t_end-t_start)/1e9);

  // Stop replicator threads (safe only after ALL peers finish, to avoid
  // tearing down a ring while a peer writer is still enqueueing).
  // Primary client does the cross-host "all done" wait; children exit cleanly.
  if (!is_primary_client) {
    // Also wait for all workers to be done before store.stop() (Option A
    // replicator must not disappear while peers still push). Simple barrier:
    // each worker waits for every worker's done flag.
    for (int h = 0; h < num_hosts; h++) {
      for (int c = 0; c < num_clients; c++) {
        while (CACHELINE_LOAD(&shared->hosts[h].workers[c].done) == 0)
          __builtin_ia32_pause();
      }
    }
    store.stop();
    cxl_region_destroy(&r);
    _exit(0);
  }

  // Primary client: wait for ALL 2N workers to be done.
  for (int h = 0; h < num_hosts; h++) {
    for (int c = 0; c < num_clients; c++) {
      while (CACHELINE_LOAD(&shared->hosts[h].workers[c].done) == 0)
        __builtin_ia32_pause();
    }
  }
  store.stop();

  // Reap our fork children.
  for (pid_t p : children) { int st; waitpid(p, &st, 0); }

  // Host 1's primary client (host_id=1, client_id=0) is the "local primary"
  // on its machine — it reaps its own children and then exits. Only host 0's
  // global primary prints the YCSB line.
  if (host_id != 0) {
    cxl_region_destroy(&r);
    return 0;
  }

  // Aggregate across all total_workers.
  uint64_t agg_ops = 0, max_wall_ns = 0;
  uint64_t w_count = 0, w_sum_ns = 0, r_count = 0, r_sum_ns = 0;
  std::vector<uint64_t> worker_w_p50, worker_w_p99, worker_r_p50, worker_r_p99;
  worker_w_p50.reserve(total_workers); worker_w_p99.reserve(total_workers);
  worker_r_p50.reserve(total_workers); worker_r_p99.reserve(total_workers);
  for (int h = 0; h < num_hosts; h++) {
    for (int c = 0; c < num_clients; c++) {
      YcsbWorker *w = &shared->hosts[h].workers[c];
      uint64_t ops_w = CACHELINE_LOAD(&w->ops);
      uint64_t wall = CACHELINE_LOAD(&w->wall_ns);
      agg_ops += ops_w;
      if (wall > max_wall_ns) max_wall_ns = wall;
      uint64_t wc = CACHELINE_LOAD(&w->w_count);
      uint64_t wsum = CACHELINE_LOAD(&w->w_sum_ns);
      uint64_t rc = CACHELINE_LOAD(&w->r_count);
      uint64_t rsum = CACHELINE_LOAD(&w->r_sum_ns);
      w_count += wc; w_sum_ns += wsum; r_count += rc; r_sum_ns += rsum;
      if (wc > 0) {
        worker_w_p50.push_back(CACHELINE_LOAD(&w->w_p50_ns));
        worker_w_p99.push_back(CACHELINE_LOAD(&w->w_p99_ns));
      }
      if (rc > 0) {
        worker_r_p50.push_back(CACHELINE_LOAD(&w->r_p50_ns));
        worker_r_p99.push_back(CACHELINE_LOAD(&w->r_p99_ns));
      }
    }
  }
  auto median_of = [&](std::vector<uint64_t> &v) -> uint64_t {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
  };
  auto max_of = [&](const std::vector<uint64_t> &v) -> uint64_t {
    uint64_t m = 0; for (auto x : v) if (x > m) m = x; return m;
  };
  uint64_t w_avg_ns = w_count ? w_sum_ns / w_count : 0;
  uint64_t r_avg_ns = r_count ? r_sum_ns / r_count : 0;
  uint64_t w_p50_ns = median_of(worker_w_p50);
  uint64_t w_p99_ns = max_of(worker_w_p99);
  uint64_t r_p50_ns = median_of(worker_r_p50);
  uint64_t r_p99_ns = max_of(worker_r_p99);

  double wall_max_s = max_wall_ns / 1e9;
  double agg_thpt   = wall_max_s > 0 ? (double)agg_ops / wall_max_s : 0;
  double load_wall_s = (now_ns() - t_load_start) / 1e9;
  double load_thpt  = load_wall_s > 0
                       ? (double)load_ops_v.size() / load_wall_s : 0;

  // Print requested threads (for plot alignment), not the clamped value.
  // A "threads_eff" column gives the actual worker count used.
  printf("YCSB opt=%c cache=%d num_hosts=%d threads=%d threads_eff=%d "
         "load_ops=%zu load_thpt=%.0f "
         "trans_ops=%lu trans_wall_max=%.3fs trans_agg_thpt=%.0f "
         "w_avg_ns=%lu w_p50_ns=%lu w_p99_ns=%lu "
         "r_avg_ns=%lu r_p50_ns=%lu r_p99_ns=%lu\n",
         kConsensusOpt, cache_on ? 1 : 0, num_hosts,
         requested_clients, num_clients,
         load_ops_v.size(), load_thpt,
         agg_ops, wall_max_s, agg_thpt,
         w_avg_ns, w_p50_ns, w_p99_ns,
         r_avg_ns, r_p50_ns, r_p99_ns);

  cxl_region_destroy(&r);
  return 0;
}
