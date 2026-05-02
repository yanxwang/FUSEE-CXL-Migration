/*
 * Lightweight MLC-style bench for FUSEE-CXL testbed (g3/g4).
 *
 * Measures, for both DRAM (local anon mmap) and CXL (/dev/dax0.0 mmap):
 *   - seq_read_bw      : memcpy, warm, all cores
 *   - seq_write_bw     : memcpy, clflushopt+sfence write-back
 *   - rand_lat_idle    : single-threaded pointer-chase, 64 B stride
 *   - rand_lat_flush   : single-threaded clflushopt + mfence + load
 *                        (FUSEE's CACHELINE_LOAD path)
 *   - rand_read_bw     : many threads, independent random loads (no chase)
 *
 * Build:
 *   gcc -O3 -march=native -pthread -o bench_cxl_dram bench_cxl_dram.c
 *
 * Run:
 *   ./bench_cxl_dram [target]          # target = dram | cxl (default: both)
 *   ./bench_cxl_dram cxl 16             # threaded tests use 16 threads
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <x86intrin.h>

#define CXL_PATH "/dev/dax0.0"
#define REGION_SZ ((size_t)8 * 1024 * 1024 * 1024) // 8 GiB
#define CACHELINE 64

static inline uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* ---------- buffer setup ---------- */

static void *map_cxl(size_t bytes) {
  int fd = open(CXL_PATH, O_RDWR);
  if (fd < 0) {
    perror("open " CXL_PATH);
    return NULL;
  }
  void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) {
    perror("mmap cxl");
    close(fd);
    return NULL;
  }
  close(fd);
  return p;
}

static void *map_dram(size_t bytes) {
  void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    perror("mmap dram");
    return NULL;
  }
  // touch pages so they are really allocated + NUMA-local to calling core
  memset(p, 0xAB, bytes);
  return p;
}

/* ---------- sequential memcpy BW ---------- */

struct seq_arg {
  char *dst;
  char *src;
  size_t bytes;
  int is_write;   // 1 → flush after write
  uint64_t dt_ns; // out
};

static void *seq_worker(void *arg) {
  struct seq_arg *a = (struct seq_arg *)arg;
  uint64_t t0 = now_ns();
  if (a->is_write) {
    // non-temporal-ish write path: plain memcpy + clflushopt
    memcpy(a->dst, a->src, a->bytes);
    for (size_t i = 0; i < a->bytes; i += CACHELINE)
      _mm_clflushopt(a->dst + i);
    _mm_sfence();
  } else {
    // read: flush first so we actually hit memory (not L1/L2/L3 cache),
    // then stream through with a dependency chain to prevent DCE.
    volatile const uint64_t *p = (volatile const uint64_t *)a->src;
    size_t n = a->bytes / 8;
    for (size_t i = 0; i < a->bytes; i += CACHELINE)
      _mm_clflushopt((const char *)a->src + i);
    _mm_mfence();
    uint64_t acc = 0;
    for (size_t i = 0; i < n; i += 8) {
      acc ^= p[i + 0]; acc ^= p[i + 1]; acc ^= p[i + 2]; acc ^= p[i + 3];
      acc ^= p[i + 4]; acc ^= p[i + 5]; acc ^= p[i + 6]; acc ^= p[i + 7];
    }
    __asm__ volatile("" : : "r"(acc) : "memory");
  }
  a->dt_ns = now_ns() - t0;
  return NULL;
}

static double seq_bw_mbps(void *region, size_t region_bytes, int threads,
                           int is_write) {
  // Split the region across threads; each thread touches its slice.
  pthread_t *th = calloc(threads, sizeof(*th));
  struct seq_arg *args = calloc(threads, sizeof(*args));
  size_t per = region_bytes / threads;
  per &= ~(CACHELINE - 1);

  // For writes we need a source in DRAM; for reads we read the region
  // itself. To simplify, always treat region as src and allocate a
  // small DRAM scratch as dst. For writes we flush dst (the CXL region
  // if target is CXL).
  char *dram_scratch = NULL;
  if (is_write) {
    dram_scratch = map_dram(per);
    if (!dram_scratch) return 0;
    memset(dram_scratch, 0x55, per);
  }

  for (int i = 0; i < threads; i++) {
    args[i].bytes = per;
    if (is_write) {
      args[i].dst = (char *)region + (size_t)i * per;
      args[i].src = dram_scratch;
      args[i].is_write = 1;
    } else {
      args[i].src = (char *)region + (size_t)i * per;
      args[i].dst = NULL;
      args[i].is_write = 0;
    }
    pthread_create(&th[i], NULL, seq_worker, &args[i]);
  }
  uint64_t dt_max = 0;
  for (int i = 0; i < threads; i++) {
    pthread_join(th[i], NULL);
    if (args[i].dt_ns > dt_max) dt_max = args[i].dt_ns;
  }
  double mb = (double)per * threads / (1024.0 * 1024.0);
  double sec = dt_max / 1e9;
  if (dram_scratch) munmap(dram_scratch, per);
  free(args);
  free(th);
  return mb / sec;
}

/* ---------- random load BW, many independent loads ---------- */

struct rbw_arg {
  const uint64_t *region;
  size_t n_u64;
  uint64_t nops;
  uint64_t dt_ns;
  uint64_t sink; // prevent DCE
};

static void *rbw_worker(void *arg) {
  struct rbw_arg *a = (struct rbw_arg *)arg;
  uint64_t x = (uint64_t)(uintptr_t)a ^ 0xdeadbeefULL;
  uint64_t acc = 0;
  uint64_t t0 = now_ns();
  for (uint64_t i = 0; i < a->nops; i++) {
    x = x * 6364136223846793005ULL + 1442695040888963407ULL;
    size_t idx = (x >> 3) % a->n_u64;  // 8-byte aligned stride = cacheline
    acc += a->region[idx];
  }
  a->dt_ns = now_ns() - t0;
  a->sink = acc;
  return NULL;
}

static double rand_read_bw_mops(void *region, size_t bytes, int threads) {
  uint64_t nops = 2 * 1000 * 1000; // 2M per thread
  pthread_t *th = calloc(threads, sizeof(*th));
  struct rbw_arg *args = calloc(threads, sizeof(*args));
  for (int i = 0; i < threads; i++) {
    args[i].region = (const uint64_t *)region;
    args[i].n_u64 = bytes / 8;
    args[i].nops = nops;
    pthread_create(&th[i], NULL, rbw_worker, &args[i]);
  }
  uint64_t dt_max = 0;
  uint64_t sink = 0;
  for (int i = 0; i < threads; i++) {
    pthread_join(th[i], NULL);
    if (args[i].dt_ns > dt_max) dt_max = args[i].dt_ns;
    sink += args[i].sink;
  }
  double total_ops = (double)nops * threads;
  double sec = dt_max / 1e9;
  free(args);
  free(th);
  if (sink == 0x123) fprintf(stderr, "sink: %lu\n", sink);
  return total_ops / sec / 1e6;
}

/* ---------- single-thread pointer-chase latency ---------- */

static double pointer_chase_ns(void *region, size_t bytes, int n_chases,
                                int flush_each) {
  // Build a random-permutation pointer chain with CACHELINE stride.
  size_t n = bytes / CACHELINE;
  uint64_t *idx = malloc(n * sizeof(uint64_t));
  for (size_t i = 0; i < n; i++) idx[i] = i;
  // Fisher-Yates
  uint64_t seed = 0xbeef1234ULL;
  for (size_t i = n - 1; i > 0; i--) {
    seed = seed * 6364136223846793005ULL + 1ULL;
    size_t j = seed % (i + 1);
    uint64_t t = idx[i]; idx[i] = idx[j]; idx[j] = t;
  }
  // Build pointer chain in the region: region[idx[i]] = &region[idx[i+1]]
  uint64_t **r = (uint64_t **)region;
  for (size_t i = 0; i < n - 1; i++) {
    size_t slot = idx[i] * (CACHELINE / 8);
    size_t next = idx[i + 1] * (CACHELINE / 8);
    r[slot] = (uint64_t *)&r[next];
  }
  r[idx[n - 1] * (CACHELINE / 8)] = (uint64_t *)&r[idx[0] * (CACHELINE / 8)];

  // Chase
  uint64_t **p = (uint64_t **)&r[idx[0] * (CACHELINE / 8)];
  uint64_t t0 = now_ns();
  for (int i = 0; i < n_chases; i++) {
    if (flush_each) {
      _mm_clflushopt(p);
      _mm_mfence();
    }
    p = (uint64_t **)*p;
  }
  uint64_t dt = now_ns() - t0;
  if (p == (void *)0x123) fprintf(stderr, "p=%p\n", p);
  free(idx);
  return (double)dt / n_chases;
}

/* ---------- runner ---------- */

static void run_target(const char *name, void *region, size_t bytes,
                        int threads) {
  printf("\n=== %s ===\n", name);
  fflush(stdout);

  // Warm up the region (first-touch)
  if (strcmp(name, "CXL") == 0) {
    // For CXL just issue a pass of clflushopt+load
    volatile char *v = (volatile char *)region;
    for (size_t i = 0; i < bytes; i += 4096) v[i] = 0;
  }

  printf("  seq_read_bw   (%d thr): %.1f MB/s\n", threads,
         seq_bw_mbps(region, bytes, threads, 0));
  fflush(stdout);

  printf("  seq_write_bw  (%d thr): %.1f MB/s\n", threads,
         seq_bw_mbps(region, bytes, threads, 1));
  fflush(stdout);

  printf("  rand_read_bw  (%d thr): %.2f Mops/s  (%.1f MB/s)\n", threads,
         rand_read_bw_mops(region, bytes, threads),
         rand_read_bw_mops(region, bytes, threads) * 64.0);
  fflush(stdout);

  double lat_idle = pointer_chase_ns(region, 512ULL << 20, 200000, 0);
  printf("  rand_lat_idle          : %.1f ns/op (no flush, warm)\n", lat_idle);
  fflush(stdout);

  double lat_flush = pointer_chase_ns(region, 512ULL << 20, 20000, 1);
  printf("  rand_lat_flush         : %.1f ns/op (clflushopt+mfence+load)\n",
         lat_flush);
  fflush(stdout);
}

int main(int argc, char **argv) {
  const char *target = (argc > 1) ? argv[1] : "both";
  int threads = (argc > 2) ? atoi(argv[2]) : 16;

  printf("FUSEE-CXL bench (MLC-style). threads=%d region=%zu MiB\n",
         threads, (size_t)(REGION_SZ >> 20));

  if (!strcmp(target, "dram") || !strcmp(target, "both")) {
    void *dram = map_dram(REGION_SZ);
    if (!dram) return 1;
    run_target("DRAM", dram, REGION_SZ, threads);
    munmap(dram, REGION_SZ);
  }

  if (!strcmp(target, "cxl") || !strcmp(target, "both")) {
    void *cxl = map_cxl(REGION_SZ);
    if (!cxl) {
      fprintf(stderr, "CXL region not available (is /dev/dax0.0 devdax?)\n");
      return 1;
    }
    run_target("CXL", cxl, REGION_SZ, threads);
    munmap(cxl, REGION_SZ);
  }
  return 0;
}
