// Phase 5: minimal YCSB workload-file runner for the CXL-FUSEE KV store.
//
// Reads a YCSB-style spec file (one op per line, formats supported:
//   "OP KEY"            (YCSB_10M-style)
//   "OP TABLE KEY"      (original FUSEE spec)
//   where OP is one of INSERT / READ / UPDATE / DELETE and KEY is an
//   arbitrary string), hashes the key down to u64, and dispatches to
//   CxlKvStore. Single-process; multi-proc can be built on top later.
//
// Usage:
//   ./cxl_ycsb_runner <dev_path> <load_file> <trans_file> [num_buckets] [max_ops]
//
// load_file is the INSERT-only phase (typically <workload>.spec_load);
// trans_file is the mixed phase (<workload>.spec_trans).
// max_ops (optional): cap each phase to this many ops. Useful for quick
// smoke runs against the real YCSB workloads which can be 10 M+ lines.
//
// Role mode for cross-machine runs (set both env vars):
//   FUSEE_NUM_HOSTS=N     total number of hosts (invocations) in this run
//   FUSEE_HOST_ID=i       0-based role id for THIS invocation
//
//   With role mode, host 0 runs the entire load phase alone to populate the
//   store, then all hosts run their slice of the trans phase in parallel
//   (host i executes trans ops at indices k where k % N == i). Host 0
//   prints the aggregated YCSB summary (sum of per-host trans_thpt across
//   hosts, matched against the max wall across hosts).
//
// Emits a single line summary:
//   YCSB opt=X load_ops=N load_thpt=... trans_ops=M trans_thpt=... ...

#include "cxl_kv_store.h"
#include "cxl_mm.h"
#include "cxl_hashtable.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

extern "C" {
#include "common.h"   // cacheline_u64, CACHELINE_LOAD / _STORE
}

// Shared stats region for cross-host aggregation. Placed at the END of the
// CXL region so it does not collide with the KV-store carve-out.
namespace {
struct YcsbHostRow {
  cacheline_u64 attached;
  cacheline_u64 load_done;   // set by host 0 after load phase
  cacheline_u64 trans_done;  // per-host: set after each host's trans slice
  cacheline_u64 trans_ops;
  cacheline_u64 trans_wall_ns;
};
struct YcsbShared {
  cacheline_u64 init_done;
  YcsbHostRow hosts[4];      // kMaxHosts
};
constexpr size_t kYcsbStatsOffsetFromEnd = 8192;
} // namespace

using fusee::CxlKvStore;
using fusee::CXLRegion;
using fusee::cxl_region_destroy;
using fusee::cxl_region_init;
using fusee::fnv1a_u64;
using fusee::kConsensusOpt;

namespace {

uint64_t now_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t hash_str(const std::string &s) {
  // Chain FNV-1a over bytes, fold into a 64-bit result. Reserves 0.
  uint64_t h = 0xcbf29ce484222325ULL;
  for (unsigned char c : s) {
    h ^= c;
    h *= 0x100000001b3ULL;
  }
  if (h == 0) h = 1; // cannot use kEmptyKey
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

struct Op {
  OpKind kind;
  uint64_t key;
};

std::vector<Op> load_ops(const std::string &path) {
  std::vector<Op> out;
  std::ifstream f(path);
  if (!f) {
    fprintf(stderr, "failed to open %s: %s\n", path.c_str(), strerror(errno));
    return out;
  }
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    // Tokens separated by whitespace; accept "OP KEY" or "OP TABLE KEY".
    size_t p1 = line.find_first_of(" \t");
    if (p1 == std::string::npos) continue;
    size_t p2 = line.find_first_not_of(" \t", p1);
    if (p2 == std::string::npos) continue;
    size_t p3 = line.find_first_of(" \t", p2);
    std::string op_tok = line.substr(0, p1);
    std::string key_tok;
    if (p3 == std::string::npos) {
      key_tok = line.substr(p2);
    } else {
      size_t p4 = line.find_first_not_of(" \t", p3);
      if (p4 == std::string::npos) continue;
      key_tok = line.substr(p4);
      // If there was a third token, assume first-after-op was "TABLE".
    }
    // Strip trailing whitespace.
    while (!key_tok.empty() &&
           (key_tok.back() == ' ' || key_tok.back() == '\t' ||
            key_tok.back() == '\n' || key_tok.back() == '\r')) {
      key_tok.pop_back();
    }
    OpKind k = parse_op(op_tok);
    if (k == OP_SKIP) continue;
    out.push_back({k, hash_str(key_tok)});
  }
  return out;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr,
            "usage: %s <dev_path> <load_file> <trans_file> [num_buckets] [max_ops]\n",
            argv[0]);
    return 2;
  }
  const char *dev = argv[1];
  std::string load_path = argv[2];
  std::string trans_path = argv[3];
  uint32_t num_buckets =
      (argc >= 5) ? (uint32_t)strtoul(argv[4], nullptr, 0) : 16384U;
  size_t max_ops =
      (argc >= 6) ? (size_t)strtoull(argv[5], nullptr, 0) : 0;

  std::vector<Op> load_ops_v = load_ops(load_path);
  std::vector<Op> trans_ops_v = load_ops(trans_path);
  if (max_ops > 0) {
    if (load_ops_v.size()  > max_ops) load_ops_v.resize(max_ops);
    if (trans_ops_v.size() > max_ops) trans_ops_v.resize(max_ops);
  }
  if (load_ops_v.empty() && trans_ops_v.empty()) {
    fprintf(stderr, "no valid ops parsed\n");
    return 1;
  }
  printf("parsed %zu load ops, %zu trans ops\n",
         load_ops_v.size(), trans_ops_v.size());

  // Role mode detection. If FUSEE_NUM_HOSTS is unset, behave as the
  // original single-process runner. Otherwise we are one of N cooperating
  // processes (possibly on different machines) sharing this CXL region.
  int num_hosts = 1;
  int host_id = 0;
  {
    const char *nh = getenv("FUSEE_NUM_HOSTS");
    const char *hid = getenv("FUSEE_HOST_ID");
    if (nh && nh[0] != '\0') num_hosts = atoi(nh);
    if (hid && hid[0] != '\0') host_id = atoi(hid);
    if (num_hosts < 1 || num_hosts > 4 || host_id < 0 || host_id >= num_hosts) {
      fprintf(stderr, "bad FUSEE_NUM_HOSTS=%d FUSEE_HOST_ID=%d (need 1..4)\n",
              num_hosts, host_id);
      return 2;
    }
  }
  bool role_mode = (num_hosts > 1);
  bool is_primary = (host_id == 0);

  size_t store_bytes = CxlKvStore::bytes_for(num_buckets);
  size_t needed_kv = store_bytes;
  if (role_mode) needed_kv += kYcsbStatsOffsetFromEnd;
  size_t needed = ((needed_kv + fusee::kCxlDevdaxAlign - 1) /
                   fusee::kCxlDevdaxAlign) *
                  fusee::kCxlDevdaxAlign;

  CXLRegion r{};
  if (cxl_region_init(&r, dev, needed) < 0) {
    fprintf(stderr, "cxl_region_init failed\n");
    return 1;
  }

  // Shared stats region (only meaningful in role mode).
  YcsbShared *shared = nullptr;
  if (role_mode) {
    shared = reinterpret_cast<YcsbShared *>(
        reinterpret_cast<char *>(r.base) + r.size - kYcsbStatsOffsetFromEnd);
    if (is_primary) {
      std::memset(shared, 0, sizeof(*shared));
      flush_region(shared, sizeof(*shared));
      store_fence();
    }
  }

  // Primary attaches with init_region=true; others wait for init_done.
  CxlKvStore store;
  size_t kv_bytes = role_mode ? (r.size - kYcsbStatsOffsetFromEnd) : r.size;
  if (is_primary) {
    if (store.attach(r.base, kv_bytes, num_buckets, host_id, num_hosts,
                     /*init_region=*/true) != 0) {
      fprintf(stderr, "attach failed\n");
      cxl_region_destroy(&r);
      return 1;
    }
    if (role_mode) CACHELINE_STORE(&shared->init_done, 1ULL);
  } else {
    while (CACHELINE_LOAD(&shared->init_done) == 0) __builtin_ia32_pause();
    if (store.attach(r.base, kv_bytes, num_buckets, host_id, num_hosts,
                     /*init_region=*/false) != 0) {
      fprintf(stderr, "[host %d] attach failed\n", host_id);
      cxl_region_destroy(&r);
      return 1;
    }
  }

  const char *cache_env = getenv("FUSEE_CACHE");
  bool cache_on = (cache_env && cache_env[0] == '1');
  if (cache_on) store.enable_dram_cache(true);

  // "Attached" barrier in role mode so every host's replicator thread is
  // running before anyone starts mutating the store.
  if (role_mode) {
    CACHELINE_STORE(&shared->hosts[host_id].attached, 1ULL);
    for (int h = 0; h < num_hosts; h++) {
      while (CACHELINE_LOAD(&shared->hosts[h].attached) == 0)
        __builtin_ia32_pause();
    }
  }

  auto run_phase = [&](const std::vector<Op> &ops, const char *label) {
    uint64_t fails = 0;
    uint64_t t0 = now_ns();
    for (const auto &o : ops) {
      uint64_t v = o.key ^ 0xCAFEBABEULL;
      uint64_t out = 0;
      int rc = 0;
      switch (o.kind) {
        case OP_INSERT: rc = store.insert(o.key, v); break;
        case OP_UPDATE: rc = store.update(o.key, v); break;
        case OP_DELETE: rc = store.remove(o.key); break;
        case OP_READ:   rc = store.search(o.key, &out); break;
        default: break;
      }
      if (rc < 0 && rc != -1 && rc != -2) fails++;
    }
    uint64_t t1 = now_ns();
    double wall = (t1 - t0) / 1e9;
    double thpt = ops.empty() ? 0.0 : ops.size() / wall;
    printf("PHASE host=%d label=%s ops=%zu wall=%.3fs thpt=%.0f fails=%lu\n",
           host_id, label, ops.size(), wall, thpt, fails);
    return std::pair<double, double>{wall, thpt};
  };

  // LOAD phase:
  //   role mode: host 0 does the whole load alone; others wait for load_done.
  //   single:    as before.
  std::pair<double, double> load_res{0.0, 0.0};
  if (!role_mode) {
    load_res = run_phase(load_ops_v, "load");
  } else if (is_primary) {
    load_res = run_phase(load_ops_v, "load");
    CACHELINE_STORE(&shared->hosts[0].load_done, 1ULL);
  } else {
    while (CACHELINE_LOAD(&shared->hosts[0].load_done) == 0)
      __builtin_ia32_pause();
  }

  // TRANS phase:
  //   role mode: each host picks indices k where k % num_hosts == host_id.
  //   single:    runs all trans ops.
  std::vector<Op> my_trans;
  if (role_mode) {
    my_trans.reserve(trans_ops_v.size() / num_hosts + 1);
    for (size_t k = host_id; k < trans_ops_v.size(); k += num_hosts) {
      my_trans.push_back(trans_ops_v[k]);
    }
  }
  auto trans_res = role_mode ? run_phase(my_trans, "trans")
                             : run_phase(trans_ops_v, "trans");

  if (role_mode) {
    CACHELINE_STORE(&shared->hosts[host_id].trans_ops,
                    (uint64_t)my_trans.size());
    CACHELINE_STORE(&shared->hosts[host_id].trans_wall_ns,
                    (uint64_t)(trans_res.first * 1e9));
    CACHELINE_STORE(&shared->hosts[host_id].trans_done, 1ULL);
  }

  if (role_mode) {
    // Wait for all hosts to finish before stopping our replicator; mirrors
    // the fix we applied in cxl_kv_bench_mp.
    for (int h = 0; h < num_hosts; h++) {
      while (CACHELINE_LOAD(&shared->hosts[h].trans_done) == 0)
        __builtin_ia32_pause();
    }
  }
  store.stop();

  if (role_mode && !is_primary) {
    cxl_region_destroy(&r);
    return 0;
  }

  if (!role_mode) {
    printf("YCSB opt=%c cache=%d load_ops=%zu load_thpt=%.0f "
           "trans_ops=%zu trans_thpt=%.0f\n",
           kConsensusOpt, cache_on ? 1 : 0,
           load_ops_v.size(), load_res.second,
           trans_ops_v.size(), trans_res.second);
  } else {
    uint64_t agg_trans_ops = 0;
    double   max_wall_s    = 0.0;
    for (int h = 0; h < num_hosts; h++) {
      uint64_t ops = CACHELINE_LOAD(&shared->hosts[h].trans_ops);
      uint64_t wns = CACHELINE_LOAD(&shared->hosts[h].trans_wall_ns);
      agg_trans_ops += ops;
      double ws = wns / 1e9;
      if (ws > max_wall_s) max_wall_s = ws;
    }
    double agg_thpt = (max_wall_s > 0.0)
                          ? (double)agg_trans_ops / max_wall_s
                          : 0.0;
    printf("YCSB opt=%c cache=%d num_hosts=%d "
           "load_ops=%zu load_thpt=%.0f "
           "trans_ops=%lu trans_wall_max=%.3fs trans_agg_thpt=%.0f\n",
           kConsensusOpt, cache_on ? 1 : 0, num_hosts,
           load_ops_v.size(), load_res.second,
           agg_trans_ops, max_wall_s, agg_thpt);
  }

  cxl_region_destroy(&r);
  return 0;
}
