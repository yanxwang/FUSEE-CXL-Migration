// iter-4A Phase 2 unit + cross-process test for SlotDirectory.
//
// Validates spec §I5/I7/I8/AP6/AP7. Uses MAP_SHARED|MAP_ANONYMOUS so
// the test mirrors how the runner allocates the directory pre-fork.

#include "cxl_directory.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using fusee::SlotDirectory;
using fusee::SlotDirectoryEntry;
using fusee::slot_directory_init;
using fusee::slot_directory_destroy;
using fusee::slot_directory_entry;
using fusee::slot_directory_lock;
using fusee::slot_directory_unlock;
using fusee::slot_directory_set_sharer;
using fusee::slot_directory_clear_sharer;
using fusee::slot_directory_bytes;

static uint64_t now_ns() {
  timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Test 1: basic init/lock/unlock/CRUD.
static int test_basic() {
  const uint32_t B = 64, S = 7;
  void *mem = mmap(nullptr, slot_directory_bytes(B, S),
                   PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) { perror("mmap"); return 1; }

  SlotDirectory dir;
  if (slot_directory_init(&dir, mem, B, S) != 0) {
    fprintf(stderr, "FAIL: init\n"); return 1;
  }

  // All entries should start INVALID with empty sharers.
  for (uint32_t b = 0; b < B; b++) {
    for (uint32_t s = 0; s < S; s++) {
      auto *e = slot_directory_entry(&dir, b, s);
      if (e->state != fusee::kDirStateInvalid || e->sharer_bitmap != 0 ||
          e->version != 0) {
        fprintf(stderr, "FAIL: entry (%u,%u) not zero-init\n", b, s);
        return 1;
      }
    }
  }

  // CRUD: set 3 sharers on one entry, verify, clear, verify.
  auto *e = slot_directory_entry(&dir, 5, 3);
  slot_directory_lock(e);
  slot_directory_set_sharer(e, 0);
  slot_directory_set_sharer(e, 1);
  slot_directory_set_sharer(e, 3);
  if (e->sharer_bitmap != 0b1011 || e->state != fusee::kDirStateShared) {
    fprintf(stderr, "FAIL: set_sharer\n"); slot_directory_unlock(e); return 1;
  }
  slot_directory_clear_sharer(e, 1);
  if (e->sharer_bitmap != 0b1001) {
    fprintf(stderr, "FAIL: clear_sharer\n"); slot_directory_unlock(e); return 1;
  }
  slot_directory_clear_sharer(e, 0);
  slot_directory_clear_sharer(e, 3);
  if (e->sharer_bitmap != 0 || e->state != fusee::kDirStateInvalid) {
    fprintf(stderr, "FAIL: clear-all -> invalid\n"); slot_directory_unlock(e); return 1;
  }
  slot_directory_unlock(e);

  slot_directory_destroy(&dir);
  munmap(mem, slot_directory_bytes(B, S));
  printf("test_basic: PASS\n");
  return 0;
}

// Test 2: multi-thread stress, single-process.
static int test_multithread_stress() {
  const uint32_t B = 64, S = 7;
  const uint32_t N = 8;
  const uint32_t OPS = 10000;
  void *mem = mmap(nullptr, slot_directory_bytes(B, S),
                   PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) return 1;

  SlotDirectory dir;
  slot_directory_init(&dir, mem, B, S);

  // Each thread does OPS rounds: lock entry t, increment its version,
  // toggle bit (t % 8) on sharer_bitmap, unlock. Final version of entry
  // (t,0) should be exactly OPS, and bit (t%8) should be back at 0
  // (toggled even times).
  std::vector<std::thread> threads;
  for (uint32_t t = 0; t < N; t++) {
    threads.emplace_back([t, &dir, OPS]() {
      for (uint32_t i = 0; i < OPS; i++) {
        auto *e = slot_directory_entry(&dir, t, 0);
        slot_directory_lock(e);
        e->version++;
        // Toggle even number of times -> back to 0.
        if (e->sharer_bitmap & (1u << (t % 8))) {
          slot_directory_clear_sharer(e, t % 8);
        } else {
          slot_directory_set_sharer(e, t % 8);
        }
        slot_directory_unlock(e);
      }
    });
  }
  for (auto &th : threads) th.join();

  for (uint32_t t = 0; t < N; t++) {
    auto *e = slot_directory_entry(&dir, t, 0);
    if (e->version != OPS) {
      fprintf(stderr, "FAIL: entry (%u,0) version=%u (expected %u)\n",
              t, e->version, OPS);
      return 1;
    }
    if (OPS % 2 == 0 && e->sharer_bitmap != 0) {
      fprintf(stderr, "FAIL: entry (%u,0) bitmap=%u (expected 0)\n",
              t, e->sharer_bitmap);
      return 1;
    }
  }

  slot_directory_destroy(&dir);
  munmap(mem, slot_directory_bytes(B, S));
  printf("test_multithread_stress: PASS\n");
  return 0;
}

// Test 3: cross-process via fork + MAP_SHARED.
static int test_fork_share() {
  const uint32_t B = 64, S = 7;
  const int CHILDREN = 4;
  const int OPS = 1000;
  void *mem = mmap(nullptr, slot_directory_bytes(B, S),
                   PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) return 1;

  SlotDirectory dir;
  slot_directory_init(&dir, mem, B, S);

  std::vector<pid_t> kids;
  for (int c = 0; c < CHILDREN; c++) {
    pid_t p = fork();
    if (p == 0) {
      for (int i = 0; i < OPS; i++) {
        auto *e = slot_directory_entry(&dir, 0, 0);
        slot_directory_lock(e);
        e->version++;
        slot_directory_set_sharer(e, c);  // each child sets its bit
        slot_directory_unlock(e);
      }
      _exit(0);
    }
    kids.push_back(p);
  }
  for (auto p : kids) waitpid(p, nullptr, 0);

  auto *e = slot_directory_entry(&dir, 0, 0);
  uint32_t expected_v = (uint32_t)(CHILDREN * OPS);
  if (e->version != expected_v) {
    fprintf(stderr, "FAIL: fork share version=%u (expected %u)\n",
            e->version, expected_v);
    return 1;
  }
  // Each of the 4 children's bits should be set.
  if (e->sharer_bitmap != 0b1111) {
    fprintf(stderr, "FAIL: fork share bitmap=%u (expected 15)\n",
            e->sharer_bitmap);
    return 1;
  }

  slot_directory_destroy(&dir);
  munmap(mem, slot_directory_bytes(B, S));
  printf("test_fork_share: PASS\n");
  return 0;
}

// Test 4: spinlock latency.
static int test_lock_latency() {
  const uint32_t B = 64, S = 7;
  void *mem = mmap(nullptr, slot_directory_bytes(B, S),
                   PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) return 1;

  SlotDirectory dir;
  slot_directory_init(&dir, mem, B, S);
  auto *e = slot_directory_entry(&dir, 0, 0);

  const int M = 1000000;
  uint64_t t0 = now_ns();
  for (int i = 0; i < M; i++) {
    slot_directory_lock(e);
    e->version++;
    slot_directory_unlock(e);
  }
  uint64_t t1 = now_ns();
  double ns = (double)(t1 - t0) / M;
  printf("uncontended lock+unlock: %.1f ns/op\n", ns);
  if (ns > 100.0) {
    fprintf(stderr, "WARN: spinlock slower than 100 ns budget (plan target 30 ns)\n");
  }

  slot_directory_destroy(&dir);
  munmap(mem, slot_directory_bytes(B, S));
  return 0;
}

int main() {
  if (test_basic() != 0) return 1;
  if (test_multithread_stress() != 0) return 1;
  if (test_fork_share() != 0) return 1;
  if (test_lock_latency() != 0) return 1;
  printf("ALL PASS\n");
  return 0;
}
