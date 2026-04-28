// Per-stage latency decomposition for protocol A write path, forked from
// cxl_ycsb_runner.cc. Identical client model (fork children, per-client
// global_id = host_id * N + client_index, load done by primary, trans
// done by all 2N workers) so the contention pattern observed here mirrors
// the real YCSB scaling sweep. The difference is that the linked library
// is fusee_cxl_decomp (built with FUSEE_LATENCY_DECOMP=1), so every call
// to CxlKvStoreC::insert/update feeds per-stage samples into a
// process-local DecompProbe.
//
// After the TRANS phase each worker:
//   - summarises its 6 probe stages into (avg/p50/p99/count) and writes
//     them into a DecompWorker slot in the shared stats region.
// Host 0 primary aggregates across all 2N workers and prints a single
// DECOMP_C line per invocation, suitable for the orchestrator shell to
// grep.
//
// Env vars (same as cxl_ycsb_runner):
//   FUSEE_NUM_HOSTS  FUSEE_HOST_ID  FUSEE_NUM_THREADS  FUSEE_RUN_COOKIE
//   FUSEE_CACHE=1 enables the DRAM cache.
//
// Output line (single line, host 0 primary only):
//   DECOMP_C threads=N num_hosts=H cache=0|1 ops=<K_writes>
//     stage_lock_avg=<ns> stage_lock_p50=<ns> stage_lock_p99=<ns>
//     stage_scan_avg=<ns> stage_scan_p50=<ns> stage_scan_p99=<ns>
//     stage_publish_avg=<ns> stage_publish_p50=<ns> stage_publish_p99=<ns>
//     stage_epoch_avg=<ns> stage_epoch_p50=<ns> stage_epoch_p99=<ns>
//     stage_unlock_avg=<ns> stage_unlock_p50=<ns> stage_unlock_p99=<ns>
//     stage_total_avg=<ns> stage_total_p50=<ns> stage_total_p99=<ns>
//     trans_agg_thpt=<ops/s>

#include "cxl_kv_store.h"
#include "cxl_latency_decomp_probe.h"
#include "cxl_mm.h"
#include "cxl_hashtable.h"

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

struct DecompStageStats {
  cacheline_u64 count;
  cacheline_u64 avg_ns;
  cacheline_u64 p50_ns;
  cacheline_u64 p99_ns;
};

struct DecompWorker {
  cacheline_u64 started;
  cacheline_u64 done;
  cacheline_u64 ops;          // write ops (insert+update) the worker issued
  cacheline_u64 wall_ns;
  DecompStageStats stages[fusee::kDecompStageCount];
};

struct DecompHostRow {
  cacheline_u64 load_done_all;
  DecompWorker workers[kMaxClientsPerHost];
};

struct DecompShared {
  cacheline_u64 run_cookie;
  cacheline_u64 init_done;
  cacheline_u64 trans_go;
  DecompHostRow hosts[kMaxHostsLoc];
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

uint64_t quantile_u32(std::vector<uint32_t> &v, double q) {
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
using fusee::kDecompStageCount;
using fusee::decomp_probe;
using fusee::decomp_probe_reset;
using fusee::decomp_probe_reserve;

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

  int num_hosts = 1, host_id = 0, num_clients = 1;
  uint64_t run_cookie = 0;
  {
    const char *nh = getenv("FUSEE_NUM_HOSTS");
    const char *hid = getenv("FUSEE_HOST_ID");
    const char *nc = getenv("FUSEE_NUM_THREADS");
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

  size_t store_bytes = CxlKvStore::bytes_for(num_buckets);
  size_t needed_total = store_bytes + kYcsbStatsOffsetFromEnd;
  size_t needed = ((needed_total + fusee::kCxlDevdaxAlign - 1) /
                   fusee::kCxlDevdaxAlign) * fusee::kCxlDevdaxAlign;

  CXLRegion r{};
  if (cxl_region_init(&r, dev, needed) < 0) {
    fprintf(stderr, "cxl_region_init failed\n"); return 1;
  }
  DecompShared *shared = reinterpret_cast<DecompShared *>(
      reinterpret_cast<char *>(r.base) + r.size - kYcsbStatsOffsetFromEnd);
  bool is_host_primary = (host_id == 0);
  if (is_host_primary) {
    std::memset(shared, 0, sizeof(*shared));
    flush_region(shared, sizeof(*shared));
    store_fence();
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
  const int attach_id = global_id;
  const int attach_n  = total_workers;

  CxlKvStore store;
  size_t kv_bytes = r.size - kYcsbStatsOffsetFromEnd;
  if (is_primary_client) {
    if (store.attach(r.base, kv_bytes, num_buckets, attach_id, attach_n,
                     /*init_region=*/true, /*read_only=*/false) != 0) {
      fprintf(stderr, "[h%d c%d] attach failed\n", host_id, client_id);
      cxl_region_destroy(&r); return 1;
    }
    if (num_clients > 1 || role_mode) {
      CACHELINE_STORE(&shared->init_done, 1ULL);
    }
    if (role_mode) {
      CACHELINE_STORE(&shared->run_cookie, run_cookie);
    }
  } else {
    if (role_mode) {
      while (CACHELINE_LOAD(&shared->run_cookie) != run_cookie)
        __builtin_ia32_pause();
    }
    while (CACHELINE_LOAD(&shared->init_done) == 0)
      __builtin_ia32_pause();
    if (store.attach(r.base, kv_bytes, num_buckets, attach_id, attach_n,
                     /*init_region=*/false, /*read_only=*/false) != 0) {
      fprintf(stderr, "[h%d c%d] attach failed\n", host_id, client_id);
      cxl_region_destroy(&r);
      if (client_id > 0) _exit(1);
      return 1;
    }
  }
  bool cache_on = getenv("FUSEE_CACHE") && getenv("FUSEE_CACHE")[0] == '1';
  if (cache_on) store.enable_dram_cache(true);

  auto do_op = [&](const Op &o) {
    uint64_t v = o.key ^ 0xCAFEBABEULL;
    uint64_t out = 0;
    switch (o.kind) {
      case OP_INSERT: store.insert(o.key, v); break;
      case OP_UPDATE: store.update(o.key, v); break;
      case OP_DELETE: store.remove(o.key); break;
      case OP_READ:   store.search(o.key, &out); break;
      default: break;
    }
  };

  // LOAD on primary client only; discard its decomp samples afterwards so
  // the TRANS phase starts with an empty probe (we want only the contended
  // trans-phase samples).
  if (is_primary_client) {
    for (const Op &o : load_ops_v) do_op(o);
    decomp_probe_reset();
  }

  // Per-worker probe reservation for expected trans writes. Estimate 1.0×
  // trans_ops / total_workers; over-reserving is cheap and avoids realloc.
  decomp_probe_reserve(trans_ops_v.size() / total_workers + 64);

  DecompWorker *my_row = &shared->hosts[host_id].workers[client_id];
  CACHELINE_STORE(&my_row->started, 1ULL);

  if (is_primary_client) {
    for (int h = 0; h < num_hosts; h++) {
      for (int c = 0; c < num_clients; c++) {
        while (CACHELINE_LOAD(&shared->hosts[h].workers[c].started) == 0)
          __builtin_ia32_pause();
      }
    }
    CACHELINE_STORE(&shared->hosts[0].load_done_all, 1ULL);
    CACHELINE_STORE(&shared->trans_go, 1ULL);
  } else {
    while (CACHELINE_LOAD(&shared->trans_go) == 0) __builtin_ia32_pause();
  }

  // TRANS: same slicing as cxl_ycsb_runner so the workload-A contention
  // profile (Zipfian, 50/50 READ/UPDATE on 200k ops) is reproduced here.
  size_t NT = trans_ops_v.size();
  uint64_t t_start = now_ns();
  uint64_t my_ops = 0, my_writes = 0;
  for (size_t i = (size_t)global_id; i < NT; i += (size_t)total_workers) {
    const Op &o = trans_ops_v[i];
    do_op(o);
    my_ops++;
    if (o.kind == OP_INSERT || o.kind == OP_UPDATE) my_writes++;
  }
  uint64_t t_end = now_ns();

  // Summarise the local DecompProbe into per-stage (avg, p50, p99).
  {
    fusee::DecompProbe *probe = decomp_probe();
    for (int s = 0; s < kDecompStageCount; s++) {
      auto &samples = probe->stage_ns[s];
      uint64_t count = samples.size();
      uint64_t sum = 0;
      for (uint32_t x : samples) sum += x;
      uint64_t avg = count ? sum / count : 0;
      uint64_t p50 = quantile_u32(samples, 0.50);
      uint64_t p99 = quantile_u32(samples, 0.99);
      CACHELINE_STORE(&my_row->stages[s].count,  count);
      CACHELINE_STORE(&my_row->stages[s].avg_ns, avg);
      CACHELINE_STORE(&my_row->stages[s].p50_ns, p50);
      CACHELINE_STORE(&my_row->stages[s].p99_ns, p99);
    }
  }
  CACHELINE_STORE(&my_row->ops,     my_writes);
  CACHELINE_STORE(&my_row->wall_ns, t_end - t_start);
  CACHELINE_STORE(&my_row->done,    1ULL);

  if (!is_primary_client) {
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

  for (int h = 0; h < num_hosts; h++) {
    for (int c = 0; c < num_clients; c++) {
      while (CACHELINE_LOAD(&shared->hosts[h].workers[c].done) == 0)
        __builtin_ia32_pause();
    }
  }
  store.stop();
  for (pid_t p : children) { int st; waitpid(p, &st, 0); }

  if (host_id != 0) {
    cxl_region_destroy(&r);
    return 0;
  }

  // Aggregate across all 2N workers. Aggregation matches scaling_ycsb_spec:
  //   avg : total_sum / total_count
  //   p50 : median across per-worker p50
  //   p99 : max of per-worker p99 (conservative tail)
  uint64_t agg_writes = 0, max_wall_ns = 0;
  uint64_t stage_total_count[kDecompStageCount] = {0};
  uint64_t stage_total_sum[kDecompStageCount]   = {0};
  std::vector<uint64_t> worker_p50[kDecompStageCount];
  std::vector<uint64_t> worker_p99[kDecompStageCount];
  for (int s = 0; s < kDecompStageCount; s++) {
    worker_p50[s].reserve(total_workers);
    worker_p99[s].reserve(total_workers);
  }
  for (int h = 0; h < num_hosts; h++) {
    for (int c = 0; c < num_clients; c++) {
      DecompWorker *w = &shared->hosts[h].workers[c];
      uint64_t ops  = CACHELINE_LOAD(&w->ops);
      uint64_t wall = CACHELINE_LOAD(&w->wall_ns);
      agg_writes += ops;
      if (wall > max_wall_ns) max_wall_ns = wall;
      for (int s = 0; s < kDecompStageCount; s++) {
        uint64_t cnt = CACHELINE_LOAD(&w->stages[s].count);
        uint64_t avg = CACHELINE_LOAD(&w->stages[s].avg_ns);
        uint64_t p50 = CACHELINE_LOAD(&w->stages[s].p50_ns);
        uint64_t p99 = CACHELINE_LOAD(&w->stages[s].p99_ns);
        stage_total_count[s] += cnt;
        stage_total_sum[s]   += avg * cnt;
        if (cnt > 0) {
          worker_p50[s].push_back(p50);
          worker_p99[s].push_back(p99);
        }
      }
    }
  }

  auto median_of = [](std::vector<uint64_t> &v) -> uint64_t {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
  };
  auto max_of = [](const std::vector<uint64_t> &v) -> uint64_t {
    uint64_t m = 0; for (auto x : v) if (x > m) m = x; return m;
  };

  uint64_t agg_avg[kDecompStageCount];
  uint64_t agg_p50[kDecompStageCount];
  uint64_t agg_p99[kDecompStageCount];
  for (int s = 0; s < kDecompStageCount; s++) {
    agg_avg[s] = stage_total_count[s]
                   ? stage_total_sum[s] / stage_total_count[s] : 0;
    agg_p50[s] = median_of(worker_p50[s]);
    agg_p99[s] = max_of(worker_p99[s]);
  }

  double wall_max_s = max_wall_ns / 1e9;
  double agg_thpt = wall_max_s > 0 ? (double)agg_writes / wall_max_s : 0;

  printf(
    "DECOMP_A threads=%d num_hosts=%d cache=%d ops=%lu "
    "stage_lock_avg=%lu stage_lock_p50=%lu stage_lock_p99=%lu "
    "stage_scan_avg=%lu stage_scan_p50=%lu stage_scan_p99=%lu "
    "stage_publish_avg=%lu stage_publish_p50=%lu stage_publish_p99=%lu "
    "stage_epoch_avg=%lu stage_epoch_p50=%lu stage_epoch_p99=%lu "
    "stage_unlock_avg=%lu stage_unlock_p50=%lu stage_unlock_p99=%lu "
    "stage_total_avg=%lu stage_total_p50=%lu stage_total_p99=%lu "
    "lfm_localstore_avg=%lu lfm_localstore_p50=%lu lfm_localstore_p99=%lu "
    "lfm_peerscan_avg=%lu lfm_peerscan_p50=%lu lfm_peerscan_p99=%lu "
    "lfm_contwait_avg=%lu lfm_contwait_p50=%lu lfm_contwait_p99=%lu "
    "lfm_entercs_avg=%lu lfm_entercs_p50=%lu lfm_entercs_p99=%lu "
    "aggr_enq_avg=%lu aggr_enq_p50=%lu aggr_enq_p99=%lu "
    "sender_batch_avg=%lu sender_batch_p50=%lu sender_batch_p99=%lu "
    "recv_entry_avg=%lu recv_entry_p50=%lu recv_entry_p99=%lu "
    "recv_atomic_avg=%lu recv_atomic_p50=%lu recv_atomic_p99=%lu "
    "l1_slot_avg=%lu l1_slot_p50=%lu l1_slot_p99=%lu "
    "trans_wall_max=%.3fs trans_agg_thpt=%.0f\n",
    num_clients, num_hosts, cache_on ? 1 : 0, agg_writes,
    agg_avg[fusee::kDecompStageLock],    agg_p50[fusee::kDecompStageLock],    agg_p99[fusee::kDecompStageLock],
    agg_avg[fusee::kDecompStageScan],    agg_p50[fusee::kDecompStageScan],    agg_p99[fusee::kDecompStageScan],
    agg_avg[fusee::kDecompStagePublish], agg_p50[fusee::kDecompStagePublish], agg_p99[fusee::kDecompStagePublish],
    agg_avg[fusee::kDecompStageEpoch],   agg_p50[fusee::kDecompStageEpoch],   agg_p99[fusee::kDecompStageEpoch],
    agg_avg[fusee::kDecompStageUnlock],  agg_p50[fusee::kDecompStageUnlock],  agg_p99[fusee::kDecompStageUnlock],
    agg_avg[fusee::kDecompStageTotal],   agg_p50[fusee::kDecompStageTotal],   agg_p99[fusee::kDecompStageTotal],
    agg_avg[fusee::kDecompLfmLocalStore], agg_p50[fusee::kDecompLfmLocalStore], agg_p99[fusee::kDecompLfmLocalStore],
    agg_avg[fusee::kDecompLfmPeerScan],   agg_p50[fusee::kDecompLfmPeerScan],   agg_p99[fusee::kDecompLfmPeerScan],
    agg_avg[fusee::kDecompLfmContWait],   agg_p50[fusee::kDecompLfmContWait],   agg_p99[fusee::kDecompLfmContWait],
    agg_avg[fusee::kDecompLfmEnterCS],    agg_p50[fusee::kDecompLfmEnterCS],    agg_p99[fusee::kDecompLfmEnterCS],
    agg_avg[fusee::kDecompStageA_AggrEnq],    agg_p50[fusee::kDecompStageA_AggrEnq],    agg_p99[fusee::kDecompStageA_AggrEnq],
    agg_avg[fusee::kDecompStageA_SenderBatch], agg_p50[fusee::kDecompStageA_SenderBatch], agg_p99[fusee::kDecompStageA_SenderBatch],
    agg_avg[fusee::kDecompStageA_RecvEntry],   agg_p50[fusee::kDecompStageA_RecvEntry],   agg_p99[fusee::kDecompStageA_RecvEntry],
    agg_avg[fusee::kDecompStageA_RecvAtomic],  agg_p50[fusee::kDecompStageA_RecvAtomic],  agg_p99[fusee::kDecompStageA_RecvAtomic],
    agg_avg[fusee::kDecompStageA_LockL1Scan],  agg_p50[fusee::kDecompStageA_LockL1Scan],  agg_p99[fusee::kDecompStageA_LockL1Scan],
    wall_max_s, agg_thpt);
  (void)kConsensusOpt;

  cxl_region_destroy(&r);
  return 0;
}
