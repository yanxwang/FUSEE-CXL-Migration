// Multi-threaded client scaling on a single machine.
//
// Models FUSEE's "one client process with N worker threads" pattern:
// one process, one CXL region, one BucketLockTable, N pthreads each
// running its own workload against the shared hash table. Each thread
// gets a unique LFM id in [0, N).
//
// Implements Option C semantics directly (lock + 7-slot probe + store +
// epoch bump + unlock) without routing through the CxlKvStore API, so
// it does not interact with the PendingRing matrix (whose kMaxHosts is
// still 8).
//
// Usage:
//   ./cxl_kv_bench_threads <dev> <threads> <ops_per_thread> <wratio> [num_buckets]
//
// Prints per-thread and aggregated lines.

#include "cxl_mm.h"
#include "cxl_bucket_lock.h"
#include "cxl_hashtable.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <random>
#include <thread>
#include <vector>

extern "C" {
#include "common.h"
}

using fusee::BucketLockTable;
using fusee::CXLRegion;
using fusee::CxlKvBucket;
using fusee::CxlKvSlot;
using fusee::cxl_region_destroy;
using fusee::cxl_region_init;
using fusee::kCxlKvSlotsPerBucket;

static inline uint64_t now_ns() {
  timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

struct ThreadCtx {
  int tid;
  int num_threads;
  uint64_t ops;
  double wratio;
  uint32_t num_buckets;
  BucketLockTable *lt;
  CxlKvBucket *buckets;
  // Output
  uint64_t wall_ns;
  uint64_t w_sum_ns, r_sum_ns;
  uint64_t w_cnt, r_cnt;
  uint64_t w_p99_ns, r_p99_ns;
};

static uint64_t make_key(int tid, uint64_t i) {
  return (static_cast<uint64_t>(tid + 1) << 40) | (i + 1);
}

static uint32_t bucket_idx(uint64_t key, uint32_t nb) {
  // Simple splitmix-like hash (matches CxlKvStoreC's style well enough).
  uint64_t x = key;
  x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return (uint32_t)(x & (nb - 1));
}

static int insert_op(ThreadCtx *c, uint64_t key, uint64_t val) {
  uint32_t idx = bucket_idx(key, c->num_buckets);
  c->lt->lock(idx, c->tid, c->num_threads);
  CxlKvBucket *b = &c->buckets[idx];
  CxlKvSlot *empty = nullptr;
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) flush_line(&b->slots[s].key);
  full_fence();
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
    uint64_t k = b->slots[s].key;
    if (k == key) { c->lt->unlock(idx, c->tid); return -2; }
    if (!empty && k == 0ULL) empty = &b->slots[s];
  }
  if (!empty) { c->lt->unlock(idx, c->tid); return -1; }
  empty->key = key;
  empty->value = val;
  flush_line(empty);
  store_fence();
  auto *entry = c->lt->entry(idx);
  uint64_t cur = CACHELINE_LOAD(&entry->write_epoch);
  CACHELINE_STORE(&entry->write_epoch, cur + 1);
  c->lt->unlock(idx, c->tid);
  return 0;
}

static int update_op(ThreadCtx *c, uint64_t key, uint64_t val) {
  uint32_t idx = bucket_idx(key, c->num_buckets);
  c->lt->lock(idx, c->tid, c->num_threads);
  CxlKvBucket *b = &c->buckets[idx];
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) flush_line(&b->slots[s].key);
  full_fence();
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
    if (b->slots[s].key == key) {
      b->slots[s].value = val;
      flush_line(&b->slots[s].value);
      store_fence();
      auto *entry = c->lt->entry(idx);
      uint64_t cur = CACHELINE_LOAD(&entry->write_epoch);
      CACHELINE_STORE(&entry->write_epoch, cur + 1);
      c->lt->unlock(idx, c->tid);
      return 0;
    }
  }
  c->lt->unlock(idx, c->tid);
  return -1;
}

static int search_op(ThreadCtx *c, uint64_t key, uint64_t *out) {
  uint32_t idx = bucket_idx(key, c->num_buckets);
  CxlKvBucket *b = &c->buckets[idx];
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) flush_line(&b->slots[s].key);
  full_fence();
  for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
    if (b->slots[s].key == key) {
      *out = b->slots[s].value;
      return 0;
    }
  }
  return -1;
}

static uint64_t quantile_ns(std::vector<uint64_t> &v, double q) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[(size_t)(q * (v.size() - 1))];
}

static void *thread_main(void *arg) {
  ThreadCtx *c = (ThreadCtx *)arg;
  // Populate own range
  std::vector<uint64_t> my_keys;
  my_keys.reserve(c->ops);
  for (uint64_t i = 0; i < c->ops; i++) {
    uint64_t k = make_key(c->tid, i);
    if (insert_op(c, k, k ^ 0x1234ULL) == 0) my_keys.push_back(k);
  }
  if (my_keys.empty()) { c->wall_ns = 0; return nullptr; }

  // Timed mixed workload
  std::mt19937_64 rng(0x1234u + c->tid);
  std::uniform_int_distribution<size_t> pick(0, my_keys.size() - 1);
  std::vector<uint64_t> wlat, rlat;
  wlat.reserve((size_t)(c->ops * c->wratio) + 16);
  rlat.reserve((size_t)(c->ops * (1.0 - c->wratio)) + 16);

  uint64_t t0 = now_ns();
  for (uint64_t i = 0; i < c->ops; i++) {
    uint64_t k = my_keys[pick(rng)];
    uint64_t ts = now_ns();
    if ((double)rng() / (double)rng.max() < c->wratio) {
      update_op(c, k, k ^ i);
      wlat.push_back(now_ns() - ts);
    } else {
      uint64_t out = 0;
      search_op(c, k, &out);
      rlat.push_back(now_ns() - ts);
    }
  }
  c->wall_ns = now_ns() - t0;

  uint64_t w_sum = 0; for (auto x : wlat) w_sum += x;
  uint64_t r_sum = 0; for (auto x : rlat) r_sum += x;
  c->w_sum_ns = w_sum; c->w_cnt = wlat.size();
  c->r_sum_ns = r_sum; c->r_cnt = rlat.size();
  c->w_p99_ns = quantile_ns(wlat, 0.99);
  c->r_p99_ns = quantile_ns(rlat, 0.99);
  return nullptr;
}

int main(int argc, char **argv) {
  if (argc < 5) {
    fprintf(stderr,
      "usage: %s <dev> <threads> <ops_per_thread> <wratio> [num_buckets]\n",
      argv[0]);
    return 2;
  }
  const char *dev = argv[1];
  int num_threads = atoi(argv[2]);
  uint64_t ops = strtoull(argv[3], nullptr, 0);
  double wratio = atof(argv[4]);
  uint32_t num_buckets = (argc >= 6) ? strtoul(argv[5], nullptr, 0) : 65536U;
  if (num_threads < 1 || num_threads > 32) {
    fprintf(stderr, "threads must be 1..32\n"); return 2;
  }
  if (wratio < 0) wratio = 0; if (wratio > 1) wratio = 1;

  size_t lt_bytes = BucketLockTable::bytes_for(num_buckets);
  size_t buckets_bytes = sizeof(CxlKvBucket) * num_buckets;
  size_t needed = ((lt_bytes + buckets_bytes + fusee::kCxlDevdaxAlign - 1) /
                   fusee::kCxlDevdaxAlign) * fusee::kCxlDevdaxAlign;

  CXLRegion r{};
  if (cxl_region_init(&r, dev, needed) < 0) {
    fprintf(stderr, "cxl_region_init failed\n"); return 1;
  }
  std::memset(r.base, 0, lt_bytes + buckets_bytes);
  flush_region(r.base, lt_bytes + buckets_bytes);
  store_fence();

  BucketLockTable lt;
  lt.attach(r.base, num_buckets, /*init_mutexes=*/true);
  CxlKvBucket *buckets = reinterpret_cast<CxlKvBucket *>(
      reinterpret_cast<char *>(r.base) + lt_bytes);

  std::vector<pthread_t> pthreads(num_threads);
  std::vector<ThreadCtx> ctxs(num_threads);
  for (int i = 0; i < num_threads; i++) {
    ctxs[i] = ThreadCtx{i, num_threads, ops, wratio, num_buckets, &lt, buckets,
                        0, 0, 0, 0, 0, 0, 0};
  }
  uint64_t t0 = now_ns();
  for (int i = 0; i < num_threads; i++) {
    pthread_create(&pthreads[i], nullptr, thread_main, &ctxs[i]);
  }
  for (int i = 0; i < num_threads; i++) pthread_join(pthreads[i], nullptr);
  uint64_t t_end = now_ns();

  uint64_t total_ops = 0;
  double max_wall_s = 0.0;
  for (int i = 0; i < num_threads; i++) {
    total_ops += ops;
    double ws = ctxs[i].wall_ns / 1e9;
    if (ws > max_wall_s) max_wall_s = ws;
    double thpt = ws > 0 ? ops / ws : 0;
    double w_avg_us = ctxs[i].w_cnt ? ctxs[i].w_sum_ns / (double)ctxs[i].w_cnt / 1000.0 : 0.0;
    double r_avg_us = ctxs[i].r_cnt ? ctxs[i].r_sum_ns / (double)ctxs[i].r_cnt / 1000.0 : 0.0;
    printf("THREAD tid=%d ops=%lu wall=%.3fs thpt=%.0f "
           "w_avg=%.2f w_p99=%.2f r_avg=%.2f r_p99=%.2f\n",
           i, ops, ws, thpt,
           w_avg_us, ctxs[i].w_p99_ns / 1000.0,
           r_avg_us, ctxs[i].r_p99_ns / 1000.0);
  }
  printf("AGG threads=%d wratio=%.2f total_ops=%lu wall_max=%.3fs "
         "wall_total=%.3fs agg_thpt=%.0f\n",
         num_threads, wratio, total_ops, max_wall_s,
         (t_end - t0) / 1e9, total_ops / max_wall_s);

  cxl_region_destroy(&r);
  return 0;
}
