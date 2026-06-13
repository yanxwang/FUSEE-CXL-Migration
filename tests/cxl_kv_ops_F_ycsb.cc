// FUSEE Figure 13 YCSB throughput — port of FUSEE's
// ycsb-test/ycsb_test_multi_client.cc + ycsb_test.cc::load_test_cnt_ops_mt
// to Protocol F.
//
// Reference: /home/yanwang/g1/FUSEE/ycsb-test/{ycsb_test_multi_client,ycsb_test}.cc
//
// FUSEE's setup:
//   - num_clients pthreads/process × num_coroutines_ fibers each
//   - Workload: YCSB-A/B/C/D, 100K keys load + 10M ops trans, 1024 B KV
//     (paper §6.3).
//   - 2 files per (workload, thread):
//       workloads/<wl>.spec_load   — 100K INSERT lines
//       workloads/<wl>.spec_trans  — 10M op lines (mix per spec)
//     FUSEE pre-splits trans into .spec_trans<thread_id>; we slice the
//     single trans file in memory by global_id.
//   - Phase 1: thread-0 of process-0 loads all 100K keys.
//     Other threads wait at a barrier.
//   - Phase 2: timer-bounded (5s default).  Each thread streams its
//     slice cyclically until should_stop.
//   - Throughput = aggregate_ops / wall_time.
//
// Protocol F port:
//   - 2 hosts (g1 + g2), C pthreads/host (up to 64) → total 2*C clients.
//   - Cross-host barriers via TailBarriers (same mechanism as
//     cxl_kv_ops_F_micro_throughput.cc / cxl_kv_ops_F_2host_hashdiff.cc).
//   - Workload files in setup/workloads/.  Key u64 derived from FNV-1a
//     hash of the line's "userXXXX" suffix so load + trans see the same
//     key consistently.
//   - KV size: u64 / u64 (tiny class).  Diverges from paper 1024 B — see
//     docs/protocol_F_vs_FUSEE_alignment.md §3.6.
//
// Usage:
//   FUSEE_F_HOST_ID=<0|1> FUSEE_F_NUM_HOSTS=2 \
//     FUSEE_F_NUM_CLIENTS=<C> FUSEE_F_RUN_COOKIE=<u64> \
//     FUSEE_F_WORKLOAD=<a|b|c|d> [FUSEE_F_RUN_MS=5000] \
//     ./cxl_kv_ops_F_ycsb <dev_path> <load_file> <trans_file>
//
// Output:
//   SUMMARY F_ycsb host=<H> workload=<wl> num_hosts=<N> num_clients_per_host=<C>
//     total_clients=<T> wall_ms=<M> trans_ops=<O> trans_tpt=<Mops/s>
//     # ycsb_optF_h<H>_w<wl>_c<C>

#define _GNU_SOURCE
#include <sched.h>
#include <pthread.h>

#include "cxl_kv_ops_F.h"
#include "cxl_mm.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "common.h"
}

using namespace fusee;

namespace {

// Sizing — 100K-key Zipf workload + UPDATE churn → keep slot capacity ~5x
// keyspace.  num_buckets = 32768 → 229K slots, plenty for 100K + churn
// at LF ≤ 0.45.  LFM table ≈ 8.6 GiB.
constexpr uint32_t kNumBuckets     = 32768;
constexpr uint64_t kTotalRecords   = 1500000;    // 4× update churn headroom
constexpr int      kMaxHosts       = 4;
constexpr uint32_t kKvRecordSize   = 1024;       // paper §6.3: 1024 B KV
constexpr uint32_t kValueLen       = 1024 - 8;

constexpr int    kNumBarriers     = 8;
// CRITICAL: each (barrier_idx, host_id) cookie MUST own its own cacheline,
// else write-write race during cacheline-M acquisition can clobber the
// peer's published cookie.  See cxl_kv_ops_F_micro_throughput.cc comment.
struct alignas(64) HostCookie {
  std::atomic<uint64_t> v;
  char _pad[64 - sizeof(std::atomic<uint64_t>)];
};
static_assert(sizeof(HostCookie) == 64);
struct HostBarrier { HostCookie c[kMaxHosts]; };
struct TailBarriers { HostBarrier b[kNumBarriers]; };
static_assert(sizeof(TailBarriers) <= 4096);

void barrier_arrive_and_wait(TailBarriers *t, int idx, uint64_t cookie,
                             int host_id, int num_hosts) {
  t->b[idx].c[host_id].v.store(cookie, std::memory_order_release);
  flush_line(&t->b[idx].c[host_id].v);
  store_fence();
  for (int h = 0; h < num_hosts; h++) {
    if (h == host_id) continue;
    for (;;) {
      flush_line(&t->b[idx].c[h].v);
      full_fence();
      if (t->b[idx].c[h].v.load(std::memory_order_acquire) == cookie) break;
      for (int p = 0; p < 1024; p++) __builtin_ia32_pause();
    }
  }
}

int env_int(const char *name, int dflt) {
  const char *e = std::getenv(name);
  if (e && e[0]) return std::atoi(e);
  return dflt;
}

uint64_t env_u64(const char *name, uint64_t dflt) {
  const char *e = std::getenv(name);
  if (e && e[0]) return std::strtoull(e, nullptr, 0);
  return dflt;
}

std::string env_str(const char *name, const std::string &dflt) {
  const char *e = std::getenv(name);
  if (e && e[0]) return std::string(e);
  return dflt;
}

// FNV-1a-64 over the FULL line tail.  Used to map a "userXXXX" string to
// a u64 key consistently between load + trans.
uint64_t fnv1a_u64(const char *s, std::size_t n) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (std::size_t i = 0; i < n; i++) {
    h ^= static_cast<uint8_t>(s[i]);
    h *= 0x100000001b3ULL;
  }
  return h;
}

// Parse a single trans line of the form "<OP> usertable user<digits>".
// Returns false on malformed lines.
enum OpKind : uint8_t { OP_INSERT = 0, OP_READ = 1, OP_UPDATE = 2,
                        OP_SCAN = 3, OP_DELETE = 4, OP_UNKNOWN = 5 };
struct Op { OpKind kind; uint64_t key; };

bool parse_line(const std::string &line, Op *out) {
  if (line.empty()) return false;
  std::size_t sp1 = line.find(' ');
  if (sp1 == std::string::npos) return false;
  std::size_t sp2 = line.find(' ', sp1 + 1);
  if (sp2 == std::string::npos) return false;
  const char *op_str = line.c_str();
  OpKind k = OP_UNKNOWN;
  if (sp1 == 6 && !std::memcmp(op_str, "INSERT", 6)) k = OP_INSERT;
  else if (sp1 == 4 && !std::memcmp(op_str, "READ",   4)) k = OP_READ;
  else if (sp1 == 6 && !std::memcmp(op_str, "UPDATE", 6)) k = OP_UPDATE;
  else if (sp1 == 4 && !std::memcmp(op_str, "SCAN",   4)) k = OP_SCAN;
  else if (sp1 == 6 && !std::memcmp(op_str, "DELETE", 6)) k = OP_DELETE;
  else return false;
  // Key is the substring after sp2.
  const char *key_start = line.c_str() + sp2 + 1;
  std::size_t key_len = line.size() - (sp2 + 1);
  out->kind = k;
  out->key  = fnv1a_u64(key_start, key_len);
  return true;
}

struct ThreadArgs {
  int          thread_id;
  int          global_id;
  int          num_total_threads;
  CxlKvStoreF *store;
  pthread_barrier_t *local_barrier;
  volatile bool *should_stop;
  const std::vector<Op> *trans_ops;
  // Slice bounds (inclusive..exclusive).
  std::size_t  trans_lo;
  std::size_t  trans_hi;
  // Counters.
  uint64_t     ops_done;
  uint64_t     ops_failed;
};

void *thread_main(void *_a) {
  ThreadArgs *a = static_cast<ThreadArgs *>(_a);
  // Per-thread LFM id registration (see micro_throughput.cc).
  CxlKvStoreF::set_worker_id(a->global_id);
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(a->thread_id, &cs);
  pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);

  // Per-thread 1024 B value buffer (alloc once, reused).
  alignas(8) uint8_t vbuf[kValueLen];
  auto fill_v = [&](uint64_t marker) {
    uint64_t *p = reinterpret_cast<uint64_t *>(vbuf);
    for (uint32_t i = 0; i < kValueLen / 8; i++) p[i] = marker + i;
  };

  pthread_barrier_wait(a->local_barrier);   // start of trans phase

  uint64_t cnt = 0, failed = 0;
  std::size_t cursor = a->trans_lo;
  const std::size_t lo = a->trans_lo, hi = a->trans_hi;
  const auto &ops = *a->trans_ops;
  while (!*a->should_stop) {
    if (cursor >= hi) cursor = lo;
    const Op &op = ops[cursor];
    cursor++;
    int rc = 0;
    switch (op.kind) {
      case OP_READ:   {
        uint32_t want = kValueLen;
        rc = a->store->search_blob(op.key, vbuf, &want);
        break;
      }
      case OP_UPDATE:
        fill_v(op.key * 13 + 1);
        rc = a->store->update_blob(op.key, vbuf, kValueLen);
        break;
      case OP_INSERT:
        fill_v(op.key * 13 + 1);
        rc = a->store->insert_blob(op.key, vbuf, kValueLen);
        break;
      case OP_DELETE: rc = a->store->remove(op.key); break;
      default: rc = -1; break;  // SCAN unsupported
    }
    if (rc != 0) failed++;
    cnt++;
  }
  a->ops_done = cnt;
  a->ops_failed = failed;

  pthread_barrier_wait(a->local_barrier);   // end of trans phase
  return nullptr;
}

std::size_t read_file_to_ops(const std::string &path, std::vector<Op> *out) {
  std::ifstream f(path);
  if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 0; }
  std::string line;
  Op op{};
  while (std::getline(f, line)) {
    if (parse_line(line, &op)) out->push_back(op);
  }
  return out->size();
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 4) {
    std::fprintf(stderr,
        "usage: %s <dev_path> <load_file> <trans_file>\n", argv[0]);
    return 1;
  }
  const char *dev_path  = argv[1];
  const char *load_path = argv[2];
  const char *trans_path = argv[3];

  int host_id   = env_int("FUSEE_F_HOST_ID",   0);
  int num_hosts = env_int("FUSEE_F_NUM_HOSTS", 2);
  int num_clients = env_int("FUSEE_F_NUM_CLIENTS", 1);
  uint64_t run_cookie = env_u64("FUSEE_F_RUN_COOKIE", 0xCAFEBABEULL);
  int run_ms = env_int("FUSEE_F_RUN_MS", 5000);
  std::string wl = env_str("FUSEE_F_WORKLOAD", "a");

  if (host_id < 0 || host_id >= num_hosts || num_hosts > kMaxHosts) {
    std::fprintf(stderr, "bad host_id/num_hosts\n");
    return 1;
  }
  if (num_clients < 1) num_clients = 1;
  int num_total_threads = num_hosts * num_clients;

  std::size_t store_bytes =
      CxlKvStoreF::bytes_for(kNumBuckets, kTotalRecords, num_hosts, kKvRecordSize);
  std::size_t total_bytes = store_bytes + sizeof(TailBarriers);

  CXLRegion r{};
  if (cxl_region_init(&r, dev_path, total_bytes) != 0) {
    std::perror("cxl_region_init");
    return 1;
  }
  std::fprintf(stderr,
      "host %d/%d clients=%d wl=%s cookie=%lu store_bytes=%zu map=%zu\n",
      host_id, num_hosts, num_clients, wl.c_str(), run_cookie, store_bytes, r.size);

  CxlKvStoreF store;
  int rc = store.attach(r.base, store_bytes, kNumBuckets, kTotalRecords,
                        host_id, num_hosts, /*init_region=*/(host_id == 0),
                        kKvRecordSize);
  if (rc != 0) {
    std::fprintf(stderr, "host %d attach rc=%d\n", host_id, rc);
    return 1;
  }
  store.set_total_workers(num_hosts * num_clients);
  std::fprintf(stderr, "host %d attach OK\n", host_id);

  TailBarriers *barriers = reinterpret_cast<TailBarriers *>(
      reinterpret_cast<uint8_t *>(r.base) + store_bytes);
  if (host_id == 0) {
    std::memset(barriers, 0, sizeof(*barriers));
    flush_region(barriers, sizeof(*barriers));
    store_fence();
  }

  // --- Phase 1: LOAD ----------------------------------------------------
  // Host 0 reads the load file and INSERTs everything sequentially with
  // 1024 B values (paper §6.3 alignment).
  if (host_id == 0) {
    std::vector<Op> load_ops;
    load_ops.reserve(100000);
    std::size_t n = read_file_to_ops(load_path, &load_ops);
    std::fprintf(stderr, "host 0 load file parsed: %zu ops\n", n);
    alignas(8) uint8_t load_vbuf[kValueLen];
    auto t0 = std::chrono::steady_clock::now();
    uint64_t loaded = 0, load_failed = 0;
    for (const Op &op : load_ops) {
      uint64_t marker = op.key * 13 + 1;
      uint64_t *p = reinterpret_cast<uint64_t *>(load_vbuf);
      for (uint32_t i = 0; i < kValueLen / 8; i++) p[i] = marker + i;
      int r2 = store.insert_blob(op.key, load_vbuf, kValueLen);
      if (r2 != 0) load_failed++;
      loaded++;
    }
    auto t1 = std::chrono::steady_clock::now();
    auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::fprintf(stderr, "host 0 load done: %lu ops, %lu failed, wall_ms=%ld\n",
                 loaded, load_failed, wall_ms);
  }
  barrier_arrive_and_wait(barriers, 0, run_cookie + 1, host_id, num_hosts);
  std::fprintf(stderr, "host %d post-load barrier\n", host_id);

  // --- Phase 2: TRANS (timed) ------------------------------------------
  // All hosts parse the trans file (each thread takes its global_id-strided
  // slice).  Then barrier-sync, run for run_ms wall time, count ops.
  std::vector<Op> trans_ops;
  trans_ops.reserve(10000000);
  std::size_t n_trans = read_file_to_ops(trans_path, &trans_ops);
  std::fprintf(stderr, "host %d trans file parsed: %zu ops\n", host_id, n_trans);
  if (n_trans == 0) {
    std::fprintf(stderr, "no trans ops; exiting\n");
    return 2;
  }

  // Each thread gets a contiguous slice of the trans file based on its
  // global_id (host_id*num_clients + thread_id).
  std::size_t slice = n_trans / num_total_threads;
  if (slice == 0) slice = 1;

  pthread_barrier_t local_barrier;
  pthread_barrier_init(&local_barrier, nullptr, num_clients + 1);
  volatile bool should_stop = false;

  ThreadArgs *args_list = new ThreadArgs[num_clients];
  pthread_t  *tids      = new pthread_t[num_clients];
  for (int i = 0; i < num_clients; i++) {
    args_list[i] = {};
    args_list[i].thread_id     = i;
    args_list[i].global_id     = host_id * num_clients + i;
    args_list[i].num_total_threads = num_total_threads;
    args_list[i].store         = &store;
    args_list[i].local_barrier = &local_barrier;
    args_list[i].should_stop   = &should_stop;
    args_list[i].trans_ops     = &trans_ops;
    args_list[i].trans_lo      = args_list[i].global_id * slice;
    args_list[i].trans_hi      = (args_list[i].global_id == num_total_threads - 1)
                                 ? n_trans
                                 : args_list[i].trans_lo + slice;
    pthread_create(&tids[i], nullptr, thread_main, &args_list[i]);
  }

  barrier_arrive_and_wait(barriers, 1, run_cookie + 2, host_id, num_hosts);
  pthread_barrier_wait(&local_barrier);     // release workers
  auto t0 = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::milliseconds(run_ms));
  should_stop = true;
  pthread_barrier_wait(&local_barrier);     // workers done
  auto t1 = std::chrono::steady_clock::now();
  auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

  for (int i = 0; i < num_clients; i++) pthread_join(tids[i], nullptr);

  uint64_t tot_ops = 0, tot_failed = 0;
  for (int i = 0; i < num_clients; i++) {
    tot_ops    += args_list[i].ops_done;
    tot_failed += args_list[i].ops_failed;
  }
  uint64_t good_ops = (tot_ops > tot_failed) ? (tot_ops - tot_failed) : 0;
  uint64_t tpt_ops_s = good_ops * 1000ULL / static_cast<uint64_t>(wall_ms);

  std::printf("host %d wall_ms=%ld trans_ops=%lu failed=%lu tpt=%lu ops/s\n",
              host_id, wall_ms, tot_ops, tot_failed, tpt_ops_s);

  std::printf("SUMMARY F_ycsb host=%d workload=%s num_hosts=%d "
              "num_clients_per_host=%d total_clients=%d "
              "wall_ms=%ld trans_ops=%lu trans_tpt=%lu "
              "# ycsb_optF_h%d_w%s_c%d\n",
              host_id, wl.c_str(), num_hosts, num_clients,
              num_total_threads, wall_ms, good_ops, tpt_ops_s,
              host_id, wl.c_str(), num_clients);

  delete[] tids;
  delete[] args_list;
  store.stop();
  cxl_region_destroy(&r);
  return 0;
}
