// Unit test for CxlFuseeIndexCache.

#include "cxl_fusee_index_cache.h"

#include <cassert>
#include <cstdio>

using namespace fusee;

namespace {

void test_lookup_absent() {
  CxlFuseeIndexCache cache(/*capacity=*/8);
  CxlFuseeIndexCacheEntry e;
  assert(!cache.lookup(42, &e));
  assert(cache.size() == 0);
  std::printf("test_lookup_absent PASS\n");
}

void test_insert_lookup_hit() {
  CxlFuseeIndexCache cache(8);
  cache.insert(42, 0x1000, 0xa000);
  CxlFuseeIndexCacheEntry e;
  assert(cache.lookup(42, &e));
  assert(e.slot_addr   == 0x1000);
  assert(e.kvpair_addr == 0xa000);
  assert(e.access_cnt  == 0);
  assert(e.invalid_cnt == 0);
  assert(!e.bypass);
  std::printf("test_insert_lookup_hit PASS\n");
}

void test_lru_eviction() {
  CxlFuseeIndexCache cache(3);
  cache.insert(1, 0x1000, 0xa000);
  cache.insert(2, 0x2000, 0xb000);
  cache.insert(3, 0x3000, 0xc000);
  assert(cache.size() == 3);
  CxlFuseeIndexCacheEntry e;
  assert(cache.lookup(1, &e));
  cache.insert(4, 0x4000, 0xd000);
  assert(cache.size() == 3);
  assert(!cache.lookup(2, &e) && "key 2 should have been evicted");
  assert(cache.lookup(1, &e));
  assert(cache.lookup(3, &e));
  assert(cache.lookup(4, &e));
  std::printf("test_lru_eviction PASS\n");
}

void test_record_hit_increments() {
  CxlFuseeIndexCache cache(8);
  cache.insert(42, 0x1000, 0xa000);
  for (int i = 0; i < 5; i++) cache.record_hit(42);
  CxlFuseeIndexCacheEntry e;
  assert(cache.lookup(42, &e));
  assert(e.access_cnt  == 5);
  assert(e.invalid_cnt == 0);
  assert(!e.bypass);
  std::printf("test_record_hit_increments PASS\n");
}

void test_bypass_triggers_above_threshold() {
  CxlFuseeIndexCache cache(8, 0.5, 8);
  cache.insert(42, 0x1000, 0xa000);
  for (int i = 0; i < 6; i++) cache.record_miss(42);
  for (int i = 0; i < 2; i++) cache.record_hit(42);
  CxlFuseeIndexCacheEntry e;
  assert(cache.lookup(42, &e));
  assert(e.access_cnt  == 8);
  assert(e.invalid_cnt == 6);
  assert(e.bypass && "key should be bypassed at ratio 0.75");
  std::printf("test_bypass_triggers_above_threshold PASS\n");
}

void test_bypass_below_min_samples_holds_off() {
  CxlFuseeIndexCache cache(8, 0.5, 16);
  cache.insert(42, 0x1000, 0xa000);
  for (int i = 0; i < 5; i++) cache.record_miss(42);
  cache.record_hit(42);
  CxlFuseeIndexCacheEntry e;
  assert(cache.lookup(42, &e));
  assert(!e.bypass);
  std::printf("test_bypass_below_min_samples_holds_off PASS\n");
}

void test_bypass_decays_on_read_pressure() {
  CxlFuseeIndexCache cache(8, 0.5, 8);
  cache.insert(42, 0x1000, 0xa000);
  for (int i = 0; i < 8; i++) cache.record_miss(42);
  CxlFuseeIndexCacheEntry e1;
  assert(cache.lookup(42, &e1));
  assert(e1.bypass);
  for (int i = 0; i < 10; i++) cache.record_hit(42);
  CxlFuseeIndexCacheEntry e2;
  assert(cache.lookup(42, &e2));
  assert(!e2.bypass);
  std::printf("test_bypass_decays_on_read_pressure PASS\n");
}

void test_insert_existing_preserves_counters() {
  CxlFuseeIndexCache cache(8, 0.5, 8);
  cache.insert(42, 0x1000, 0xa000);
  for (int i = 0; i < 5; i++) cache.record_miss(42);
  for (int i = 0; i < 3; i++) cache.record_hit(42);

  CxlFuseeIndexCacheEntry before;
  assert(cache.lookup(42, &before));
  uint64_t acc = before.access_cnt;
  uint64_t inv = before.invalid_cnt;
  bool bypass_before = before.bypass;

  cache.insert(42, 0x1000, 0xb000);

  CxlFuseeIndexCacheEntry after;
  assert(cache.lookup(42, &after));
  assert(after.slot_addr   == 0x1000);
  assert(after.kvpair_addr == 0xb000);
  assert(after.access_cnt  == acc);
  assert(after.invalid_cnt == inv);
  assert(after.bypass      == bypass_before);
  std::printf("test_insert_existing_preserves_counters PASS\n");
}

void test_evict() {
  CxlFuseeIndexCache cache(8);
  cache.insert(42, 0x1000, 0xa000);
  CxlFuseeIndexCacheEntry e;
  assert(cache.lookup(42, &e));
  cache.evict(42);
  assert(!cache.lookup(42, &e));
  assert(cache.size() == 0);
  cache.evict(99);
  std::printf("test_evict PASS\n");
}

}  // namespace

int main() {
  test_lookup_absent();
  test_insert_lookup_hit();
  test_lru_eviction();
  test_record_hit_increments();
  test_bypass_triggers_above_threshold();
  test_bypass_below_min_samples_holds_off();
  test_bypass_decays_on_read_pressure();
  test_insert_existing_preserves_counters();
  test_evict();
  std::printf("ALL TESTS PASS\n");
  return 0;
}
