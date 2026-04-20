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
  if (store.attach(r.base, r.size, num_buckets, 0, 1, true) != 0) {
    fprintf(stderr, "attach failed\n");
    cxl_region_destroy(&r);
    return 1;
  }

  // FUSEE_CACHE=1 toggles the per-bucket DRAM cache. Off by default so the
  // sweep has comparable cache_off and cache_on runs side by side, matching
  // what cxl_kv_bench_mp already does.
  const char *cache_env = getenv("FUSEE_CACHE");
  bool cache_on = (cache_env && cache_env[0] == '1');
  if (cache_on) store.enable_dram_cache(true);

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
      // Treat duplicate-key INSERT and not-found READ as non-fatal; YCSB
      // workloads can reasonably produce both depending on generator settings.
      if (rc < 0 && rc != -1 && rc != -2) fails++;
    }
    uint64_t t1 = now_ns();
    double wall = (t1 - t0) / 1e9;
    double thpt = ops.empty() ? 0.0 : ops.size() / wall;
    printf("PHASE label=%s ops=%zu wall=%.3fs thpt=%.0f fails=%lu\n",
           label, ops.size(), wall, thpt, fails);
    return std::pair<double, double>{wall, thpt};
  };

  auto load_res = run_phase(load_ops_v, "load");
  auto trans_res = run_phase(trans_ops_v, "trans");

  printf("YCSB opt=%c cache=%d load_ops=%zu load_thpt=%.0f "
         "trans_ops=%zu trans_thpt=%.0f\n",
         kConsensusOpt, cache_on ? 1 : 0,
         load_ops_v.size(), load_res.second,
         trans_ops_v.size(), trans_res.second);

  store.stop();
  cxl_region_destroy(&r);
  return 0;
}
