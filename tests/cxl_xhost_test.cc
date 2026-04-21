// Cross-host CXL shared-region verification.
//
// Two machines run this binary against the same /dev/dax0.0 (assumed to be
// the same physical bytes via a shared CXL fabric). Host 0 writes a magic
// pattern, sets a handshake word, and waits for an ACK. Host 1 spins on
// the handshake, verifies the bytes, writes its own ACK, and exits.
//
// Usage:
//   On each host:  FUSEE_HOST_ID={0|1} ./cxl_xhost_test <dev_path>
//
// Exit code 0 means the fabric really does share the bytes. Non-zero on
// either host means the two devices are NOT the same memory.
//
// Region layout (only first ~1 KiB touched):
//   [0   .. 64)   : handshake word  (host 0 -> 1, value = MAGIC_H0)
//   [64  .. 128)  : ack word        (host 1 -> 0, value = MAGIC_H1)
//   [128 .. 896)  : 12 × 64-byte cacheline payload (host 0 writes distinct
//                                                   pattern per line,
//                                                   host 1 verifies)

#include "cxl_mm.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>

extern "C" {
#include "common.h"
}

using fusee::CXLRegion;
using fusee::cxl_region_destroy;
using fusee::cxl_region_init;

namespace {
constexpr uint64_t kMagicH0 = 0xC0DEB00BA1CE1337ULL;
constexpr uint64_t kMagicH1 = 0xDEADD00D5EA150DAULL;
constexpr uint64_t kPatternSeed = 0xABCDEF0123456789ULL;

constexpr size_t kHandshakeOff = 0;
constexpr size_t kAckOff       = 64;
constexpr size_t kPayloadOff   = 128;
constexpr int    kPayloadLines = 12;

uint64_t now_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t pattern_for(int line, int word_in_line) {
  return kPatternSeed ^ ((uint64_t)(line + 1) * 0x9E3779B97F4A7C15ULL)
                      ^ ((uint64_t)word_in_line * 0x517CC1B727220A95ULL);
}

void write_payload(void *base) {
  auto *p = reinterpret_cast<uint64_t *>(
      reinterpret_cast<char *>(base) + kPayloadOff);
  for (int line = 0; line < kPayloadLines; line++) {
    for (int w = 0; w < 8; w++) {
      p[line * 8 + w] = pattern_for(line, w);
    }
  }
  flush_region(reinterpret_cast<char *>(base) + kPayloadOff,
               kPayloadLines * 64);
  store_fence();
}

int verify_payload(void *base) {
  auto *p = reinterpret_cast<uint64_t *>(
      reinterpret_cast<char *>(base) + kPayloadOff);
  int bad = 0;
  for (int line = 0; line < kPayloadLines; line++) {
    flush_line(p + line * 8);
  }
  full_fence();
  for (int line = 0; line < kPayloadLines; line++) {
    for (int w = 0; w < 8; w++) {
      uint64_t got = p[line * 8 + w];
      uint64_t want = pattern_for(line, w);
      if (got != want) {
        fprintf(stderr, "MISMATCH line=%d word=%d got=%016lx want=%016lx\n",
                line, w, got, want);
        bad++;
      }
    }
  }
  return bad;
}
} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: FUSEE_HOST_ID={0|1} %s <dev_path>\n", argv[0]);
    return 2;
  }
  const char *role_env = getenv("FUSEE_HOST_ID");
  if (!role_env) {
    fprintf(stderr, "FUSEE_HOST_ID must be set (0 or 1)\n");
    return 2;
  }
  int host = atoi(role_env);
  if (host != 0 && host != 1) {
    fprintf(stderr, "FUSEE_HOST_ID must be 0 or 1 (got %d)\n", host);
    return 2;
  }

  CXLRegion r{};
  if (cxl_region_init(&r, argv[1], 4UL * 1024 * 1024) < 0) {
    fprintf(stderr, "[host %d] cxl_region_init failed\n", host);
    return 1;
  }

  auto *hs  = reinterpret_cast<cacheline_u64 *>(
      reinterpret_cast<char *>(r.base) + kHandshakeOff);
  auto *ack = reinterpret_cast<cacheline_u64 *>(
      reinterpret_cast<char *>(r.base) + kAckOff);

  const int kSpinUs     = 200;
  const int kTimeoutS   = 60;

  if (host == 0) {
    // Reset both words, then write payload, then publish handshake.
    CACHELINE_STORE(hs, 0ULL);
    CACHELINE_STORE(ack, 0ULL);
    write_payload(r.base);
    CACHELINE_STORE(hs, kMagicH0);
    printf("[host 0] wrote payload + handshake; waiting for ACK...\n");

    uint64_t t0 = now_ns();
    for (;;) {
      uint64_t got = CACHELINE_LOAD(ack);
      if (got == kMagicH1) {
        double wait_s = (now_ns() - t0) / 1e9;
        printf("[host 0] got ACK after %.3fs\n", wait_s);
        cxl_region_destroy(&r);
        printf("OK: cross-host CXL fabric verified on %s\n", argv[1]);
        return 0;
      }
      if ((now_ns() - t0) / 1000000000ULL > (uint64_t)kTimeoutS) {
        fprintf(stderr,
                "[host 0] TIMEOUT after %ds waiting for ACK "
                "(peer did not see our writes → fabric NOT shared)\n",
                kTimeoutS);
        cxl_region_destroy(&r);
        return 1;
      }
      usleep(kSpinUs);
    }
  } else {
    // host 1: spin on handshake, verify, publish ACK.
    printf("[host 1] waiting for peer's handshake...\n");
    uint64_t t0 = now_ns();
    for (;;) {
      uint64_t got = CACHELINE_LOAD(hs);
      if (got == kMagicH0) break;
      if ((now_ns() - t0) / 1000000000ULL > (uint64_t)kTimeoutS) {
        fprintf(stderr,
                "[host 1] TIMEOUT after %ds waiting for peer handshake\n",
                kTimeoutS);
        cxl_region_destroy(&r);
        return 1;
      }
      usleep(kSpinUs);
    }
    double wait_s = (now_ns() - t0) / 1e9;
    printf("[host 1] saw handshake after %.3fs; verifying payload\n", wait_s);

    int bad = verify_payload(r.base);
    if (bad == 0) {
      CACHELINE_STORE(ack, kMagicH1);
      printf("[host 1] payload verified; ACK sent\n");
      cxl_region_destroy(&r);
      printf("OK: cross-host CXL fabric verified on %s\n", argv[1]);
      return 0;
    } else {
      fprintf(stderr, "[host 1] FAIL: %d word mismatches\n", bad);
      cxl_region_destroy(&r);
      return 1;
    }
  }
}
