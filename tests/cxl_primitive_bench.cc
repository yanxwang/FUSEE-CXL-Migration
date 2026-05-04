// iter-8A Phase 1.A — CXL/DRAM primitive microbenchmark.
//
// Measures the cost of every primitive used in the Protocol A
// read/write path so Phase 3 attribution has a per-primitive baseline.
//
// Modes (env FUSEE_BENCH_MODE):
//   ALL       — run every benchmark (default)
//   FLUSH     — clflushopt issue + complete
//   LD_CXL    — CXL coherent load (post-flush)
//   ST_CXL    — CXL store + flush + sfence
//   FETCHADD  — atomic fetch_add on CXL
//   PINGPONG  — cross-host fetch_add ping-pong (needs 2 hosts)
//   SPINLOCK  — pthread_spinlock contention sweep (1/2/4/8/16/32/64 threads)
//   FENCES    — mfence / sfence / lfence
//
// Runs locally (FUSEE_NUM_HOSTS=1) for most ops; PINGPONG requires
// both hosts. Output: stderr table with median + p99 ns per op.

#include "cxl_mm.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <thread>
#include <vector>
#include <algorithm>
#include <sys/wait.h>
#include <unistd.h>

extern "C" {
#include "common.h"
}

using namespace fusee;

static inline uint64_t now_ns() {
  timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline uint64_t rdtscp_now(uint32_t *cpu) {
  unsigned a, d, c;
  asm volatile ("rdtscp" : "=a"(a), "=d"(d), "=c"(c));
  asm volatile ("lfence");
  *cpu = c;
  return ((uint64_t)d << 32) | a;
}

static double tsc_freq_ghz = 0;
static void calibrate_tsc() {
  uint32_t cpu;
  uint64_t t0_ns = now_ns();
  uint64_t c0 = rdtscp_now(&cpu);
  // ~100 ms calibration
  while (now_ns() - t0_ns < 100ULL * 1000000) { asm volatile("pause"); }
  uint64_t t1_ns = now_ns();
  uint64_t c1 = rdtscp_now(&cpu);
  tsc_freq_ghz = (double)(c1 - c0) / (double)(t1_ns - t0_ns);
  fprintf(stderr, "TSC freq calibrated: %.3f GHz\n", tsc_freq_ghz);
}

static uint64_t cycles_to_ns(uint64_t cycles) {
  return (uint64_t)((double)cycles / tsc_freq_ghz);
}

static uint64_t pct(std::vector<uint64_t> &v, double q) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[(size_t)(q * (v.size() - 1))];
}

static void print_row(const char *name, std::vector<uint64_t> &cycles_v, int N) {
  uint64_t p50 = pct(cycles_v, 0.50);
  uint64_t p99 = pct(cycles_v, 0.99);
  uint64_t pmax = cycles_v.empty() ? 0 : *std::max_element(cycles_v.begin(), cycles_v.end());
  fprintf(stderr,
    "| %-32s | %5d | %6.1f | %6.1f | %7.1f | %6lu | %6lu | %7lu |\n",
    name, N,
    cycles_to_ns(p50) * 1.0,
    cycles_to_ns(p99) * 1.0,
    cycles_to_ns(pmax) * 1.0,
    p50, p99, pmax);
}

// ============================================================
// Bench 1: clflushopt + sfence (single line)
// ============================================================
static void bench_flush(uint8_t *cxl_base, int iters) {
  std::vector<uint64_t> v; v.reserve(iters);
  uint8_t *p = cxl_base + 64 * 16;  // some line not in caches
  for (int i = 0; i < iters; i++) {
    uint32_t cpu;
    uint64_t c0 = rdtscp_now(&cpu);
    flush_line(p);
    asm volatile("sfence");
    uint64_t c1 = rdtscp_now(&cpu);
    v.push_back(c1 - c0);
  }
  print_row("clflushopt+sfence (CXL)", v, iters);
}

// ============================================================
// Bench 2: LD-CXL post-flush + mfence
// ============================================================
static void bench_ld_cxl(uint8_t *cxl_base, int iters) {
  std::vector<uint64_t> v; v.reserve(iters);
  // Fill some lines with something so loads return a known value.
  volatile uint64_t *p = (volatile uint64_t *)(cxl_base + 64 * 32);
  *p = 0xdeadbeef; flush_line((void *)p); asm volatile("sfence");
  for (int i = 0; i < iters; i++) {
    flush_line((void *)p);
    asm volatile("mfence");
    uint32_t cpu;
    uint64_t c0 = rdtscp_now(&cpu);
    uint64_t val = *p;
    asm volatile("" : : "r"(val) : "memory");
    uint64_t c1 = rdtscp_now(&cpu);
    v.push_back(c1 - c0);
  }
  print_row("LD-CXL (after flush+mfence)", v, iters);
}

// ============================================================
// Bench 3: ST-CXL + flush + sfence
// ============================================================
static void bench_st_cxl(uint8_t *cxl_base, int iters) {
  std::vector<uint64_t> v; v.reserve(iters);
  volatile uint64_t *p = (volatile uint64_t *)(cxl_base + 64 * 48);
  for (int i = 0; i < iters; i++) {
    uint32_t cpu;
    uint64_t c0 = rdtscp_now(&cpu);
    *p = (uint64_t)i;
    flush_line((void *)p);
    asm volatile("sfence");
    uint64_t c1 = rdtscp_now(&cpu);
    v.push_back(c1 - c0);
  }
  print_row("ST-CXL+flush+sfence", v, iters);
}

// ============================================================
// Bench 4: atomic fetch_add on CXL (single host)
// ============================================================
static void bench_fetchadd(uint8_t *cxl_base, int iters) {
  std::vector<uint64_t> v; v.reserve(iters);
  std::atomic<uint64_t> *a = (std::atomic<uint64_t> *)(cxl_base + 64 * 64);
  a->store(0);
  flush_line((void *)a); asm volatile("sfence");
  for (int i = 0; i < iters; i++) {
    uint32_t cpu;
    uint64_t c0 = rdtscp_now(&cpu);
    a->fetch_add(1, std::memory_order_acq_rel);
    uint64_t c1 = rdtscp_now(&cpu);
    v.push_back(c1 - c0);
  }
  print_row("CXL atomic fetch_add (no flush)", v, iters);
}

// ============================================================
// Bench 5: atomic fetch_add on CXL + flush_line + sfence (the AP16-correct form)
// ============================================================
static void bench_fetchadd_flushed(uint8_t *cxl_base, int iters) {
  std::vector<uint64_t> v; v.reserve(iters);
  std::atomic<uint64_t> *a = (std::atomic<uint64_t> *)(cxl_base + 64 * 80);
  a->store(0);
  flush_line((void *)a); asm volatile("sfence");
  for (int i = 0; i < iters; i++) {
    uint32_t cpu;
    uint64_t c0 = rdtscp_now(&cpu);
    a->fetch_add(1, std::memory_order_acq_rel);
    flush_line((void *)a);
    asm volatile("sfence");
    uint64_t c1 = rdtscp_now(&cpu);
    v.push_back(c1 - c0);
  }
  print_row("CXL atomic fetch_add+flush+sfence", v, iters);
}

// ============================================================
// Bench 6: fences alone
// ============================================================
static void bench_fences(int iters) {
  std::vector<uint64_t> mfence_v, sfence_v, lfence_v;
  for (int i = 0; i < iters; i++) {
    uint32_t cpu;
    uint64_t c0 = rdtscp_now(&cpu);
    asm volatile("mfence");
    uint64_t c1 = rdtscp_now(&cpu);
    mfence_v.push_back(c1 - c0);
  }
  for (int i = 0; i < iters; i++) {
    uint32_t cpu;
    uint64_t c0 = rdtscp_now(&cpu);
    asm volatile("sfence");
    uint64_t c1 = rdtscp_now(&cpu);
    sfence_v.push_back(c1 - c0);
  }
  for (int i = 0; i < iters; i++) {
    uint32_t cpu;
    uint64_t c0 = rdtscp_now(&cpu);
    asm volatile("lfence");
    uint64_t c1 = rdtscp_now(&cpu);
    lfence_v.push_back(c1 - c0);
  }
  print_row("mfence (alone)", mfence_v, iters);
  print_row("sfence (alone)", sfence_v, iters);
  print_row("lfence (alone)", lfence_v, iters);
}

// ============================================================
// Bench 7: pthread_spinlock — uncontended single thread
// ============================================================
static void bench_spinlock_uncontested(int iters) {
  pthread_spinlock_t lock;
  pthread_spin_init(&lock, PTHREAD_PROCESS_PRIVATE);
  std::vector<uint64_t> v; v.reserve(iters);
  for (int i = 0; i < iters; i++) {
    uint32_t cpu;
    uint64_t c0 = rdtscp_now(&cpu);
    pthread_spin_lock(&lock);
    pthread_spin_unlock(&lock);
    uint64_t c1 = rdtscp_now(&cpu);
    v.push_back(c1 - c0);
  }
  print_row("spinlock uncontested (lock+unlock)", v, iters);
  pthread_spin_destroy(&lock);
}

// ============================================================
// Bench 8: pthread_spinlock — N-way contention
// ============================================================
struct ContestedArg {
  pthread_spinlock_t *lock;
  int iters;
  std::vector<uint64_t> samples;
};

static void *contested_worker(void *arg) {
  auto *a = (ContestedArg *)arg;
  a->samples.reserve(a->iters);
  for (int i = 0; i < a->iters; i++) {
    uint32_t cpu;
    uint64_t c0 = rdtscp_now(&cpu);
    pthread_spin_lock(a->lock);
    // tiny critical section
    asm volatile("nop");
    pthread_spin_unlock(a->lock);
    uint64_t c1 = rdtscp_now(&cpu);
    a->samples.push_back(c1 - c0);
  }
  return nullptr;
}

static void bench_spinlock_contested(int n_threads, int iters_per_thread) {
  pthread_spinlock_t lock;
  pthread_spin_init(&lock, PTHREAD_PROCESS_PRIVATE);
  std::vector<pthread_t> tids(n_threads);
  std::vector<ContestedArg> args(n_threads);
  for (int t = 0; t < n_threads; t++) {
    args[t].lock = &lock;
    args[t].iters = iters_per_thread;
  }
  for (int t = 0; t < n_threads; t++) {
    pthread_create(&tids[t], nullptr, contested_worker, &args[t]);
  }
  for (int t = 0; t < n_threads; t++) {
    pthread_join(tids[t], nullptr);
  }
  std::vector<uint64_t> all;
  for (int t = 0; t < n_threads; t++) {
    all.insert(all.end(), args[t].samples.begin(), args[t].samples.end());
  }
  char name[64];
  snprintf(name, sizeof(name), "spinlock contested T=%d", n_threads);
  print_row(name, all, all.size());
  pthread_spin_destroy(&lock);
}

int main(int argc, char **argv) {
  const char *dev = (argc > 1) ? argv[1] : "/dev/dax0.0";
  const char *mode = getenv("FUSEE_BENCH_MODE");
  if (!mode) mode = "ALL";

  calibrate_tsc();

  // Open small CXL region for tests.
  CXLRegion r{};
  if (cxl_region_init(&r, dev, 64 * 1024 * 1024) < 0) {
    fprintf(stderr, "cxl_region_init failed\n"); return 1;
  }
  uint8_t *cxl = (uint8_t *)r.base;
  std::memset(cxl, 0, 64 * 1024);

  fprintf(stderr,
    "\n| %-32s | %5s | %6s | %6s | %7s | %6s | %6s | %7s |\n",
    "primitive", "N", "p50ns", "p99ns", "maxns", "p50c", "p99c", "maxc");
  fprintf(stderr,
    "|%s|%s|%s|%s|%s|%s|%s|%s|\n",
    "----------------------------------",
    "-------",
    "--------",
    "--------",
    "---------",
    "--------",
    "--------",
    "---------");

  bool all = (std::strcmp(mode, "ALL") == 0);
  if (all || std::strcmp(mode, "FENCES") == 0) bench_fences(10000);
  if (all || std::strcmp(mode, "FLUSH") == 0) bench_flush(cxl, 10000);
  if (all || std::strcmp(mode, "LD_CXL") == 0) bench_ld_cxl(cxl, 10000);
  if (all || std::strcmp(mode, "ST_CXL") == 0) bench_st_cxl(cxl, 10000);
  if (all || std::strcmp(mode, "FETCHADD") == 0) {
    bench_fetchadd(cxl, 10000);
    bench_fetchadd_flushed(cxl, 10000);
  }
  if (all || std::strcmp(mode, "SPINLOCK") == 0) {
    bench_spinlock_uncontested(10000);
    for (int n : {2, 4, 8, 16, 32, 64}) {
      bench_spinlock_contested(n, 1000);
    }
  }

  cxl_region_destroy(&r);
  return 0;
}
