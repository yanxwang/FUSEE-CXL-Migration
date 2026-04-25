// Dual-host CXL aggregate write-bandwidth microbench.
// Both hosts simultaneously stream nontemporal AVX-512 writes to disjoint
// halves of a shared CXL devdax region for a fixed wall-clock duration.
// The aggregate result tells us whether the expander supports 2x per-host
// bandwidth or shares 51.78 GB/s.
//
// Usage:
//   ssh g3: FUSEE_HOST_ID=0 FUSEE_RUN_COOKIE=<c> ./cxl_dualhost_bw_bench /dev/dax0.0 <region_GiB> <run_seconds>
//   ssh g4: FUSEE_HOST_ID=1 FUSEE_RUN_COOKIE=<c> ./cxl_dualhost_bw_bench /dev/dax0.0 <region_GiB> <run_seconds>

#include "cxl_mm.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <immintrin.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" {
#include "common.h"
}

using fusee::CXLRegion;
using fusee::cxl_region_destroy;
using fusee::cxl_region_init;

static inline uint64_t now_ns() {
  timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: %s <dev> <region_GiB> <run_seconds> [<num_threads>]\n", argv[0]);
    return 2;
  }
  const char *dev = argv[1];
  size_t region_gib = (size_t)atoll(argv[2]);
  double run_seconds = atof(argv[3]);
  int num_threads = (argc >= 5) ? atoi(argv[4]) : 1;
  if (num_threads < 1) num_threads = 1;
  int host_id = getenv("FUSEE_HOST_ID") ? atoi(getenv("FUSEE_HOST_ID")) : 0;
  uint64_t cookie = getenv("FUSEE_RUN_COOKIE") ?
                      strtoull(getenv("FUSEE_RUN_COOKIE"), nullptr, 0) : 0;

  const size_t total_bytes = region_gib * (1ULL << 30);
  CXLRegion r{};
  if (cxl_region_init(&r, dev, total_bytes + (1ULL << 20)) < 0) {
    fprintf(stderr, "cxl_region_init failed\n"); return 1;
  }

  // Layout: [coord cachelines | host0 half | host1 half]
  // Coord block: cookie at +0, host0_done at +64, host1_done at +128, host0_bytes at +192, host1_bytes at +256.
  auto *cookie_slot   = reinterpret_cast<cacheline_u64 *>((char *)r.base + 0);
  auto *done_host0    = reinterpret_cast<cacheline_u64 *>((char *)r.base + 64);
  auto *done_host1    = reinterpret_cast<cacheline_u64 *>((char *)r.base + 128);
  auto *bytes_host0   = reinterpret_cast<cacheline_u64 *>((char *)r.base + 192);
  auto *bytes_host1   = reinterpret_cast<cacheline_u64 *>((char *)r.base + 256);
  const size_t coord_bytes = 1024;
  const size_t half = (total_bytes - coord_bytes) / 2;
  const size_t half_aligned = half & ~(size_t)63;
  char *bench_base = (char *)r.base + coord_bytes;
  char *my_region = bench_base + (host_id == 0 ? 0 : half_aligned);

  if (host_id == 0) {
    CACHELINE_STORE(done_host0, 0ULL);
    CACHELINE_STORE(done_host1, 0ULL);
    CACHELINE_STORE(bytes_host0, 0ULL);
    CACHELINE_STORE(bytes_host1, 0ULL);
    flush_region(r.base, 320);
    store_fence();
    CACHELINE_STORE(cookie_slot, cookie);
    fprintf(stderr, "[h0] coord ready, cookie=%lu region=%zuGiB half=%zu\n",
            cookie, region_gib, half_aligned);
  } else {
    fprintf(stderr, "[h1] waiting for cookie=%lu\n", cookie);
    while (CACHELINE_LOAD(cookie_slot) != cookie) __builtin_ia32_pause();
    fprintf(stderr, "[h1] saw cookie\n");
  }

  // Stream 64-byte cachelines via AVX-512 nontemporal stores. Each thread
  // owns a contiguous slice of my_region. Threads start at the same time and
  // run for `run_seconds`. Total bytes is the sum of per-thread bytes.
  __m512i payload = _mm512_set1_epi64((long long)0x9e3779b97f4a7c15ULL ^ host_id);
  std::atomic<uint64_t> total_bytes_written{0};
  std::atomic<bool> start_flag{false};
  std::atomic<bool> stop_flag{false};
  uint64_t t0 = 0;

  auto worker = [&](int tid) {
    size_t slice = half_aligned / num_threads;
    slice &= ~(size_t)63;
    char *base = my_region + (size_t)tid * slice;
    while (!start_flag.load(std::memory_order_acquire)) __builtin_ia32_pause();
    uint64_t bytes = 0;
    size_t off = 0;
    while (!stop_flag.load(std::memory_order_relaxed)) {
      char *p = base + off;
      for (int i = 0; i < 64; i++) {
        _mm512_stream_si512((__m512i *)(p + (i << 6)), payload);
      }
      bytes += 64ULL * 64ULL;
      off += 64 * 64;
      if (off + 64 * 64 >= slice) off = 0;
    }
    _mm_sfence();
    total_bytes_written.fetch_add(bytes, std::memory_order_relaxed);
  };

  std::vector<std::thread> ths;
  for (int t = 0; t < num_threads; t++) ths.emplace_back(worker, t);
  // Tiny delay to let all threads spin up to the start_flag.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  t0 = now_ns();
  start_flag.store(true, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::duration<double>(run_seconds));
  stop_flag.store(true, std::memory_order_relaxed);
  for (auto &th : ths) th.join();
  uint64_t t1 = now_ns();
  uint64_t bytes_written = total_bytes_written.load();
  _mm_sfence();

  double sec = (t1 - t0) / 1e9;
  double gbps = (double)bytes_written / 1e9 / sec;
  fprintf(stderr, "[h%d] threads=%d wrote %.3f GB in %.3fs => %.2f GB/s\n",
          host_id, num_threads, bytes_written / 1e9, sec, gbps);

  // Publish my byte count + done flag.
  auto *my_bytes = (host_id == 0) ? bytes_host0 : bytes_host1;
  auto *my_done  = (host_id == 0) ? done_host0  : done_host1;
  auto *peer_bytes = (host_id == 0) ? bytes_host1 : bytes_host0;
  auto *peer_done  = (host_id == 0) ? done_host1  : done_host0;
  CACHELINE_STORE(my_bytes, bytes_written);
  CACHELINE_STORE(my_done, 1ULL);

  // Host 0 waits for host 1 done, prints joint result.
  if (host_id == 0) {
    while (CACHELINE_LOAD(peer_done) == 0) __builtin_ia32_pause();
    uint64_t b0 = bytes_written;
    uint64_t b1 = CACHELINE_LOAD(peer_bytes);
    double gbps0 = (double)b0 / 1e9 / sec;
    double gbps1 = (double)b1 / 1e9 / sec;
    double agg = gbps0 + gbps1;
    printf("M1 dual-host BW: host0=%.2f GB/s host1=%.2f GB/s aggregate=%.2f GB/s "
           "duration=%.2fs region_GiB=%zu threads_per_host=%d\n",
           gbps0, gbps1, agg, sec, region_gib, num_threads);
    // Compare against per-host 51.78 GB/s ceiling (mlc).
    printf("M1 ratio_to_per_host_ceiling=%.2fx (per-host=51.78 GB/s)\n",
           agg / 51.78);
  }

  cxl_region_destroy(&r);
  return 0;
}
