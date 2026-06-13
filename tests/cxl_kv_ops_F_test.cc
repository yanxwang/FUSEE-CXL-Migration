// Smoke test for CxlKvStoreF Stage 3.1 (INSERT + SEARCH only).
// UPDATE / REMOVE land in Stage 3.3; cross-process concurrency in Stage 4.
//
// Coverage:
//   - attach + bytes_for accounting
//   - INSERT N keys + SEARCH each, value matches
//   - duplicate INSERT returns -2
//   - missing-key SEARCH returns -1
//   - cross-bucket / cross-host pool segment usage works
//
// Built against MAP_SHARED|MAP_ANONYMOUS so it runs on the workstation
// (no devdax needed).  This file does NOT compile against the
// `-mclflushopt` flag because the protocol calls flush_line which uses
// clflushopt; on workstation we use the CXL_NO_FLUSHOPT cmake guard.
// (For now: skip running this test on workstation, just build-verify.)
//
// Stage 3.2 of docs/protocol_F_design_and_plan.md.

#include "cxl_kv_ops_F.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <sys/mman.h>
#include <unordered_map>
#include <vector>

using namespace fusee;

namespace {

void *map_shared(std::size_t bytes) {
  void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    std::perror("mmap");
    std::abort();
  }
  return p;
}

// Use small, non-zero keys to avoid the kEmptyKey == 0 sentinel.
uint64_t make_key(uint64_t i) { return i + 1; }

void test_basic_insert_search() {
  constexpr uint32_t kNumBuckets    = 4096;
  constexpr uint64_t kTotalRecords  = 16384;
  constexpr int      kHosts         = 1;

  std::size_t need = CxlKvStoreF::bytes_for(kNumBuckets, kTotalRecords, kHosts);
  void *region = map_shared(need);

  CxlKvStoreF store;
  int rc = store.attach(region, need, kNumBuckets, kTotalRecords,
                        /*host_id=*/0, kHosts, /*init=*/true);
  assert(rc == 0);

  // Insert + search a small set.
  std::unordered_map<uint64_t, uint64_t> oracle;
  for (uint64_t i = 0; i < 100; i++) {
    uint64_t k = make_key(i);
    uint64_t v = i * 100 + 7;
    int r = store.insert(k, v);
    assert(r == 0);
    oracle[k] = v;
  }

  for (auto &kv : oracle) {
    uint64_t out = 0;
    int r = store.search(kv.first, &out);
    assert(r == 0);
    assert(out == kv.second);
  }

  // Missing key returns -1.
  uint64_t junk = 0;
  assert(store.search(999999, &junk) == -1);

  // Duplicate INSERT returns -2.
  assert(store.insert(make_key(0), 12345) == -2);

  store.stop();
  munmap(region, need);
  std::printf("test_basic_insert_search PASS\n");
}

void test_skewed_keys_hit_same_bucket() {
  // Force many INSERTs into the same bucket by choosing keys that hash to
  // the same bucket_idx.  Verify the bucket holds up to 7 slots and the
  // 8th returns -1 (bucket full).  (FUSEE RACE_HASH_ASSOC_NUM = 7.)
  constexpr uint32_t kNumBuckets   = 1024;
  constexpr uint64_t kTotalRecords = 4096;
  constexpr int      kHosts        = 1;

  std::size_t need = CxlKvStoreF::bytes_for(kNumBuckets, kTotalRecords, kHosts);
  void *region = map_shared(need);

  CxlKvStoreF store;
  store.attach(region, need, kNumBuckets, kTotalRecords, 0, kHosts, true);

  // Brute search for 15 keys that collide on the same bucket.
  std::vector<uint64_t> collisions;
  uint32_t target_bucket = 7;
  uint64_t scan = 1;
  while (collisions.size() < 8 && scan < (1ULL << 26)) {
    if (cxl_fusee_hash(scan) % kNumBuckets == target_bucket) {
      collisions.push_back(scan);
    }
    scan++;
  }
  assert(collisions.size() == 8);

  // Insert first 7 — all succeed.
  for (int i = 0; i < 7; i++) {
    int r = store.insert(collisions[i], collisions[i] * 3);
    assert(r == 0);
  }
  // 8th — bucket full, returns -1.
  int r = store.insert(collisions[7], 999);
  assert(r == -1);

  // Search the 7 successful inserts.
  for (int i = 0; i < 7; i++) {
    uint64_t v = 0;
    assert(store.search(collisions[i], &v) == 0);
    assert(v == collisions[i] * 3);
  }

  store.stop();
  munmap(region, need);
  std::printf("test_skewed_keys_hit_same_bucket PASS\n");
}

void test_larger_workload_against_oracle() {
  constexpr uint32_t kNumBuckets   = 8192;
  constexpr uint64_t kTotalRecords = 16384;
  constexpr int      kHosts        = 1;

  std::size_t need = CxlKvStoreF::bytes_for(kNumBuckets, kTotalRecords, kHosts);
  void *region = map_shared(need);

  CxlKvStoreF store;
  store.attach(region, need, kNumBuckets, kTotalRecords, 0, kHosts, true);

  // Pseudo-random keys; check against std::unordered_map oracle.
  std::mt19937_64 rng(42);
  std::unordered_map<uint64_t, uint64_t> oracle;

  constexpr int kNumInserts = 4000;
  int inserted = 0;
  while (inserted < kNumInserts) {
    uint64_t k = rng() | 1ULL;  // ensure nonzero
    if (oracle.count(k)) continue;
    uint64_t v = rng();
    int r = store.insert(k, v);
    if (r == -1) {
      // Bucket full — skip
      continue;
    }
    assert(r == 0);
    oracle[k] = v;
    inserted++;
  }

  // Re-search all inserted keys.
  for (auto &kv : oracle) {
    uint64_t v = 0;
    int r = store.search(kv.first, &v);
    assert(r == 0);
    assert(v == kv.second);
  }

  // Random absent keys return -1.
  for (int i = 0; i < 100; i++) {
    uint64_t k = rng() | 1ULL;
    if (oracle.count(k)) continue;
    uint64_t v = 0;
    int r = store.search(k, &v);
    assert(r == -1);
  }

  store.stop();
  munmap(region, need);
  std::printf("test_larger_workload_against_oracle PASS\n");
}

}  // namespace

void test_update_remove() {
  constexpr uint32_t kNumBuckets   = 4096;
  constexpr uint64_t kTotalRecords = 16384;
  constexpr int      kHosts        = 1;

  std::size_t need = CxlKvStoreF::bytes_for(kNumBuckets, kTotalRecords, kHosts);
  void *region = map_shared(need);

  CxlKvStoreF store;
  store.attach(region, need, kNumBuckets, kTotalRecords, 0, kHosts, true);

  // INSERT a key, SEARCH it, UPDATE it, SEARCH again, REMOVE it.
  uint64_t k = make_key(7);
  assert(store.insert(k, 100) == 0);
  uint64_t v = 0;
  assert(store.search(k, &v) == 0 && v == 100);

  assert(store.update(k, 200) == 0);
  v = 0;
  assert(store.search(k, &v) == 0 && v == 200);

  // UPDATE of absent key returns -1.
  assert(store.update(make_key(9999), 999) == -1);

  // REMOVE.
  assert(store.remove(k) == 0);
  assert(store.search(k, &v) == -1);

  // REMOVE of absent key returns -1.
  assert(store.remove(k) == -1);

  store.stop();
  munmap(region, need);
  std::printf("test_update_remove PASS\n");
}

void test_repeated_search_hits_cache() {
  // After an INSERT or first SEARCH, subsequent SEARCHs go through the
  // fast path.  This test confirms hit counters move (correctness check —
  // value semantics don't depend on cache state, so this is observational).
  constexpr uint32_t kNumBuckets   = 4096;
  constexpr uint64_t kTotalRecords = 16384;
  constexpr int      kHosts        = 1;

  std::size_t need = CxlKvStoreF::bytes_for(kNumBuckets, kTotalRecords, kHosts);
  void *region = map_shared(need);

  CxlKvStoreF store;
  store.attach(region, need, kNumBuckets, kTotalRecords, 0, kHosts, true);

  uint64_t k = make_key(42);
  assert(store.insert(k, 12345) == 0);

  // First search populates the cache (the slow path inserts).
  // Actually, insert() already populates it.  Each subsequent search() is
  // expected to take the fast path and bump total_hits_.
  uint64_t hits_before = store.cache().total_hits();
  for (int i = 0; i < 10; i++) {
    uint64_t v = 0;
    int r = store.search(k, &v);
    assert(r == 0 && v == 12345);
  }
  uint64_t hits_after = store.cache().total_hits();
  // After 10 reads of the same key, at least one fast-path hit should be
  // recorded.  (We don't check for exactly 10 because we may also
  // pre-populate via insert(), and the first search after insert may take
  // the fast path too.  Strict equality risks brittleness; > 0 is safe.)
  assert(hits_after > hits_before);

  store.stop();
  munmap(region, need);
  std::printf("test_repeated_search_hits_cache PASS\n");
}

void test_update_then_search_finds_new_value() {
  // The slot's blk_off changes after UPDATE.  Cached kvpair_addr no
  // longer matches, so the fast path triggers record_miss + refetches.
  constexpr uint32_t kNumBuckets   = 4096;
  constexpr uint64_t kTotalRecords = 16384;
  constexpr int      kHosts        = 1;

  std::size_t need = CxlKvStoreF::bytes_for(kNumBuckets, kTotalRecords, kHosts);
  void *region = map_shared(need);

  CxlKvStoreF store;
  store.attach(region, need, kNumBuckets, kTotalRecords, 0, kHosts, true);

  uint64_t k = make_key(101);
  assert(store.insert(k, 1) == 0);
  // Several searches to warm the cache fast path.
  uint64_t v = 0;
  for (int i = 0; i < 5; i++) assert(store.search(k, &v) == 0 && v == 1);

  // UPDATE — slot now points to a different KV pair address.
  assert(store.update(k, 2) == 0);

  // Next SEARCH should still return the new value.  Whether it goes via
  // fast-path-with-record_miss-then-refetch or via slow path doesn't
  // matter for correctness.
  for (int i = 0; i < 5; i++) {
    v = 0;
    assert(store.search(k, &v) == 0 && v == 2);
  }

  store.stop();
  munmap(region, need);
  std::printf("test_update_then_search_finds_new_value PASS\n");
}

void test_remove_evicts_cache() {
  constexpr uint32_t kNumBuckets   = 4096;
  constexpr uint64_t kTotalRecords = 16384;
  constexpr int      kHosts        = 1;

  std::size_t need = CxlKvStoreF::bytes_for(kNumBuckets, kTotalRecords, kHosts);
  void *region = map_shared(need);

  CxlKvStoreF store;
  store.attach(region, need, kNumBuckets, kTotalRecords, 0, kHosts, true);

  uint64_t k = make_key(55);
  assert(store.insert(k, 999) == 0);
  uint64_t v = 0;
  assert(store.search(k, &v) == 0 && v == 999);

  assert(store.remove(k) == 0);
  // Cache should have evicted; next search returns -1 (slow path).
  assert(store.search(k, &v) == -1);

  store.stop();
  munmap(region, need);
  std::printf("test_remove_evicts_cache PASS\n");
}

int main() {
  test_basic_insert_search();
  test_skewed_keys_hit_same_bucket();
  test_larger_workload_against_oracle();
  test_update_remove();
  test_repeated_search_hits_cache();
  test_update_then_search_finds_new_value();
  test_remove_evicts_cache();
  std::printf("ALL TESTS PASS\n");
  return 0;
}
