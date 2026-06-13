// CXL cross-host write-atomicity probe.
//
// Two hosts attach to the same /dev/dax0.0. For K rounds, both hosts race
// to write a rank-tagged N-byte pattern into a shared target cell. Host 0
// reads the final pattern and classifies the outcome as
// overwrite_a / overwrite_b / interleave / corruption.
//
// Pattern encoding (so we can distinguish A's bytes from B's bytes anywhere
// in the cell):
//   Host 0 byte i   = 0xA0 | (i & 0x0F)        // top nibble 0xA
//   Host 1 byte i   = 0xB0 | (i & 0x0F)        // top nibble 0xB
//   Reset/background byte i = 0xC0 | (i & 0x0F)  // top nibble 0xC
//
// Classification of N bytes read:
//   - All top nibbles == 0xA AND all low nibbles match index: OVERWRITE_A
//   - All top nibbles == 0xB AND all low nibbles match index: OVERWRITE_B
//   - Any byte has top nibble == 0xC: SOME WRITE DROPPED (counts as
//     OVERWRITE_OTHER but indicates a non-trivial failure mode)
//   - Mixed 0xA and 0xB top nibbles: INTERLEAVE
//   - Any byte fails the low-nibble index check: CORRUPTION
//
// For N > 64, both the full-N outcome AND per-cacheline outcomes are
// classified (the per-cacheline view tells us if LFM v2 can layer N
// independent 64 B flag cells).
//
// Usage:
//   ssh g1: FUSEE_HOST_ID=0 FUSEE_RUN_COOKIE=<c> ATOMICITY_N=8 ATOMICITY_K=1000 \
//             ./cxl_write_atomicity_probe /dev/dax0.0
//   ssh g2: FUSEE_HOST_ID=1 FUSEE_RUN_COOKIE=<c> ATOMICITY_N=8 ATOMICITY_K=1000 \
//             ./cxl_write_atomicity_probe /dev/dax0.0
//
// Output (host 0 only, to stdout, CSV-friendly one line):
//   ATOMICITY n=8 align=aligned store=memcpy fence=sfence loc=same_cl K=1000
//             rounds=1000 OW_A=512 OW_B=488 OW_OTHER=0 INTERLEAVE=0
//             CORRUPT=0 [per-cl OW_A=... ...]
//
// Study: docs/study_cxl_write_atomicity/PLAN.md

#include "cxl_mm.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

extern "C" {
#include "common.h"
}

using fusee::CXLRegion;
using fusee::cxl_region_init;

static inline uint64_t now_ns() {
  timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Header struct on CXL. All fields cacheline-aligned to avoid false sharing.
struct alignas(64) Hdr {
  // Magic + cookie so peers verify they're on the same run.
  uint64_t magic;
  char _p0[64 - 8];

  uint64_t cookie;
  char _p1[64 - 8];

  // Per-round sync. Host 0 bumps go_round to (i+1) to start round i.
  // Both hosts spin on it.
  uint64_t go_round;
  char _p2[64 - 8];

  // Done bits per host per round, expressed as last-completed-round counter.
  uint64_t done_a;
  char _p3[64 - 8];

  uint64_t done_b;
  char _p4[64 - 8];
};

static constexpr uint64_t kMagic = 0x4154434B4D43584CULL;  // "LXCMKCTA" rev

// Target lives at a fixed offset past the header. Reserve 4 KB for any
// alignment / multi-cacheline experiments.
static constexpr size_t kTargetOffset = 4096;
static constexpr size_t kTargetMax = 4096;

// Region size: header + target + spare.
static constexpr size_t kRegionSize = 64 * 1024;

enum AlignMode { ALIGN_ALIGNED, ALIGN_MISALIGNED, ALIGN_STRADDLE_CL };
enum StoreMode { STORE_MEMCPY, STORE_VECTOR, STORE_MOVNTI };
enum FenceMode { FENCE_NONE, FENCE_SFENCE, FENCE_CLFLUSH_SFENCE };
enum LocMode   { LOC_SAME_CL, LOC_CROSS_CL, LOC_PAGE_BOUNDARY };
// Race mode controls the sync model:
//   BARRIER: per-round handshake — host 0 publishes go_round and writes;
//            host 1 spins on go_round, writes when it changes. ~1 µs skew
//            from CXL coherence latency means host 1 always writes second.
//            Tests the "scheduled race" scenario.
//   ASYNC:   both hosts loop tight — each iteration does write+fence and
//            optionally peeks at the address. No per-iteration handshake.
//            Host 0 occasionally samples and classifies. Tests the real
//            LFM-contention scenario where the two writers don't coordinate.
enum RaceMode  { RACE_BARRIER, RACE_ASYNC };

static const char *align_name(int m) {
  switch (m) {
    case ALIGN_ALIGNED:      return "aligned";
    case ALIGN_MISALIGNED:   return "misaligned";
    case ALIGN_STRADDLE_CL:  return "straddle_cl";
    default: return "?";
  }
}
static const char *store_name(int m) {
  switch (m) {
    case STORE_MEMCPY: return "memcpy";
    case STORE_VECTOR: return "vector";
    case STORE_MOVNTI: return "movnti";
    default: return "?";
  }
}
static const char *fence_name(int m) {
  switch (m) {
    case FENCE_NONE:            return "none";
    case FENCE_SFENCE:          return "sfence";
    case FENCE_CLFLUSH_SFENCE:  return "clflush_sfence";
    default: return "?";
  }
}
static const char *loc_name(int m) {
  switch (m) {
    case LOC_SAME_CL:        return "same_cl";
    case LOC_CROSS_CL:       return "cross_cl";
    case LOC_PAGE_BOUNDARY:  return "page_boundary";
    default: return "?";
  }
}

static int parse_align(const char *s) {
  if (!s) return ALIGN_ALIGNED;
  if (!strcmp(s, "aligned")) return ALIGN_ALIGNED;
  if (!strcmp(s, "misaligned")) return ALIGN_MISALIGNED;
  if (!strcmp(s, "straddle_cl")) return ALIGN_STRADDLE_CL;
  return ALIGN_ALIGNED;
}
static int parse_store(const char *s) {
  if (!s) return STORE_MEMCPY;
  if (!strcmp(s, "memcpy")) return STORE_MEMCPY;
  if (!strcmp(s, "vector")) return STORE_VECTOR;
  if (!strcmp(s, "movnti")) return STORE_MOVNTI;
  return STORE_MEMCPY;
}
static int parse_fence(const char *s) {
  if (!s) return FENCE_SFENCE;
  if (!strcmp(s, "none")) return FENCE_NONE;
  if (!strcmp(s, "sfence")) return FENCE_SFENCE;
  if (!strcmp(s, "clflush_sfence")) return FENCE_CLFLUSH_SFENCE;
  return FENCE_SFENCE;
}
static int parse_loc(const char *s) {
  if (!s) return LOC_SAME_CL;
  if (!strcmp(s, "same_cl")) return LOC_SAME_CL;
  if (!strcmp(s, "cross_cl")) return LOC_CROSS_CL;
  if (!strcmp(s, "page_boundary")) return LOC_PAGE_BOUNDARY;
  return LOC_SAME_CL;
}
static int parse_mode(const char *s) {
  if (!s) return RACE_BARRIER;
  if (!strcmp(s, "barrier")) return RACE_BARRIER;
  if (!strcmp(s, "async")) return RACE_ASYNC;
  return RACE_BARRIER;
}
static const char *mode_name(int m) {
  switch (m) {
    case RACE_BARRIER: return "barrier";
    case RACE_ASYNC:   return "async";
    default: return "?";
  }
}

// Compute target pointer based on loc + align mode.
static uint8_t *target_ptr(uint8_t *cell_base, int N, int loc, int align) {
  // cell_base points at the start of the 4 KB target region.
  uint8_t *p = cell_base;
  switch (loc) {
    case LOC_SAME_CL: {
      // Pin target within a single 64 B cacheline at offset 0 of cell_base.
      p = cell_base;
      if (align == ALIGN_MISALIGNED && N < 64) {
        // Misalign by 1 byte within the same cacheline (if it fits).
        size_t room = 64 - 1;
        if ((size_t)N <= room) p = cell_base + 1;
      } else if (align == ALIGN_STRADDLE_CL && N >= 2 && N <= 64) {
        // Position so that the N-byte cell straddles the 64 B boundary.
        p = cell_base + 64 - (N / 2);
      }
      break;
    }
    case LOC_CROSS_CL: {
      // For N > 64, just place at a cacheline-aligned start; the write
      // necessarily spans (N + 63) / 64 cachelines. Misalignment also OK.
      p = cell_base;
      if (align == ALIGN_MISALIGNED && N > 64) p = cell_base + 1;
      break;
    }
    case LOC_PAGE_BOUNDARY: {
      // 4 KB page boundary is at cell_base + 2048 (cell_base must be page-
      // aligned for this to be meaningful; we trust cxl_region_init for it).
      // Straddle the 4 KB boundary at midpoint of [cell_base, cell_base+4096).
      p = cell_base + 2048 - (N / 2);
      break;
    }
  }
  return p;
}

// Encode a rank-tagged byte: top nibble identifies the writer, low nibble
// indexes the position so partial-byte corruption can be detected.
static inline uint8_t encode_byte(int host_id, int i) {
  uint8_t top = (host_id == 0) ? 0xA0 : 0xB0;
  return top | (uint8_t)(i & 0x0F);
}
static inline uint8_t reset_byte(int i) { return 0xC0 | (uint8_t)(i & 0x0F); }

// Classify N bytes. Returns one of:
//   0 = OVERWRITE_A, 1 = OVERWRITE_B, 2 = OVERWRITE_OTHER (saw 0xCx leftover),
//   3 = INTERLEAVE, 4 = CORRUPTION
static int classify(const uint8_t *buf, int N) {
  int saw_a = 0, saw_b = 0, saw_c = 0;
  int corrupt = 0;
  for (int i = 0; i < N; i++) {
    uint8_t b = buf[i];
    uint8_t top = b & 0xF0, lo = b & 0x0F;
    if ((uint8_t)(i & 0x0F) != lo) corrupt = 1;
    if (top == 0xA0) saw_a = 1;
    else if (top == 0xB0) saw_b = 1;
    else if (top == 0xC0) saw_c = 1;
    else corrupt = 1;
  }
  if (corrupt) return 4;
  if (saw_a && saw_b) return 3;       // INTERLEAVE
  if (saw_c) return 2;                // OVERWRITE_OTHER (some byte never overwritten)
  if (saw_a && !saw_b) return 0;      // OVERWRITE_A
  if (saw_b && !saw_a) return 1;      // OVERWRITE_B
  return 4;                            // shouldn't reach
}

static void do_store(uint8_t *dst, const uint8_t *src, int N, int store_mode) {
  switch (store_mode) {
    case STORE_VECTOR: {
      // Use AVX2 32 B unaligned stores when N >= 32, otherwise plain memcpy.
      int i = 0;
      while (i + 32 <= N) {
        __asm__ __volatile__("vmovdqu (%1), %%ymm0\n\t"
                              "vmovdqu %%ymm0, (%0)\n\t"
                              :
                              : "r"(dst + i), "r"(src + i)
                              : "ymm0", "memory");
        i += 32;
      }
      if (i < N) std::memcpy(dst + i, src + i, N - i);
      break;
    }
    case STORE_MOVNTI: {
      // Non-temporal 8 B stores. Falls back to memcpy for sub-8 B tails.
      int i = 0;
      while (i + 8 <= N) {
        uint64_t v;
        std::memcpy(&v, src + i, 8);
        __asm__ __volatile__("movnti %1, (%0)\n\t"
                              :
                              : "r"(dst + i), "r"(v)
                              : "memory");
        i += 8;
      }
      if (i < N) std::memcpy(dst + i, src + i, N - i);
      break;
    }
    default:
      std::memcpy(dst, src, N);
      break;
  }
}

static void do_fence(uint8_t *dst, int N, int fence_mode) {
  switch (fence_mode) {
    case FENCE_NONE: break;
    case FENCE_SFENCE:
      __asm__ __volatile__("sfence" ::: "memory");
      break;
    case FENCE_CLFLUSH_SFENCE: {
      uint8_t *p = reinterpret_cast<uint8_t *>(
          reinterpret_cast<uintptr_t>(dst) & ~(uintptr_t)63);
      uint8_t *end = dst + N;
      while (p < end) { flush_line(p); p += 64; }
      __asm__ __volatile__("sfence" ::: "memory");
      break;
    }
  }
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
  int N = getenv("ATOMICITY_N") ? atoi(getenv("ATOMICITY_N")) : 8;
  int K = getenv("ATOMICITY_K") ? atoi(getenv("ATOMICITY_K")) : 1000;
  int align_mode = parse_align(getenv("ATOMICITY_ALIGN"));
  int store_mode = parse_store(getenv("ATOMICITY_STORE"));
  int fence_mode = parse_fence(getenv("ATOMICITY_FENCE"));
  int loc_mode   = parse_loc(getenv("ATOMICITY_LOC"));
  int race_mode  = parse_mode(getenv("ATOMICITY_MODE"));
  // Async-mode race duration (per round). Each round, both hosts write
  // continuously for this many µs; then host 0 samples and classifies once.
  uint64_t async_race_us = getenv("ATOMICITY_ASYNC_RACE_US")
                              ? strtoull(getenv("ATOMICITY_ASYNC_RACE_US"), nullptr, 0)
                              : 200ULL;

  if (N < 1 || N > (int)kTargetMax) {
    fprintf(stderr, "bad N=%d\n", N); return 2;
  }
  if (K < 1 || K > 1000000) { fprintf(stderr, "bad K=%d\n", K); return 2; }

  CXLRegion r{};
  if (cxl_region_init(&r, dev, kRegionSize) < 0) {
    fprintf(stderr, "cxl_region_init failed\n"); return 1;
  }
  uint8_t *base = reinterpret_cast<uint8_t *>(r.base);
  Hdr *hdr = reinterpret_cast<Hdr *>(base);
  uint8_t *cell_base = base + kTargetOffset;
  uint8_t *target = target_ptr(cell_base, N, loc_mode, align_mode);

  // Host 0 initializes header.
  if (host_id == 0) {
    std::memset(hdr, 0, sizeof(*hdr));
    hdr->magic = kMagic;
    hdr->cookie = cookie;
    hdr->go_round = 0;
    hdr->done_a = 0;
    hdr->done_b = 0;
    // Reset target region.
    for (int i = 0; i < (int)kTargetMax; i++) cell_base[i] = reset_byte(i);
    flush_region(base, kRegionSize);
    store_fence();
  } else {
    // Wait for host 0 to publish cookie.
    while (true) {
      flush_line(&hdr->magic); full_fence();
      flush_line(&hdr->cookie); full_fence();
      if (hdr->magic == kMagic && hdr->cookie == cookie) break;
      __builtin_ia32_pause();
    }
  }

  // Build per-host write buffer (encoded pattern in DRAM, copied per round).
  uint8_t my_pattern[kTargetMax];
  for (int i = 0; i < N; i++) my_pattern[i] = encode_byte(host_id, i);

  // Tally counters (host 0 only).
  uint64_t ow_a = 0, ow_b = 0, ow_other = 0, interleave = 0, corrupt = 0;
  // Per-cacheline tally for N > 64.
  int n_cls = (N + 63) / 64;
  if (n_cls > 16) n_cls = 16;
  uint64_t cl_ow_a[16] = {0}, cl_ow_b[16] = {0}, cl_ow_other[16] = {0};
  uint64_t cl_interleave[16] = {0}, cl_corrupt[16] = {0};

  // Reader scratch buffer (DRAM).
  uint8_t obs[kTargetMax];

  uint64_t t_start = now_ns();

  auto sample_classify = [&]() {
    uint8_t *flush_start = reinterpret_cast<uint8_t *>(
        reinterpret_cast<uintptr_t>(target) & ~(uintptr_t)63);
    uint8_t *flush_end = target + N + 64;
    for (uint8_t *p = flush_start; p < flush_end; p += 64) flush_line(p);
    full_fence();
    std::memcpy(obs, target, N);
    int kind = classify(obs, N);
    if (kind == 0) ow_a++;
    else if (kind == 1) ow_b++;
    else if (kind == 2) ow_other++;
    else if (kind == 3) interleave++;
    else corrupt++;
    if (n_cls > 1) {
      for (int cl = 0; cl < n_cls; cl++) {
        int off = cl * 64;
        int sz = (N - off < 64) ? (N - off) : 64;
        int cl_kind = classify(obs + off, sz);
        if (cl_kind == 0) cl_ow_a[cl]++;
        else if (cl_kind == 1) cl_ow_b[cl]++;
        else if (cl_kind == 2) cl_ow_other[cl]++;
        else if (cl_kind == 3) cl_interleave[cl]++;
        else cl_corrupt[cl]++;
      }
    }
  };

  // ASYNC mode: K rounds, each round = (a) reset target (host 0), (b)
  // both hosts write continuously for `async_race_us` µs, (c) host 0
  // samples once. Per-round go_round / done bits used only to bracket
  // the race window — no per-iteration coordination.
  if (race_mode == RACE_ASYNC) {
    for (int round = 1; round <= K; round++) {
      if (host_id == 0) {
        for (int i = 0; i < N + 128; i++) {
          if (target + i < base + kRegionSize)
            target[i] = reset_byte(i);
        }
        uint8_t *flush_start = reinterpret_cast<uint8_t *>(
            reinterpret_cast<uintptr_t>(target) & ~(uintptr_t)63);
        uint8_t *flush_end = target + N + 64;
        for (uint8_t *p = flush_start; p < flush_end; p += 64) flush_line(p);
        store_fence();
        hdr->go_round = (uint64_t)round;
        flush_line(&hdr->go_round); store_fence();
      } else {
        while (true) {
          flush_line(&hdr->go_round); full_fence();
          if (hdr->go_round == (uint64_t)round) break;
          __builtin_ia32_pause();
        }
      }
      // Both hosts now race write the pattern continuously for the
      // configured µs window.
      uint64_t race_deadline_ns = now_ns() + async_race_us * 1000ULL;
      while (now_ns() < race_deadline_ns) {
        do_store(target, my_pattern, N, store_mode);
        do_fence(target, N, fence_mode);
      }
      // Publish done.
      if (host_id == 0) {
        hdr->done_a = (uint64_t)round;
        flush_line(&hdr->done_a); store_fence();
      } else {
        hdr->done_b = (uint64_t)round;
        flush_line(&hdr->done_b); store_fence();
      }
      if (host_id == 0) {
        while (true) {
          flush_line(&hdr->done_b); full_fence();
          if (hdr->done_b == (uint64_t)round) break;
          __builtin_ia32_pause();
        }
        // Tiny extra pause to let the last in-flight stores from peer
        // settle on CXL before we sample.
        for (int p = 0; p < 256; p++) __builtin_ia32_pause();
        sample_classify();
      }
    }
    goto print_summary;
  }

  for (int round = 1; round <= K; round++) {
    if (host_id == 0) {
      // Reset target.
      for (int i = 0; i < N + 128; i++) {
        if (target + i < base + kRegionSize)
          target[i] = reset_byte(i);
      }
      // Flush so peer sees clean state when it sees go_round.
      uint8_t *flush_start = reinterpret_cast<uint8_t *>(
          reinterpret_cast<uintptr_t>(target) & ~(uintptr_t)63);
      uint8_t *flush_end = target + N + 64;
      for (uint8_t *p = flush_start; p < flush_end; p += 64) flush_line(p);
      store_fence();

      // Release go_round.
      hdr->go_round = (uint64_t)round;
      flush_line(&hdr->go_round);
      store_fence();
    } else {
      // Wait for go_round.
      while (true) {
        flush_line(&hdr->go_round); full_fence();
        if (hdr->go_round == (uint64_t)round) break;
        __builtin_ia32_pause();
      }
    }

    // RACE: write the pattern.
    do_store(target, my_pattern, N, store_mode);
    do_fence(target, N, fence_mode);

    // Publish done.
    if (host_id == 0) {
      hdr->done_a = (uint64_t)round;
      flush_line(&hdr->done_a); store_fence();
    } else {
      hdr->done_b = (uint64_t)round;
      flush_line(&hdr->done_b); store_fence();
    }

    if (host_id == 0) {
      // Wait for peer's done bit.
      while (true) {
        flush_line(&hdr->done_b); full_fence();
        if (hdr->done_b == (uint64_t)round) break;
        __builtin_ia32_pause();
      }

      // Pull target bytes fresh from CXL.
      uint8_t *flush_start = reinterpret_cast<uint8_t *>(
          reinterpret_cast<uintptr_t>(target) & ~(uintptr_t)63);
      uint8_t *flush_end = target + N + 64;
      for (uint8_t *p = flush_start; p < flush_end; p += 64) flush_line(p);
      full_fence();
      std::memcpy(obs, target, N);

      // Classify full-N.
      int kind = classify(obs, N);
      if (kind == 0) ow_a++;
      else if (kind == 1) ow_b++;
      else if (kind == 2) ow_other++;
      else if (kind == 3) interleave++;
      else corrupt++;

      // Per-cacheline classification (for multi-cacheline N).
      if (n_cls > 1) {
        for (int cl = 0; cl < n_cls; cl++) {
          int off = cl * 64;
          int sz = (N - off < 64) ? (N - off) : 64;
          int cl_kind = classify(obs + off, sz);
          if (cl_kind == 0) cl_ow_a[cl]++;
          else if (cl_kind == 1) cl_ow_b[cl]++;
          else if (cl_kind == 2) cl_ow_other[cl]++;
          else if (cl_kind == 3) cl_interleave[cl]++;
          else cl_corrupt[cl]++;
        }
      }
    }
  }

print_summary:
  uint64_t t_end = now_ns();

  if (host_id == 0) {
    // Print CSV-friendly summary on a single line.
    printf("ATOMICITY n=%d align=%s store=%s fence=%s loc=%s mode=%s K=%d "
           "rounds=%d OW_A=%lu OW_B=%lu OW_OTHER=%lu INTERLEAVE=%lu "
           "CORRUPT=%lu wall_us=%lu",
           N, align_name(align_mode), store_name(store_mode),
           fence_name(fence_mode), loc_name(loc_mode), mode_name(race_mode),
           K, K,
           ow_a, ow_b, ow_other, interleave, corrupt,
           (t_end - t_start) / 1000ULL);

    if (n_cls > 1) {
      for (int cl = 0; cl < n_cls; cl++) {
        printf(" cl%d=OW_A=%lu,OW_B=%lu,OW_OTHER=%lu,INTERLEAVE=%lu,CORRUPT=%lu",
               cl, cl_ow_a[cl], cl_ow_b[cl], cl_ow_other[cl],
               cl_interleave[cl], cl_corrupt[cl]);
      }
    }
    printf("\n");
    fflush(stdout);
  }

  return 0;
}
