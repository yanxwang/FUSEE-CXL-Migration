// Phase 2 verification: multi-process mutual exclusion via BucketLockTable.
//
// Two processes contend for the same bucket lock and each performs N
// increments on a shared counter. If locking is correct, the final counter
// value equals 2*N. Running with a naive "no lock" implementation would race
// and under-count.
//
// Usage:
//   ./cxl_bucket_lock_test <dev_path> [iters_per_proc]
//
// Region layout (statically carved):
//   [0        .. HDR_SZ)       : header (2 cacheline_u64: counter, ready)
//   [HDR_SZ   .. HDR_SZ+TBL)   : BucketLockTable for 1 bucket
// Both processes mmap the same region and attach a BucketLockTable view.
// Parent runs shm_mutex_init (init_mutexes=true); child attaches read-only.

#include "cxl_bucket_lock.h"
#include "cxl_mm.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" {
#include "common.h"  // cacheline_u64, CACHELINE_STORE/LOAD, flush_line, store_fence
}

using fusee::BucketLockTable;
using fusee::CXLRegion;
using fusee::cxl_region_destroy;
using fusee::cxl_region_init;

namespace {

// Region header -- two cacheline_u64 slots we read/write through CXL-safe
// primitives so writes from the other process become visible.
struct RegionHeader {
  cacheline_u64 counter;
  cacheline_u64 ready; // parent sets = 1 after shm_mutex_init done.
};

constexpr size_t kHeaderBytes = sizeof(RegionHeader);

// Table base is placed right after the header, cacheline-aligned.
static inline void *table_base(void *region) {
  auto p = reinterpret_cast<char *>(region) + kHeaderBytes;
  return reinterpret_cast<void *>((reinterpret_cast<uintptr_t>(p) + 63) & ~uintptr_t(63));
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <dev_path> [iters_per_proc]\n", argv[0]);
    return 2;
  }
  const char *dev = argv[1];
  uint64_t iters = (argc >= 3) ? strtoull(argv[2], nullptr, 0) : 50000ULL;

  constexpr uint32_t kNumBuckets = 1;
  constexpr int kNumHosts = 2;

  // Large enough to hold header + bucket table (shm_mutex_t is multi-cacheline).
  size_t region_size = 4UL * 1024 * 1024;

  pid_t pid = fork();
  if (pid < 0) { perror("fork"); return 1; }

  CXLRegion r{};
  if (cxl_region_init(&r, dev, region_size) < 0) {
    fprintf(stderr, "[pid=%d] cxl_region_init failed\n", getpid());
    return 1;
  }

  auto *hdr = reinterpret_cast<RegionHeader *>(r.base);
  void *tbase = table_base(r.base);
  BucketLockTable tbl;

  int host_id = (pid == 0) ? 1 : 0;

  if (host_id == 0) {
    // Parent: reset counter, init mutexes, publish ready.
    CACHELINE_STORE(&hdr->counter, 0ULL);
    CACHELINE_STORE(&hdr->ready, 0ULL);
    tbl.attach(tbase, kNumBuckets, /*init_mutexes=*/true);
    CACHELINE_STORE(&hdr->ready, 1ULL);
  } else {
    // Child: wait for parent to finish init.
    for (int spin = 0; spin < 500; spin++) {
      if (CACHELINE_LOAD(&hdr->ready) == 1ULL) break;
      usleep(10 * 1000);
      if (spin == 499) {
        fprintf(stderr, "[child] timeout waiting for init\n");
        cxl_region_destroy(&r);
        _exit(1);
      }
    }
    tbl.attach(tbase, kNumBuckets, /*init_mutexes=*/false);
  }

  // Contention loop: increment the shared counter under the bucket lock.
  for (uint64_t i = 0; i < iters; i++) {
    tbl.lock(0, host_id, kNumHosts);
    uint64_t cur = CACHELINE_LOAD(&hdr->counter);
    CACHELINE_STORE(&hdr->counter, cur + 1);
    tbl.unlock(0, host_id);
  }

  if (host_id == 1) {
    cxl_region_destroy(&r);
    _exit(0);
  }

  // Parent: wait for child, check final counter.
  int status = 0;
  waitpid(pid, &status, 0);
  int child_rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128;

  uint64_t final_counter = CACHELINE_LOAD(&hdr->counter);
  uint64_t expected = iters * kNumHosts;

  printf("final counter = %lu, expected = %lu (iters=%lu, hosts=%d)\n",
         final_counter, expected, iters, kNumHosts);

  cxl_region_destroy(&r);

  if (child_rc != 0) {
    fprintf(stderr, "child exited rc=%d\n", child_rc);
    return 1;
  }
  if (final_counter != expected) {
    fprintf(stderr, "FAIL: lost %lu increments -- mutex did not hold\n",
            expected - final_counter);
    return 1;
  }
  printf("OK: mutex held under contention on %s\n", dev);
  return 0;
}
