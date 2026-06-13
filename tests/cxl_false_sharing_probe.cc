// CXL same-cacheline false-sharing probe.
//
// Two hosts write disjoint N-byte regions WITHIN THE SAME 64-byte cacheline.
// After both publishes, host 0 reads back the full cacheline and verifies
// that BOTH regions are intact.
//
// Layout (N=8, GAP=8 default):
//   byte:  0  1  2  3  4  5  6  7 | 8  9 10 11 12 13 14 15 | 16 ... 63
//   host:  A  A  A  A  A  A  A  A | B  B  B  B  B  B  B  B | reset 0xC..
//
// Each host writes its rank-tagged pattern:
//   Host A byte i (within its region) = 0xA0 | (i & 0x0F)
//   Host B byte i (within its region) = 0xB0 | (i & 0x0F)
//   Background reset                  = 0xC0 | (i & 0x0F)  (position-indexed)
//
// Outcomes per round:
//   BOTH_OK    — both regions show correct pattern. False sharing safe at
//                this granularity.
//   A_LOST     — A's region got clobbered (shows host B's pattern or reset).
//   B_LOST     — B's region got clobbered.
//   BOTH_LOST  — both regions corrupted/lost.
//   INTRA_INTL — within one region the bytes mix A and B → intra-region
//                interleave.
//   CORRUPT    — index field fails (deeper hw bug).
//
// Mechanism we test: when host A does memcpy + clflushopt of the cacheline,
// the eviction triggers a PCIe write. If x86's implementation uses BYTE
// ENABLES (i.e., writes back only the dirty bytes 0..7), B's bytes 8..15
// are preserved → BOTH_OK. If it writes back the FULL cacheline (with A's
// stale view of bytes 8..15 = 0xC...), B's write gets overwritten →
// B_LOST (last-flusher wins the whole CL).
//
// Same env-var convention as cxl_write_atomicity_probe; uses the same
// header/sync scheme.

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
  uint64_t go_round;    char _p2[64 - 8];
  uint64_t done_a;      char _p3[64 - 8];
  uint64_t done_b;      char _p4[64 - 8];
};
static constexpr uint64_t kMagic = 0x4654534E45524653ULL;  // FRENSTFS

static constexpr size_t kTargetOffset = 4096;
static constexpr size_t kRegionSize = 64 * 1024;

static inline uint8_t encode_byte(int host_id, int i) {
  return (uint8_t)((host_id == 0 ? 0xA0 : 0xB0) | (i & 0x0F));
}
static inline uint8_t reset_byte(int i) {
  return (uint8_t)(0xC0 | (i & 0x0F));
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <dev>\n", argv[0]);
    return 2;
  }
  const char *dev = argv[1];

  int host_id = getenv("FUSEE_HOST_ID") ? atoi(getenv("FUSEE_HOST_ID")) : 0;
  uint64_t cookie = getenv("FUSEE_RUN_COOKIE")
                      ? strtoull(getenv("FUSEE_RUN_COOKIE"), nullptr, 0) : 1;
  int N = getenv("FS_N")    ? atoi(getenv("FS_N"))    : 8;
  int GAP = getenv("FS_GAP") ? atoi(getenv("FS_GAP")) : 0;  // gap between A and B regions
  int K = getenv("FS_K")    ? atoi(getenv("FS_K"))    : 100000;
  int fence_mode = 1;  // 0 = sfence only, 1 = clflushopt + sfence
  if (getenv("FS_FENCE") && !strcmp(getenv("FS_FENCE"), "sfence")) fence_mode = 0;
  int store_mode = 0;  // 0 = memcpy, 1 = movnti
  if (getenv("FS_STORE") && !strcmp(getenv("FS_STORE"), "movnti")) store_mode = 1;

  if (N < 1 || N > 32) { fprintf(stderr, "bad FS_N=%d (must 1..32)\n", N); return 2; }
  if (GAP < 0 || N + GAP + N > 64) {
    fprintf(stderr, "bad layout: N+GAP+N must fit in 64-B CL (got %d)\n",
            N + GAP + N);
    return 2;
  }

  CXLRegion r{};
  if (cxl_region_init(&r, dev, kRegionSize) < 0) {
    fprintf(stderr, "cxl_region_init failed\n"); return 1;
  }
  uint8_t *base = reinterpret_cast<uint8_t *>(r.base);
  Hdr *hdr = reinterpret_cast<Hdr *>(base);
  uint8_t *cell = base + kTargetOffset;  // 64-B aligned start of test CL

  // Compute write pointers:
  //   Host 0 writes at cell[0..N-1]
  //   Host 1 writes at cell[N + GAP .. N + GAP + N - 1]
  int my_offset  = (host_id == 0) ? 0 : (N + GAP);
  uint8_t *my_target = cell + my_offset;

  if (host_id == 0) {
    std::memset(hdr, 0, sizeof(*hdr));
    hdr->magic = kMagic;
    hdr->cookie = cookie;
    for (int i = 0; i < 64; i++) cell[i] = reset_byte(i);
    flush_region(base, kRegionSize);
    store_fence();
  } else {
    while (true) {
      flush_line(&hdr->magic); full_fence();
      flush_line(&hdr->cookie); full_fence();
      if (hdr->magic == kMagic && hdr->cookie == cookie) break;
      __builtin_ia32_pause();
    }
  }

  uint8_t my_pattern[64];
  for (int i = 0; i < N; i++) my_pattern[i] = encode_byte(host_id, i);

  uint64_t n_both_ok = 0, n_a_lost = 0, n_b_lost = 0, n_both_lost = 0;
  uint64_t n_intra_intl = 0, n_corrupt = 0;

  uint8_t obs[64];

  uint64_t t_start = now_ns();

  for (int round = 1; round <= K; round++) {
    if (host_id == 0) {
      // Reset the whole 64-B cell.
      for (int i = 0; i < 64; i++) cell[i] = reset_byte(i);
      flush_line(cell); store_fence();
      hdr->go_round = (uint64_t)round;
      flush_line(&hdr->go_round); store_fence();
    } else {
      while (true) {
        flush_line(&hdr->go_round); full_fence();
        if (hdr->go_round == (uint64_t)round) break;
        __builtin_ia32_pause();
      }
    }

    // RACE: write our N bytes at our offset.
    if (store_mode == 0) {
      std::memcpy(my_target, my_pattern, N);
    } else {
      int i = 0;
      while (i + 8 <= N) {
        uint64_t v; std::memcpy(&v, my_pattern + i, 8);
        __asm__ __volatile__("movnti %1, (%0)" : : "r"(my_target + i), "r"(v) : "memory");
        i += 8;
      }
      if (i < N) std::memcpy(my_target + i, my_pattern + i, N - i);
    }
    if (fence_mode == 0) {
      __asm__ __volatile__("sfence" ::: "memory");
    } else {
      flush_line(my_target);
      __asm__ __volatile__("sfence" ::: "memory");
    }

    if (host_id == 0) {
      hdr->done_a = (uint64_t)round; flush_line(&hdr->done_a); store_fence();
    } else {
      hdr->done_b = (uint64_t)round; flush_line(&hdr->done_b); store_fence();
    }

    if (host_id == 0) {
      while (true) {
        flush_line(&hdr->done_b); full_fence();
        if (hdr->done_b == (uint64_t)round) break;
        __builtin_ia32_pause();
      }

      // Read full 64-B CL.
      flush_line(cell); full_fence();
      std::memcpy(obs, cell, 64);

      // Classify region A and region B separately.
      auto check_region = [&](int off, int len, int expected_top) {
        // Returns: 0 = expected (matches expected top nibble + index),
        //          1 = lost (= reset 0xCx or wrong host's nibble),
        //          2 = intra-region interleave (some A and some B in region)
        bool saw_correct = false, saw_other = false, saw_reset = false;
        bool corrupt = false;
        for (int i = 0; i < len; i++) {
          uint8_t b = obs[off + i];
          uint8_t top = b & 0xF0, lo = b & 0x0F;
          if (lo != (uint8_t)(i & 0x0F)) corrupt = true;
          if (top == (uint8_t)expected_top) saw_correct = true;
          else if (top == (uint8_t)(0xA0 + 0xB0 - expected_top)) saw_other = true;
          else if (top == 0xC0) saw_reset = true;
          else corrupt = true;
        }
        if (corrupt) return 3;
        // Mixed A+B within region.
        if (saw_correct && saw_other) return 2;
        // Whole region is the wrong host's pattern OR still reset.
        if (saw_other || saw_reset) return 1;
        // All correct host's pattern.
        if (saw_correct) return 0;
        return 3;  // shouldn't reach
      };

      int rA = check_region(0,          N, 0xA0);
      int rB = check_region(N + GAP,    N, 0xB0);

      bool intra_intl = (rA == 2) || (rB == 2);
      bool corrupt    = (rA == 3) || (rB == 3);

      if (corrupt) n_corrupt++;
      else if (intra_intl) n_intra_intl++;
      else if (rA == 0 && rB == 0) n_both_ok++;
      else if (rA == 1 && rB == 0) n_a_lost++;
      else if (rA == 0 && rB == 1) n_b_lost++;
      else if (rA == 1 && rB == 1) n_both_lost++;
      else n_corrupt++;
    }
  }

  uint64_t t_end = now_ns();

  if (host_id == 0) {
    printf("FALSE_SHARING n=%d gap=%d store=%s fence=%s K=%d "
           "rounds=%d BOTH_OK=%lu A_LOST=%lu B_LOST=%lu BOTH_LOST=%lu "
           "INTRA_INTL=%lu CORRUPT=%lu wall_us=%lu\n",
           N, GAP,
           store_mode == 0 ? "memcpy" : "movnti",
           fence_mode == 0 ? "sfence" : "clflush_sfence",
           K, K,
           n_both_ok, n_a_lost, n_b_lost, n_both_lost,
           n_intra_intl, n_corrupt,
           (t_end - t_start) / 1000ULL);
    fflush(stdout);
  }
  return 0;
}
