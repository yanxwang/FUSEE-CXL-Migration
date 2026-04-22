// Per-primitive latency decomposition on the CXL memory server.
//
// Goal: give every phase of the A/B/C write path an independently measured
// number, so protocol total latency can be estimated as the sum of
// primitives and compared to the observed end-to-end latency from
// cxl_kv_bench_mp. That lets us point at the dominant phase and decide
// where optimization effort pays off.
//
// Measured primitives (all on /dev/dax0.0):
//   P1 lock_uncontended       : BucketLockTable::lock(idx, 0, 1)
//   P2 lock_contended_N       : fork N procs, each acquires the same bucket
//   P3 flush_plus_fence       : 7 × flush_line + full_fence (read the bucket)
//   P4 store_plus_flush       : write one slot value + flush_line + store_fence
//   P5 epoch_bump             : CACHELINE_STORE(&write_epoch, v) + flush + fence
//   P6 unlock                 : shm_mutex_unlock
//   P7 search_optimistic      : search() under no contention (read only)
//
// Usage (fork-mode):
//   ./cxl_latency_decomp <dev> <iters> [num_contenders]
// num_contenders defaults to 1 (no contention). >1 requires MAX_HOST_NUM >= N.

#include "cxl_kv_store.h"
#include "cxl_mm.h"
#include "cxl_bucket_lock.h"
#include "cxl_hashtable.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include "common.h"
}

using fusee::BucketLockTable;
using fusee::CXLRegion;
using fusee::CxlKvBucket;
using fusee::CxlKvSlot;
using fusee::CxlKvStore;
using fusee::cxl_region_destroy;
using fusee::cxl_region_init;
using fusee::kCxlKvSlotsPerBucket;

static inline uint64_t now_ns() {
  timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void dump(const char *label, std::vector<uint64_t> &v) {
  if (v.empty()) { printf("%-20s n=0\n", label); return; }
  std::sort(v.begin(), v.end());
  uint64_t sum = 0; for (auto x : v) sum += x;
  printf("%-20s n=%zu min=%lu p50=%lu p90=%lu p99=%lu max=%lu avg=%lu (ns)\n",
         label, v.size(), v.front(),
         v[v.size()/2], v[v.size()*90/100], v[v.size()*99/100],
         v.back(), sum / v.size());
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <dev> <iters> [num_contenders]\n", argv[0]);
    return 2;
  }
  const char *dev = argv[1];
  uint64_t iters = strtoull(argv[2], nullptr, 0);
  int num_hosts = (argc >= 4) ? atoi(argv[3]) : 1;
  if (num_hosts < 1 || num_hosts > 4) {
    fprintf(stderr, "num_contenders must be 1..4\n"); return 2;
  }

  // Use a tiny KV store so everything fits in one cacheline-friendly region.
  const uint32_t NB = 4096;
  size_t store_bytes = CxlKvStore::bytes_for(NB);
  size_t needed = ((store_bytes + fusee::kCxlDevdaxAlign - 1) /
                   fusee::kCxlDevdaxAlign) * fusee::kCxlDevdaxAlign;

  // Fork BEFORE open + memset to avoid stale mmap on CXL devdax
  // (same issue that bit cxl_kv_bench_mp; see g34_livelock_root_cause.md).
  CXLRegion r{};
  if (cxl_region_init(&r, dev, needed) < 0) {
    fprintf(stderr, "cxl_region_init failed\n"); return 1;
  }
  std::memset(r.base, 0, std::min(store_bytes, r.size));
  flush_region(r.base, std::min(store_bytes, r.size));
  store_fence();

  std::vector<pid_t> children;
  int host_id = 0;
  for (int i = 1; i < num_hosts; i++) {
    pid_t p = fork();
    if (p < 0) { perror("fork"); return 1; }
    if (p == 0) { host_id = i; children.clear(); break; }
    children.push_back(p);
  }

  BucketLockTable lt;
  if (host_id == 0) lt.attach(r.base, NB, /*init_mutexes=*/true);
  else              lt.attach(r.base, NB, /*init_mutexes=*/false);

  // Make bucket 0 a valid object; its storage lives inside r.base already,
  // placed by the KV store layout right after the bucket-lock table.
  CxlKvBucket *buckets = reinterpret_cast<CxlKvBucket *>(
      reinterpret_cast<char *>(r.base) + BucketLockTable::bytes_for(NB));
  if (host_id == 0) {
    std::memset(buckets, 0, sizeof(CxlKvBucket) * NB);
    flush_region(buckets, sizeof(CxlKvBucket) * NB);
    store_fence();
  }

  // Simple attach/ready barrier across forked children (same structure
  // as cxl_kv_bench_mp uses).
  volatile cacheline_u64 *ready_flags =
      reinterpret_cast<cacheline_u64 *>(
          reinterpret_cast<char *>(r.base) + r.size - 256 - host_id * 64);
  for (int h = 0; h < num_hosts; h++) {
    auto *f = reinterpret_cast<cacheline_u64 *>(
        reinterpret_cast<char *>(r.base) + r.size - 256 - h * 64);
    if (h == host_id) CACHELINE_STORE(f, 1ULL);
  }
  if (host_id == 0) {
    for (int h = 0; h < num_hosts; h++) {
      auto *f = reinterpret_cast<cacheline_u64 *>(
          reinterpret_cast<char *>(r.base) + r.size - 256 - h * 64);
      while (CACHELINE_LOAD(f) == 0) __builtin_ia32_pause();
    }
  } else {
    auto *f0 = reinterpret_cast<cacheline_u64 *>(
        reinterpret_cast<char *>(r.base) + r.size - 256);
    while (CACHELINE_LOAD(f0) == 0) __builtin_ia32_pause();
  }

  // Reservoir vectors (host 0 is the one that prints — but every host
  // collects, they just don't dump unless they're 0).
  std::vector<uint64_t> t_lock, t_flush, t_store, t_epoch, t_unlock, t_search;
  t_lock.reserve(iters); t_flush.reserve(iters); t_store.reserve(iters);
  t_epoch.reserve(iters); t_unlock.reserve(iters); t_search.reserve(iters);

  // Everyone hits the same bucket (bucket 0) for max-contention
  // measurement — this is the worst case for LFM.
  const uint32_t IDX = 0;
  CxlKvBucket *b = &buckets[IDX];
  // Plant a known slot so search has a hit to find.
  CxlKvSlot *s0 = &b->slots[0];
  if (host_id == 0) {
    s0->key = 0xDEADBEEFULL;
    s0->value = 0xBADCAFEULL;
    flush_line(s0);
    store_fence();
  }

  // Barrier: wait until primary has planted the slot.
  if (host_id != 0) {
    while (true) {
      flush_line(&s0->key);
      full_fence();
      if (s0->key == 0xDEADBEEFULL) break;
      __builtin_ia32_pause();
    }
  }

  for (uint64_t i = 0; i < iters; i++) {
    // P1 / P2: lock acquire cost.
    uint64_t t0 = now_ns();
    lt.lock(IDX, host_id, num_hosts);
    uint64_t t1 = now_ns();
    t_lock.push_back(t1 - t0);

    // P3: flush + fence (reading the bucket to find a key).
    for (int s = 0; s < kCxlKvSlotsPerBucket; s++) flush_line(&b->slots[s].key);
    full_fence();
    uint64_t t2 = now_ns();
    t_flush.push_back(t2 - t1);

    // P4: store value + flush + fence (publishing a new value).
    b->slots[0].value = 0xCAFE0000ULL | (uint32_t)i;
    flush_line(&b->slots[0].value);
    store_fence();
    uint64_t t3 = now_ns();
    t_store.push_back(t3 - t2);

    // P5: epoch bump on the bucket's lock entry (how protocol C "publishes"
    // the write for readers).
    auto *entry = lt.entry(IDX);
    uint64_t cur = CACHELINE_LOAD(&entry->write_epoch);
    CACHELINE_STORE(&entry->write_epoch, cur + 1);
    uint64_t t4 = now_ns();
    t_epoch.push_back(t4 - t3);

    // P6: unlock.
    lt.unlock(IDX, host_id);
    uint64_t t5 = now_ns();
    t_unlock.push_back(t5 - t4);

    // P7: lock-free optimistic read (no lock). Measures the read-side cost
    // paid by protocol C's search even without contention.
    for (int s = 0; s < kCxlKvSlotsPerBucket; s++) flush_line(&b->slots[s].key);
    full_fence();
    uint64_t k_read = b->slots[0].key;
    uint64_t v_read = b->slots[0].value;
    (void)k_read; (void)v_read;
    uint64_t t6 = now_ns();
    t_search.push_back(t6 - t5);
  }

  if (host_id == 0) {
    printf("=== primitive latency (host=%d N=%d iters=%lu dev=%s) ===\n",
           host_id, num_hosts, iters, dev);
    dump("P1/2 lock_acquire",  t_lock);
    dump("P3 flush_plus_fence", t_flush);
    dump("P4 store_plus_flush", t_store);
    dump("P5 epoch_bump",       t_epoch);
    dump("P6 unlock",           t_unlock);
    dump("P7 search_optimistic",t_search);
  }

  if (host_id != 0) {
    cxl_region_destroy(&r);
    _exit(0);
  }
  for (pid_t p : children) { int st; waitpid(p, &st, 0); }
  cxl_region_destroy(&r);
  return 0;
}
