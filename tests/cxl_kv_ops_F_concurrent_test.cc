// 2-process concurrency test for Protocol F.  Verifies that the per-slot
// LFM lock actually serializes cross-process writers and that
// Linearizability holds against the slot atomicity + KV pair immutability
// design.
//
// Test scenarios:
//   1. Disjoint INSERTs: parent inserts even keys, child inserts odd keys,
//      both see all entries after sync.
//   2. Concurrent UPDATEs on the same key: N hammers, final value must be
//      one of the writers' inputs (no torn / corrupted slot).
//   3. Mixed read+write: writer churns one key, reader sees a value that
//      is always one of the writers' inputs and never garbage.
//   4. Cross-host hash-diff: parent + child each insert disjoint slabs;
//      both processes SEARCH every key at the end; both must agree.
//
// Stage 4b of docs/protocol_F_design_and_plan.md.

#include "cxl_kv_ops_F.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

using namespace fusee;

namespace {

void *map_shared(std::size_t bytes) {
  void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) { std::perror("mmap"); std::abort(); }
  return p;
}

uint64_t mk(uint64_t i) { return i + 1; }  // avoid sentinel 0

// ---------- Test 1: disjoint INSERT from two processes ----------
void test_disjoint_inserts() {
  constexpr uint32_t kNumBuckets   = 256;
  constexpr uint64_t kTotalRecords = 4096;
  constexpr int      kHosts        = 2;
  constexpr int      kPerProc      = 200;

  std::size_t need = CxlKvStoreF::bytes_for(kNumBuckets, kTotalRecords, kHosts);
  void *region = map_shared(need);

  CxlKvStoreF store;
  { int _rc = store.attach(region, need, kNumBuckets, kTotalRecords, 0, kHosts,
                      true); assert(_rc == 0); }

  // Cookie for the child to gate on (init_done_bit handles this in
  // attach()).
  pid_t pid = fork();
  if (pid == 0) {
    CxlKvStoreF child;
    int rc = child.attach(region, need, kNumBuckets, kTotalRecords, 1,
                          kHosts, false);
    assert(rc == 0);
    // Child inserts odd-indexed keys.
    for (int i = 0; i < kPerProc; i++) {
      uint64_t k = mk(2 * i + 1);
      uint64_t v = k * 1000;
      int r = child.insert(k, v);
      assert(r == 0);
    }
    _exit(0);
  }
  assert(pid > 0);

  // Parent inserts even-indexed keys concurrently.
  for (int i = 0; i < kPerProc; i++) {
    uint64_t k = mk(2 * i);
    uint64_t v = k * 1000;
    int r = store.insert(k, v);
    assert(r == 0);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  // Both processes' inserts must be visible from the parent.
  for (int i = 0; i < kPerProc * 2; i++) {
    uint64_t k = mk(i);
    uint64_t v = 0;
    int r = store.search(k, &v);
    assert(r == 0);
    assert(v == k * 1000);
  }

  store.stop();
  munmap(region, need);
  std::printf("test_disjoint_inserts PASS\n");
}

// ---------- Test 2: concurrent UPDATE on same key (Linearizable) ----------
void test_concurrent_updates_same_key() {
  constexpr uint32_t kNumBuckets   = 256;
  constexpr uint64_t kTotalRecords = 4096;
  constexpr int      kHosts        = 2;
  constexpr int      kUpdates      = 500;

  std::size_t need = CxlKvStoreF::bytes_for(kNumBuckets, kTotalRecords, kHosts);
  void *region = map_shared(need);

  CxlKvStoreF store;
  { int _rc = store.attach(region, need, kNumBuckets, kTotalRecords, 0, kHosts,
                      true); assert(_rc == 0); }

  uint64_t k = mk(42);
  { int _r2 = store.insert(k, 0); assert(_r2 == 0); }

  pid_t pid = fork();
  if (pid == 0) {
    CxlKvStoreF child;
    { int _rc = child.attach(region, need, kNumBuckets, kTotalRecords, 1, kHosts,
                        false); assert(_rc == 0); }
    // Child writes 2 * i + 1  (odd numbers)
    for (int i = 0; i < kUpdates; i++) {
      int r = child.update(k, static_cast<uint64_t>(2 * i + 1));
      assert(r == 0);
    }
    _exit(0);
  }
  // Parent writes 2 * i  (even numbers, starting at 2)
  for (int i = 0; i < kUpdates; i++) {
    int r = store.update(k, static_cast<uint64_t>(2 * (i + 1)));
    assert(r == 0);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  // Final value must be either the last parent write (2*kUpdates) or some
  // child odd value or one of the parent intermediates.  We only check
  // that it's NOT garbage: it must be a value either party wrote.
  uint64_t out = 0;
  { int _r2 = store.search(k, &out); assert(_r2 == 0); }
  bool is_even = (out >= 2 && out <= 2 * kUpdates && (out % 2) == 0);
  bool is_odd  = (out <= static_cast<uint64_t>(2 * kUpdates - 1) && (out % 2) == 1);
  assert((is_even || is_odd) && "final value must be one of the writers' inputs");

  store.stop();
  munmap(region, need);
  std::printf("test_concurrent_updates_same_key (final=%lu) PASS\n", out);
}

// ---------- Test 3: mixed reader+writer torn-read guard ----------
void test_reader_never_sees_torn_state() {
  // Reader continuously searches one key while a writer continuously
  // UPDATEs it.  Reader must never see a value that the writer didn't
  // explicitly publish.
  constexpr uint32_t kNumBuckets   = 256;
  constexpr uint64_t kTotalRecords = 4096;
  constexpr int      kHosts        = 2;
  constexpr int      kIters        = 5000;

  std::size_t need = CxlKvStoreF::bytes_for(kNumBuckets, kTotalRecords, kHosts);
  void *region = map_shared(need);

  CxlKvStoreF store;
  { int _rc = store.attach(region, need, kNumBuckets, kTotalRecords, 0, kHosts,
                      true); assert(_rc == 0); }

  uint64_t k = mk(7);
  { int _r2 = store.insert(k, 0xA000); assert(_r2 == 0); }

  pid_t pid = fork();
  if (pid == 0) {
    CxlKvStoreF child;
    { int _rc = child.attach(region, need, kNumBuckets, kTotalRecords, 1, kHosts,
                        false); assert(_rc == 0); }
    // Reader: only ever expects values 0xA000, 0xA001, ..., 0xA000+kIters
    // — anything else means a torn slot or memory corruption.
    int torn = 0;
    for (int i = 0; i < kIters; i++) {
      uint64_t v = 0;
      int r = child.search(k, &v);
      if (r != 0) { torn++; continue; }
      if (v < 0xA000 || v > 0xA000 + static_cast<uint64_t>(kIters)) {
        std::fprintf(stderr, "TORN VALUE 0x%lx\n", v);
        _exit(2);
      }
    }
    if (torn > 0) {
      std::fprintf(stderr, "got %d 'not found' reads (unexpected)\n", torn);
      _exit(3);
    }
    _exit(0);
  }

  // Writer: bumps value 0xA000, 0xA001, 0xA002, ...
  for (int i = 1; i <= kIters; i++) {
    int r = store.update(k, 0xA000 + static_cast<uint64_t>(i));
    assert(r == 0);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  assert(WIFEXITED(status));
  assert(WEXITSTATUS(status) == 0 && "reader observed a torn / out-of-range value");

  store.stop();
  munmap(region, need);
  std::printf("test_reader_never_sees_torn_state PASS\n");
}

// ---------- Test 4: cross-process hash-diff ----------
void test_hash_diff_cross_process() {
  constexpr uint32_t kNumBuckets   = 256;
  constexpr uint64_t kTotalRecords = 4096;
  constexpr int      kHosts        = 2;
  constexpr int      kPerProc      = 200;

  std::size_t need = CxlKvStoreF::bytes_for(kNumBuckets, kTotalRecords, kHosts);
  void *region = map_shared(need);

  // Tiny shared region for the child to write its checksum into.
  uint64_t *xfer = static_cast<uint64_t *>(map_shared(64));
  xfer[0] = 0;

  CxlKvStoreF store;
  { int _rc = store.attach(region, need, kNumBuckets, kTotalRecords, 0, kHosts,
                      true); assert(_rc == 0); }

  // Phase A: parent + child each insert their disjoint slabs concurrently.
  pid_t pid = fork();
  if (pid == 0) {
    CxlKvStoreF child;
    { int _rc = child.attach(region, need, kNumBuckets, kTotalRecords, 1, kHosts,
                        false); assert(_rc == 0); }
    for (int i = 0; i < kPerProc; i++) {
      uint64_t k = mk(kPerProc + i);
      uint64_t v = k * 17;
      { int _r2 = child.insert(k, v); assert(_r2 == 0); }
    }
    // Wait — but in this minimal version we can't pthread_barrier across
    // fork.  Instead: child waits a moment (sleep) so parent's inserts
    // have time to complete, then computes hash and writes to xfer.
    // For robustness, use a bigger sleep than the parent's expected work.
    usleep(50000);  // 50 ms
    uint64_t h = 0;
    int missing = 0, wrong = 0;
    for (int i = 0; i < 2 * kPerProc; i++) {
      uint64_t k = mk(i);
      uint64_t v = 0;
      int r = child.search(k, &v);
      if (r == 0) {
        h ^= (k * 0x9e3779b97f4a7c15ULL) ^ v;
        uint64_t expected_v = k * 17;
        if (v != expected_v) {
          std::fprintf(stderr,
              "  WRONG: i=%d k=%lu v_got=%lu v_expected=%lu\n",
              i, k, v, expected_v);
          wrong++;
        }
      } else {
        missing++;
      }
    }
    xfer[0] = h;
    std::fprintf(stderr, "child: missing=%d wrong=%d\n", missing, wrong);
    _exit(0);
  }
  for (int i = 0; i < kPerProc; i++) {
    uint64_t k = mk(i);
    uint64_t v = k * 17;
    { int _r2 = store.insert(k, v); assert(_r2 == 0); }
  }
  int status = 0;
  waitpid(pid, &status, 0);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  // Parent hash.
  uint64_t hp = 0;
  for (int i = 0; i < 2 * kPerProc; i++) {
    uint64_t k = mk(i);
    uint64_t v = 0;
    int r = store.search(k, &v);
    assert(r == 0 && "post-sync search must succeed for every key");
    hp ^= (k * 0x9e3779b97f4a7c15ULL) ^ v;
  }
  uint64_t hc = xfer[0];
  std::printf("  parent_hash=%016lx child_hash=%016lx\n", hp, hc);
  assert(hp == hc && "hash-diff: parent and child must see identical state");

  munmap(xfer, 64);
  store.stop();
  munmap(region, need);
  std::printf("test_hash_diff_cross_process PASS\n");
}

}  // namespace

int main() {
  setvbuf(stderr, nullptr, _IONBF, 0);
  setvbuf(stdout, nullptr, _IOLBF, 0);
  test_disjoint_inserts();
  test_concurrent_updates_same_key();
  test_reader_never_sees_torn_state();
  test_hash_diff_cross_process();
  std::printf("ALL TESTS PASS\n");
  return 0;
}
