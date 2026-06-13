// FUSEE Figure 11 micro throughput — port of FUSEE's
// micro-test/micro_test_multi_client.cc + micro_test.cc::run_client.
//
// Reference: /home/yanwang/g1/FUSEE/micro-test/{micro_test_multi_client,micro_test}.cc
//
// FUSEE's setup (16 CN × 8 clients/CN = 128 client pthreads, 2 MN):
//   - Each pthread runs num_coroutines_ fibers in tight loop
//   - 4 phases, each timer-bounded:
//       Phase 1 INSERT  500 ms
//       Phase 2 READ    5000 ms
//       Phase 3 UPDATE  5000 ms
//       Phase 4 DELETE  500 ms
//   - Per-phase: load_seq_kv_requests(N, op_type) → start fibers → timer
//     fiber sets should_stop=true at deadline → join → sum ops_cnt
//   - tpt = ops_summed * 1000 / wall_ms
//
// Protocol F port:
//   - num_clients_per_host pthreads on g1 + same on g2.  Total = 2 * C.
//   - Cross-host barriers via a TailCookies region at the tail of the CXL
//     mapping (same mechanism as cxl_kv_ops_F_2host_hashdiff.cc).
//   - Each pthread has a global_id = host_id * C + thread_id and owns key
//     range [global_id * g_keys_per_client, (global_id+1) * g_keys_per_client).
//     This avoids cross-thread races on the same key — FUSEE achieves the
//     same via per-fiber `coro_id`-indexed local request lists.
//   - Pre-load phase (before timing starts): every pthread inserts its
//     full key range so SEARCH/UPDATE/DELETE phases have keys to work on.
//   - INSERT phase: each thread inserts NEW keys past kPreLoadKeys.
//   - SEARCH/UPDATE phase: each thread cycles through its loaded keys.
//   - DELETE phase: each thread deletes from its loaded range one-shot.
//
// Usage:
//   FUSEE_F_HOST_ID=<0|1> FUSEE_F_NUM_HOSTS=2 \
//     FUSEE_F_NUM_CLIENTS=<C> FUSEE_F_RUN_COOKIE=<unique-u64> \
//     [FUSEE_F_INS_MS=500] [FUSEE_F_READ_MS=5000] \
//     [FUSEE_F_UPD_MS=5000] [FUSEE_F_DEL_MS=500] \
//     ./cxl_kv_ops_F_micro_throughput <dev_path>
//
// Output (per FUSEE convention, plus our SUMMARY line for sweep collection):
//   insert total: <N> ops
//   insert tpt: <N> ops/s
//   ...
//   SUMMARY F_micro_tpt num_hosts=<N> num_clients_per_host=<C> total=<T>
//     insert_tpt=<X> search_tpt=<Y> update_tpt=<Z> delete_tpt=<W>
//     # micro_optF_h<H>_c<C>

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
#include <thread>

extern "C" {
#include "common.h"
}

using namespace fusee;

namespace {

// Sizing.  LFM-per-slot is huge (~38.6 KB / mutex; MAX_HOST_NUM=200), so
// the lock table dominates the CXL footprint: num_buckets × 7 × 38.6 KB.
// At num_buckets = 65536: LFM ≈ 18 GiB.  At 131072: ≈ 35 GiB.  g1/g2 has
// 256 GiB CXL; 18 GiB is the sweet spot.
//
// g_keys_per_client is the pre-load slab.  Total pre-load at max C=64 × 2
// hosts = 128 threads × 100 = 12,800 keys.  INSERT phase adds up to
// kInsertCapPerThread NEW keys per thread (we cap this to avoid blowing
// past slot capacity even when threads are very fast).
//
// LFM-per-slot at MAX_HOST_NUM=200 = 38.6 KB/mutex.  Memory budget on
// g1/g2 (256 GB CXL) caps num_buckets practically:
//   - 131K buckets × 7 × 38.6 KB = 33 GB LFM table
//   - + 2 GB pool + 8 MB bucket array = ~35 GB total. Fits.
// Slot cap = 131K × 7 = 917K.  At target LF=0.5, max live = ~458K.
// No per-thread INSERT cap — pool exhaustion is the natural limit, not
// an arbitrary cap that masks throughput numbers.
constexpr uint32_t kNumBuckets         = 131072;
constexpr uint64_t kTotalRecordsDefault = 2000000;  // 2M × 1024B = 2 GB pool
constexpr uint64_t kKeysPerClientDefault = 100;
constexpr uint32_t kKvRecordSize       = 1024;
constexpr uint32_t kValueLen           = 1024 - 8;

// Runtime knob — initialised in main() from FUSEE_F_KEYS_PER_CLIENT env var
// (default kKeysPerClientDefault).  Use this everywhere instead of the
// compile-time constant so the same binary can run the regular sweep AND
// a DELETE-deep-pool variant where kKeysPerClient is bumped to 10K+.
uint64_t g_keys_per_client = kKeysPerClientDefault;
uint64_t g_total_records   = kTotalRecordsDefault;

uint64_t env_u64(const char *name, uint64_t dflt) {
  const char *e = std::getenv(name);
  if (e && e[0]) return std::strtoull(e, nullptr, 0);
  return dflt;
}

int env_int(const char *name, int dflt) {
  const char *e = std::getenv(name);
  if (e && e[0]) return std::atoi(e);
  return dflt;
}

uint64_t mk_load_key(int global_id, uint64_t i) {
  // Per-thread private load slab: keys [global_id*K, (global_id+1)*K).
  return static_cast<uint64_t>(global_id) * g_keys_per_client + i + 1;
}

// Per-thread INSERT stride.  Each thread owns a unique [stride*g, stride*(g+1))
// key range so concurrent INSERTs never collide.  No cap, but the stride
// must be larger than what any single thread could reach in 500 ms
// (~1M/thread max).
constexpr uint64_t kInsertStridePerThread = 2000000;
uint64_t mk_insert_key(int global_id, uint64_t cnt, uint64_t pre_load_end) {
  return pre_load_end + static_cast<uint64_t>(global_id) * kInsertStridePerThread + cnt + 1;
}

// Cross-host barrier region at tail of CXL mapping.
//
// CRITICAL: each (barrier_idx, host_id) cookie MUST be on its own
// cacheline.  Putting two hosts' cookies on the same cacheline causes a
// write-write data race: when host B brings the line into M state to
// publish its own cookie, host A's cookie value in that line may be
// stale (= initial 0), and host B's writeback then clobbers host A's
// already-written-to-memory cookie.  Symptom: half the time host A
// "vanishes" from B's view of the barrier (verified on g1/g2 2026-06-08).
constexpr int    kNumBarriers   = 8;
constexpr int    kMaxHosts      = 4;
struct alignas(64) HostCookie {
  std::atomic<uint64_t> v;
  char _pad[64 - sizeof(std::atomic<uint64_t>)];
};
static_assert(sizeof(HostCookie) == 64);
struct HostBarrier {
  HostCookie c[kMaxHosts];          // 4 × 64 = 256 B; each host owns its own line
};
struct TailBarriers {
  HostBarrier b[kNumBarriers];
};
static_assert(sizeof(TailBarriers) <= 4096);

void barrier_arrive_and_wait(TailBarriers *t, int idx, uint64_t run_cookie,
                             int host_id, int num_hosts) {
  t->b[idx].c[host_id].v.store(run_cookie, std::memory_order_release);
  flush_line(&t->b[idx].c[host_id].v);
  store_fence();
  for (int h = 0; h < num_hosts; h++) {
    if (h == host_id) continue;
    for (;;) {
      flush_line(&t->b[idx].c[h].v);
      full_fence();
      if (t->b[idx].c[h].v.load(std::memory_order_acquire) == run_cookie) break;
      for (int p = 0; p < 1024; p++) __builtin_ia32_pause();
    }
  }
}

// Per-thread args (FUSEE-equivalent MicroRunClientArgs).
struct ThreadArgs {
  int          thread_id;
  int          global_id;
  int          num_clients;
  CxlKvStoreF *store;
  pthread_barrier_t *local_barrier;  // intra-process per-phase barrier
  volatile bool *should_stop;
  // Counters (filled by thread)
  uint64_t     ins_ops;
  uint64_t     ins_failed;
  uint64_t     sea_ops;
  uint64_t     sea_failed;
  uint64_t     upd_ops;
  uint64_t     upd_failed;
  uint64_t     del_ops;
  uint64_t     del_failed;
  // Per-thread bases
  uint64_t     pre_load_end;
};

void *thread_main(void *_a) {
  ThreadArgs *a = static_cast<ThreadArgs *>(_a);

  // CRITICAL: each worker must register its UNIQUE LFM id before any KV
  // op.  Else multiple workers on the same host share the host_id LFM
  // slot → b[id] race → silent data corruption.
  CxlKvStoreF::set_worker_id(a->global_id);

  // Pin to core (FUSEE pins main_core_id + tid * 2; we use a simpler
  // 1-core-per-thread linear pin).
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(a->thread_id, &cs);
  pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);

  // Per-thread 1024 B value buffer (alloc once, reused for every op).
  alignas(8) uint8_t vbuf[kValueLen];
  auto fill_v = [&](uint64_t marker) {
    uint64_t *p = reinterpret_cast<uint64_t *>(vbuf);
    for (uint32_t i = 0; i < kValueLen / 8; i++) p[i] = marker + i;
  };

  // --- Pre-load this thread's private slab.  Untimed.
  for (uint64_t i = 0; i < g_keys_per_client; i++) {
    uint64_t k = mk_load_key(a->global_id, i);
    fill_v(k * 31 + 9);
    int rc = a->store->insert_blob(k, vbuf, kValueLen);
    if (rc != 0) {
      std::fprintf(stderr, "[t%d/g%d] pre-load insert k=%lu rc=%d\n",
                   a->thread_id, a->global_id, k, rc);
    }
  }

  // ---------------- Phase 1: INSERT (timed) ----------------
  pthread_barrier_wait(a->local_barrier);   // thread 0 sets should_stop after timer
  uint64_t ins_cnt = 0;
  while (!*a->should_stop) {
    uint64_t k = mk_insert_key(a->global_id, ins_cnt, a->pre_load_end);
    fill_v(k * 13 + 1);
    int rc = a->store->insert_blob(k, vbuf, kValueLen);
    ins_cnt++;
    if (rc != 0) a->ins_failed++;
  }
  a->ins_ops = ins_cnt;
  pthread_barrier_wait(a->local_barrier);   // phase end

  // ---------------- Phase 2: SEARCH (timed) ----------------
  pthread_barrier_wait(a->local_barrier);
  uint64_t sea_cnt = 0;
  uint64_t sea_idx = 0;
  while (!*a->should_stop) {
    uint64_t k = mk_load_key(a->global_id, sea_idx % g_keys_per_client);
    uint32_t want = kValueLen;
    int rc = a->store->search_blob(k, vbuf, &want);
    sea_cnt++;
    sea_idx++;
    if (rc != 0) a->sea_failed++;
  }
  a->sea_ops = sea_cnt;
  pthread_barrier_wait(a->local_barrier);

  // ---------------- Phase 3: UPDATE (timed) ----------------
  pthread_barrier_wait(a->local_barrier);
  uint64_t upd_cnt = 0;
  uint64_t upd_idx = 0;
  while (!*a->should_stop) {
    uint64_t k = mk_load_key(a->global_id, upd_idx % g_keys_per_client);
    fill_v(0xC0FFEEULL + upd_idx);
    int rc = a->store->update_blob(k, vbuf, kValueLen);
    upd_cnt++;
    upd_idx++;
    if (rc != 0) a->upd_failed++;
  }
  a->upd_ops = upd_cnt;
  pthread_barrier_wait(a->local_barrier);

  // ---------------- Phase 4: DELETE (timed) ----------------
  pthread_barrier_wait(a->local_barrier);
  uint64_t del_cnt = 0;
  while (!*a->should_stop && del_cnt < g_keys_per_client) {
    uint64_t k = mk_load_key(a->global_id, del_cnt);
    int rc = a->store->remove(k);
    del_cnt++;
    if (rc != 0) a->del_failed++;
  }
  a->del_ops = del_cnt;
  pthread_barrier_wait(a->local_barrier);

  return nullptr;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <dev_path>\n", argv[0]);
    return 1;
  }
  const char *dev_path = argv[1];

  int host_id   = env_int("FUSEE_F_HOST_ID",      0);
  int num_hosts = env_int("FUSEE_F_NUM_HOSTS",    2);
  int num_clients = env_int("FUSEE_F_NUM_CLIENTS", 1);
  uint64_t run_cookie = env_u64("FUSEE_F_RUN_COOKIE", 0xDEADBEEFULL);
  int ins_ms  = env_int("FUSEE_F_INS_MS",  500);
  int read_ms = env_int("FUSEE_F_READ_MS", 5000);
  int upd_ms  = env_int("FUSEE_F_UPD_MS",  5000);
  int del_ms  = env_int("FUSEE_F_DEL_MS",  500);
  g_keys_per_client = env_u64("FUSEE_F_KEYS_PER_CLIENT", kKeysPerClientDefault);
  g_total_records   = env_u64("FUSEE_F_TOTAL_RECORDS",   kTotalRecordsDefault);

  if (host_id < 0 || host_id >= num_hosts || num_hosts > kMaxHosts) {
    std::fprintf(stderr, "bad host_id/num_hosts\n");
    return 1;
  }
  if (num_clients < 1) num_clients = 1;

  std::size_t store_bytes =
      CxlKvStoreF::bytes_for(kNumBuckets, g_total_records, num_hosts, kKvRecordSize);
  std::size_t total_bytes = store_bytes + sizeof(TailBarriers);

  CXLRegion r{};
  if (cxl_region_init(&r, dev_path, total_bytes) != 0) {
    std::perror("cxl_region_init");
    return 1;
  }
  std::fprintf(stderr,
      "host %d/%d clients=%d cookie=%lu store_bytes=%zu map=%zu\n",
      host_id, num_hosts, num_clients, run_cookie, store_bytes, r.size);

  CxlKvStoreF store;
  int rc = store.attach(r.base, store_bytes, kNumBuckets, g_total_records,
                        host_id, num_hosts, /*init_region=*/(host_id == 0),
                        kKvRecordSize);
  if (rc != 0) {
    std::fprintf(stderr, "host %d attach rc=%d\n", host_id, rc);
    return 1;
  }
  // Tell the store the total cluster worker count so LFM lock_slot uses
  // the right num_hosts parameter (total workers, not physical hosts).
  store.set_total_workers(num_hosts * num_clients);
  std::fprintf(stderr, "host %d attach OK\n", host_id);

  TailBarriers *barriers = reinterpret_cast<TailBarriers *>(
      reinterpret_cast<uint8_t *>(r.base) + store_bytes);
  if (host_id == 0) {
    std::memset(barriers, 0, sizeof(*barriers));
    flush_region(barriers, sizeof(*barriers));
    store_fence();
  }

  uint64_t pre_load_end =
      static_cast<uint64_t>(num_hosts) * num_clients * g_keys_per_client + 1;

  // ---- Spawn pthreads ----
  pthread_barrier_t local_barrier;
  pthread_barrier_init(&local_barrier, nullptr, num_clients + 1);
  volatile bool should_stop = false;

  ThreadArgs *args_list = new ThreadArgs[num_clients];
  pthread_t  *tids      = new pthread_t[num_clients];
  for (int i = 0; i < num_clients; i++) {
    args_list[i] = {};
    args_list[i].thread_id     = i;
    args_list[i].global_id     = host_id * num_clients + i;
    args_list[i].num_clients   = num_clients;
    args_list[i].store         = &store;
    args_list[i].local_barrier = &local_barrier;
    args_list[i].should_stop   = &should_stop;
    args_list[i].pre_load_end  = pre_load_end;
    pthread_create(&tids[i], nullptr, thread_main, &args_list[i]);
  }

  // Wait for all threads to finish pre-load (no cross-host barrier yet —
  // pre-load is per-thread on private keyspace, no shared state).  Then
  // cross-host barrier before timed Phase 1.
  // The local_barrier with count = num_clients+1 includes the main
  // thread, so all 4 phase boundaries below sync 4 times.
  barrier_arrive_and_wait(barriers, 0, run_cookie + 100, host_id, num_hosts);
  std::fprintf(stderr, "host %d pre-load+barrier done\n", host_id);

  auto run_phase = [&](const char *name, int ms, int bar_idx) {
    should_stop = false;
    barrier_arrive_and_wait(barriers, bar_idx, run_cookie + bar_idx, host_id, num_hosts);
    pthread_barrier_wait(&local_barrier);  // release workers
    auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    should_stop = true;
    pthread_barrier_wait(&local_barrier);  // workers done
    auto t1 = std::chrono::steady_clock::now();
    auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::fprintf(stderr, "host %d phase %s done wall_ms=%ld\n", host_id, name, wall_ms);
  };

  run_phase("INSERT", ins_ms,  1);
  run_phase("SEARCH", read_ms, 2);
  run_phase("UPDATE", upd_ms,  3);
  run_phase("DELETE", del_ms,  4);

  for (int i = 0; i < num_clients; i++) pthread_join(tids[i], nullptr);

  uint64_t tot_ins = 0, tot_ins_f = 0;
  uint64_t tot_sea = 0, tot_sea_f = 0;
  uint64_t tot_upd = 0, tot_upd_f = 0;
  uint64_t tot_del = 0, tot_del_f = 0;
  for (int i = 0; i < num_clients; i++) {
    tot_ins += args_list[i].ins_ops;  tot_ins_f += args_list[i].ins_failed;
    tot_sea += args_list[i].sea_ops;  tot_sea_f += args_list[i].sea_failed;
    tot_upd += args_list[i].upd_ops;  tot_upd_f += args_list[i].upd_failed;
    tot_del += args_list[i].del_ops;  tot_del_f += args_list[i].del_failed;
  }

  // Guard against ms=0 (phase skipped via env override).
  auto tpt = [](uint64_t ops, uint64_t failed, int ms) -> uint64_t {
    if (ms <= 0) return 0;
    return (ops - failed) * 1000ULL / static_cast<uint64_t>(ms);
  };

  // Per-host stdout (FUSEE format).
  std::printf("insert total: %lu ops\n", tot_ins);
  std::printf("insert failed: %lu ops\n", tot_ins_f);
  std::printf("insert tpt: %lu ops/s\n", tpt(tot_ins, tot_ins_f, ins_ms));
  std::printf("search total: %lu ops\n", tot_sea);
  std::printf("search failed: %lu ops\n", tot_sea_f);
  std::printf("search tpt: %lu ops/s\n", tpt(tot_sea, tot_sea_f, read_ms));
  std::printf("update total: %lu ops\n", tot_upd);
  std::printf("update failed: %lu ops\n", tot_upd_f);
  std::printf("update tpt: %lu ops/s\n", tpt(tot_upd, tot_upd_f, upd_ms));
  std::printf("delete total: %lu ops\n", tot_del);
  std::printf("delete failed: %lu ops\n", tot_del_f);
  std::printf("delete tpt: %lu ops/s\n", tpt(tot_del, tot_del_f, del_ms));

  // SUMMARY line (sweep collection grep target).
  std::printf("SUMMARY F_micro_tpt host=%d num_hosts=%d num_clients_per_host=%d "
              "total_clients=%d "
              "insert_tpt=%lu search_tpt=%lu update_tpt=%lu delete_tpt=%lu "
              "# micro_optF_h%d_c%d\n",
              host_id, num_hosts, num_clients, num_hosts * num_clients,
              tpt(tot_ins, tot_ins_f, ins_ms),
              tpt(tot_sea, tot_sea_f, read_ms),
              tpt(tot_upd, tot_upd_f, upd_ms),
              tpt(tot_del, tot_del_f, del_ms),
              host_id, num_clients);

  delete[] tids;
  delete[] args_list;
  store.stop();
  cxl_region_destroy(&r);
  return 0;
}
