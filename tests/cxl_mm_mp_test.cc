// Phase 1 verification: two independent processes mmap the same CXL region
// and observe each other's writes. Each process calls cxl_region_init on its
// own fd (no mmap inheritance) to exercise the devdax cross-process path.
//
// Usage:
//   ./cxl_mm_mp_test <dev_path> [size_bytes]
//
// Protocol on the shared region (laid out at the mapped base):
//   [0..7]      : handshake word. 0 = none, 1 = writer done, 2 = reader done.
//   [8..71]     : 8-word payload the writer fills with a pattern.
//
// Writer process writes payload, then publishes handshake=1 with an sfence.
// Reader process spins on handshake==1, reads payload, verifies, then sets
// handshake=2. Writer waits for handshake==2 before exiting.

#include "cxl_mm.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using fusee::CXLRegion;
using fusee::cxl_region_destroy;
using fusee::cxl_region_init;

namespace {

inline void store_fence() { __asm__ __volatile__("sfence" ::: "memory"); }
inline void load_fence() { __asm__ __volatile__("lfence" ::: "memory"); }
inline void clflushopt(const volatile void *p) {
  __asm__ __volatile__("clflushopt (%0)" ::"r"(p) : "memory");
}

constexpr size_t kHandshakeOff = 0;
constexpr size_t kPayloadOff = 64;
constexpr int kPayloadWords = 8;
constexpr uint64_t kPatternSeed = 0xA11CE0DEDEADBEEFULL;

uint64_t pattern_word(int i) { return kPatternSeed ^ (uint64_t(i) * 0x9E3779B97F4A7C15ULL); }

void flush_and_fence(void *addr) {
  clflushopt(addr);
  store_fence();
}

int run_writer(void *base) {
  auto *handshake = reinterpret_cast<volatile uint64_t *>(
      reinterpret_cast<char *>(base) + kHandshakeOff);
  auto *payload = reinterpret_cast<volatile uint64_t *>(
      reinterpret_cast<char *>(base) + kPayloadOff);

  *handshake = 0;
  flush_and_fence((void *)handshake);

  for (int i = 0; i < kPayloadWords; i++) {
    payload[i] = pattern_word(i);
  }
  for (int i = 0; i < kPayloadWords; i++) {
    clflushopt((const void *)&payload[i]);
  }
  store_fence();

  *handshake = 1;
  flush_and_fence((void *)handshake);
  printf("[writer pid=%d] published payload\n", getpid());

  // Wait for reader to set handshake=2.
  for (int spin = 0; spin < 200; spin++) {
    clflushopt((const void *)handshake);
    load_fence();
    if (*handshake == 2) {
      printf("[writer pid=%d] reader ack'd\n", getpid());
      return 0;
    }
    usleep(10 * 1000);
  }
  fprintf(stderr, "[writer pid=%d] timeout waiting for reader ack\n", getpid());
  return 1;
}

int run_reader(void *base) {
  auto *handshake = reinterpret_cast<volatile uint64_t *>(
      reinterpret_cast<char *>(base) + kHandshakeOff);
  auto *payload = reinterpret_cast<volatile uint64_t *>(
      reinterpret_cast<char *>(base) + kPayloadOff);

  for (int spin = 0; spin < 500; spin++) {
    clflushopt((const void *)handshake);
    load_fence();
    if (*handshake == 1) break;
    usleep(10 * 1000);
    if (spin == 499) {
      fprintf(stderr, "[reader pid=%d] timeout waiting for writer\n", getpid());
      return 1;
    }
  }

  for (int i = 0; i < kPayloadWords; i++) {
    clflushopt((const void *)&payload[i]);
  }
  load_fence();

  int rc = 0;
  for (int i = 0; i < kPayloadWords; i++) {
    uint64_t got = payload[i];
    uint64_t want = pattern_word(i);
    if (got != want) {
      fprintf(stderr, "[reader pid=%d] word %d mismatch: got %016lx want %016lx\n",
              getpid(), i, got, want);
      rc = 1;
    }
  }
  if (rc == 0) printf("[reader pid=%d] payload verified\n", getpid());

  *handshake = 2;
  flush_and_fence((void *)handshake);
  return rc;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <dev_path> [size_bytes]\n", argv[0]);
    return 2;
  }
  const char *dev = argv[1];
  size_t size = (argc >= 3) ? (size_t)strtoull(argv[2], nullptr, 0)
                            : (size_t)(8UL * 1024 * 1024);

  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return 1;
  }

  CXLRegion r{};
  if (cxl_region_init(&r, dev, size) < 0) {
    fprintf(stderr, "[pid=%d] cxl_region_init failed\n", getpid());
    return 1;
  }

  int rc;
  if (pid == 0) {
    // Child = reader.
    rc = run_reader(r.base);
    cxl_region_destroy(&r);
    _exit(rc);
  }

  // Parent = writer.
  rc = run_writer(r.base);

  int status = 0;
  waitpid(pid, &status, 0);
  int child_rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
  cxl_region_destroy(&r);

  if (rc == 0 && child_rc == 0) {
    printf("OK: multi-process shared region works on %s\n", dev);
    return 0;
  }
  fprintf(stderr, "FAIL: writer=%d reader=%d\n", rc, child_rc);
  return 1;
}
