// CXL concurrent reader probe.
//
// Host 0 (writer) alternates publishing pattern P_A vs P_B into a 64-B
// target. Host 1 (reader) continuously clflushopt+mfence+load the target
// and classifies each read. We look for "torn" reads — where the
// observed bytes are a mix of P_A and P_B (or contain reset bytes that
// shouldn't be there).
//
// Pattern encoding:
//   P_A (pure A) byte i = 0xA0 | (i & 0x0F)
//   P_B (pure B) byte i = 0xB0 | (i & 0x0F)
//   reset/init   byte i = 0xC0 | (i & 0x0F)
//
// Writer cycle (per iteration):
//   1. memcpy P_A into target
//   2. clflushopt + sfence to publish
//   3. memcpy P_B into target
//   4. clflushopt + sfence to publish
// → reader should never observe a mix; only pure P_A, pure P_B, or
//   possibly the prior cycle's pattern.
//
// Reader cycle (per iteration):
//   1. clflushopt the target cacheline(s)
//   2. mfence
//   3. memcpy target bytes into local buffer
//   4. classify
//
// Outcomes (counted on reader side):
//   OBS_A     — all bytes match P_A
//   OBS_B     — all bytes match P_B
//   OBS_RESET — saw reset/zero bytes (publishes haven't started)
//   TORN_AB   — mix of P_A and P_B top nibbles
//   TORN_OTHER— mix involving reset bytes (publisher mid-write)
//   CORRUPT   — low nibble position-check failed
//
// Both hosts loop for a fixed wall-clock window (ATOMICITY_RACE_US,
// default 1 000 000 = 1 s). At end host 1 prints the histogram.

#include "cxl_mm.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

extern "C" {
#include "common.h"
}

using fusee::CXLRegion;
using fusee::cxl_region_init;

static inline uint64_t now_ns() {
  timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

struct alignas(64) Hdr {
  uint64_t magic;       char _p0[64 - 8];
  uint64_t cookie;      char _p1[64 - 8];
  uint64_t go;          char _p2[64 - 8];
  uint64_t writer_done; char _p3[64 - 8];
  uint64_t reader_done; char _p4[64 - 8];
};
static constexpr uint64_t kMagic = 0x434E435252454144ULL;  // CNCRREAD

static constexpr size_t kTargetOffset = 4096;
static constexpr size_t kRegionSize = 64 * 1024;

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <dev>\n", argv[0]); return 2;
  }
  const char *dev = argv[1];

  int host_id = getenv("FUSEE_HOST_ID") ? atoi(getenv("FUSEE_HOST_ID")) : 0;
  uint64_t cookie = getenv("FUSEE_RUN_COOKIE")
                      ? strtoull(getenv("FUSEE_RUN_COOKIE"), nullptr, 0) : 1;
  int N = getenv("CR_N") ? atoi(getenv("CR_N")) : 64;
  uint64_t race_us = getenv("CR_RACE_US")
                      ? strtoull(getenv("CR_RACE_US"), nullptr, 0)
                      : 1000000ULL;
  if (N < 1 || N > 1024) { fprintf(stderr, "bad CR_N=%d\n", N); return 2; }

  CXLRegion r{};
  if (cxl_region_init(&r, dev, kRegionSize) < 0) {
    fprintf(stderr, "cxl_region_init failed\n"); return 1;
  }
  uint8_t *base = reinterpret_cast<uint8_t *>(r.base);
  Hdr *hdr = reinterpret_cast<Hdr *>(base);
  uint8_t *target = base + kTargetOffset;

  uint8_t pat_a[1024], pat_b[1024], pat_reset[1024];
  for (int i = 0; i < N; i++) {
    pat_a[i]     = (uint8_t)(0xA0 | (i & 0x0F));
    pat_b[i]     = (uint8_t)(0xB0 | (i & 0x0F));
    pat_reset[i] = (uint8_t)(0xC0 | (i & 0x0F));
  }

  if (host_id == 0) {
    std::memset(hdr, 0, sizeof(*hdr));
    hdr->magic = kMagic; hdr->cookie = cookie;
    std::memcpy(target, pat_reset, N);
    flush_region(base, kRegionSize); store_fence();
  } else {
    while (true) {
      flush_line(&hdr->magic); full_fence();
      flush_line(&hdr->cookie); full_fence();
      if (hdr->magic == kMagic && hdr->cookie == cookie) break;
      __builtin_ia32_pause();
    }
  }

  // Host 0 publishes "go"; host 1 waits for it.
  if (host_id == 0) {
    hdr->go = 1; flush_line(&hdr->go); store_fence();
  } else {
    while (true) {
      flush_line(&hdr->go); full_fence();
      if (hdr->go == 1) break;
      __builtin_ia32_pause();
    }
  }

  uint64_t deadline = now_ns() + race_us * 1000ULL;
  uint64_t iters = 0;
  uint64_t n_obs_a = 0, n_obs_b = 0, n_obs_reset = 0;
  uint64_t n_torn_ab = 0, n_torn_other = 0, n_corrupt = 0;

  if (host_id == 0) {
    // Writer loop: cycle A → flush → B → flush
    while (now_ns() < deadline) {
      std::memcpy(target, pat_a, N);
      // Flush every cacheline in [target, target + N)
      uint8_t *p = reinterpret_cast<uint8_t *>(
          reinterpret_cast<uintptr_t>(target) & ~(uintptr_t)63);
      uint8_t *end = target + N;
      while (p < end) { flush_line(p); p += 64; }
      __asm__ __volatile__("sfence" ::: "memory");

      std::memcpy(target, pat_b, N);
      p = reinterpret_cast<uint8_t *>(
          reinterpret_cast<uintptr_t>(target) & ~(uintptr_t)63);
      while (p < end) { flush_line(p); p += 64; }
      __asm__ __volatile__("sfence" ::: "memory");
      iters++;
    }
    hdr->writer_done = 1; flush_line(&hdr->writer_done); store_fence();
    // Wait for reader done before exit
    while (true) {
      flush_line(&hdr->reader_done); full_fence();
      if (hdr->reader_done == 1) break;
      __builtin_ia32_pause();
    }
    printf("WRITER iters=%lu race_us=%lu\n", iters, race_us);
    fflush(stdout);
  } else {
    uint8_t obs[1024];
    while (now_ns() < deadline) {
      // Flush + mfence + load
      uint8_t *p = reinterpret_cast<uint8_t *>(
          reinterpret_cast<uintptr_t>(target) & ~(uintptr_t)63);
      uint8_t *end = target + N;
      while (p < end) { flush_line(p); p += 64; }
      __asm__ __volatile__("mfence" ::: "memory");
      std::memcpy(obs, target, N);

      // Classify
      bool saw_a = false, saw_b = false, saw_reset = false, corrupt = false;
      for (int i = 0; i < N; i++) {
        uint8_t b = obs[i];
        uint8_t top = b & 0xF0, lo = b & 0x0F;
        if (lo != (uint8_t)(i & 0x0F)) corrupt = true;
        if (top == 0xA0) saw_a = true;
        else if (top == 0xB0) saw_b = true;
        else if (top == 0xC0) saw_reset = true;
        else corrupt = true;
      }
      if (corrupt) n_corrupt++;
      else if (saw_a && saw_b) n_torn_ab++;
      else if ((saw_a || saw_b) && saw_reset) n_torn_other++;
      else if (saw_a && !saw_b && !saw_reset) n_obs_a++;
      else if (saw_b && !saw_a && !saw_reset) n_obs_b++;
      else if (saw_reset && !saw_a && !saw_b) n_obs_reset++;
      else n_corrupt++;
      iters++;
    }
    hdr->reader_done = 1; flush_line(&hdr->reader_done); store_fence();
    while (true) {
      flush_line(&hdr->writer_done); full_fence();
      if (hdr->writer_done == 1) break;
      __builtin_ia32_pause();
    }
    printf("READER n=%d iters=%lu race_us=%lu "
           "OBS_A=%lu OBS_B=%lu OBS_RESET=%lu "
           "TORN_AB=%lu TORN_OTHER=%lu CORRUPT=%lu\n",
           N, iters, race_us,
           n_obs_a, n_obs_b, n_obs_reset,
           n_torn_ab, n_torn_other, n_corrupt);
    fflush(stdout);
  }
  return 0;
}
