// Unit test for CxlFuseeKvPairPool.  Covers:
//   1. attach + bytes_for accounting
//   2. alloc returns distinct offsets within host segment
//   3. write/read round-trip
//   4. free + reclaim_pass round-trip
//   5. exhaust + reclaim refill
//   6. cross-process visibility via fork (peer host frees, our host reclaims)
//
// Stage 1.3 of docs/protocol_F_design_and_plan.md.

#include "cxl_fusee_kvpair_pool.h"
#include "cxl_mm.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace fusee;

namespace {

constexpr uint64_t kTotalRecords = 1024;
constexpr int      kHosts        = 2;

void *map_shared(size_t bytes) {
  void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    perror("mmap");
    std::abort();
  }
  return p;
}

// -- 1. attach + bytes_for ---------------------------------------------------
void test_attach_basic() {
  size_t need = CxlFuseeKvPairPool::bytes_for(kTotalRecords, kHosts);
  void *region = map_shared(need);

  CxlFuseeKvPairPool pool;
  int rc = pool.attach(region, need, kTotalRecords, /*host_id=*/0, kHosts,
                       /*init_region=*/true);
  assert(rc == 0);
  assert(pool.valid());
  assert(pool.total_records() == kTotalRecords);
  assert(pool.records_per_host() == kTotalRecords / kHosts);

  // Re-attach from "host 1": init=false should succeed.
  CxlFuseeKvPairPool pool1;
  rc = pool1.attach(region, need, kTotalRecords, /*host_id=*/1, kHosts,
                    /*init_region=*/false);
  assert(rc == 0);

  munmap(region, need);
  std::printf("test_attach_basic PASS\n");
}

// -- 2. alloc returns distinct offsets within host segment -------------------
void test_alloc_distinct() {
  size_t need = CxlFuseeKvPairPool::bytes_for(kTotalRecords, kHosts);
  void *region = map_shared(need);

  CxlFuseeKvPairPool pool;
  pool.attach(region, need, kTotalRecords, 0, kHosts, true);

  std::set<uint64_t> seen;
  for (uint64_t i = 0; i < kTotalRecords / kHosts; i++) {
    uint64_t off = pool.alloc_tiny();
    assert(off != 0);
    assert(seen.insert(off).second && "duplicate offset from alloc");
  }
  // Next alloc should fail (segment exhausted, no frees yet).
  uint64_t off = pool.alloc_tiny();
  assert(off == 0);

  munmap(region, need);
  std::printf("test_alloc_distinct PASS\n");
}

// -- 3. write / read round-trip ---------------------------------------------
void test_write_read() {
  size_t need = CxlFuseeKvPairPool::bytes_for(kTotalRecords, kHosts);
  void *region = map_shared(need);

  CxlFuseeKvPairPool pool;
  pool.attach(region, need, kTotalRecords, 0, kHosts, true);

  uint64_t off = pool.alloc_tiny();
  assert(off != 0);

  CxlFuseeTinyRecord rec_in  {0xdeadbeef, 0xfeedface};
  CxlFuseeTinyRecord rec_out {0, 0};
  pool.write(off, &rec_in, sizeof(rec_in));
  pool.read(off, &rec_out, sizeof(rec_out));
  assert(rec_out.key   == rec_in.key);
  assert(rec_out.value == rec_in.value);

  munmap(region, need);
  std::printf("test_write_read PASS\n");
}

// -- 4. free + reclaim --------------------------------------------------------
void test_free_reclaim() {
  size_t need = CxlFuseeKvPairPool::bytes_for(kTotalRecords, kHosts);
  void *region = map_shared(need);

  CxlFuseeKvPairPool pool;
  pool.attach(region, need, kTotalRecords, 0, kHosts, true);

  // Allocate some, capture the offsets.
  std::vector<uint64_t> allocated;
  for (int i = 0; i < 10; i++) {
    uint64_t off = pool.alloc_tiny();
    assert(off != 0);
    allocated.push_back(off);
  }

  // Free 5 of them.
  for (int i = 0; i < 5; i++) {
    pool.free(allocated[i]);
  }

  // Reclaim: should harvest exactly 5 offsets back into the local free list.
  uint32_t harvested = pool.reclaim_pass();
  assert(harvested == 5);

  // Next 5 allocs should come from the local free list (= the freed offsets).
  std::set<uint64_t> freed_set(allocated.begin(), allocated.begin() + 5);
  for (int i = 0; i < 5; i++) {
    uint64_t off = pool.alloc_tiny();
    assert(off != 0);
    assert(freed_set.erase(off) == 1 && "alloc returned an off we didn't free");
  }
  assert(freed_set.empty());

  munmap(region, need);
  std::printf("test_free_reclaim PASS\n");
}

// -- 5. exhaust + reclaim refill ---------------------------------------------
void test_exhaust_then_reclaim() {
  size_t need = CxlFuseeKvPairPool::bytes_for(kTotalRecords, kHosts);
  void *region = map_shared(need);

  CxlFuseeKvPairPool pool;
  pool.attach(region, need, kTotalRecords, 0, kHosts, true);

  std::vector<uint64_t> allocated;
  for (uint64_t i = 0; i < kTotalRecords / kHosts; i++) {
    uint64_t off = pool.alloc_tiny();
    assert(off != 0);
    allocated.push_back(off);
  }
  assert(pool.alloc_tiny() == 0);  // exhausted

  // Free 10, then alloc — alloc_tiny() should detect exhaustion, reclaim,
  // and serve the freed offsets.
  for (int i = 0; i < 10; i++) pool.free(allocated[i]);

  std::set<uint64_t> freed_set(allocated.begin(), allocated.begin() + 10);
  for (int i = 0; i < 10; i++) {
    uint64_t off = pool.alloc_tiny();
    assert(off != 0);
    assert(freed_set.erase(off) == 1);
  }
  assert(freed_set.empty());

  munmap(region, need);
  std::printf("test_exhaust_then_reclaim PASS\n");
}

// -- 6. cross-process: child frees, parent reclaims --------------------------
void test_cross_process_free() {
  size_t need = CxlFuseeKvPairPool::bytes_for(kTotalRecords, kHosts);
  void *region = map_shared(need);

  // Parent acts as host 0, init the region.
  CxlFuseeKvPairPool parent_pool;
  parent_pool.attach(region, need, kTotalRecords, 0, kHosts, true);

  // Parent allocates 20 records from its segment.
  std::vector<uint64_t> allocated;
  for (int i = 0; i < 20; i++) {
    uint64_t off = parent_pool.alloc_tiny();
    assert(off != 0);
    allocated.push_back(off);
  }

  // Use a small shared region to hand offsets to the child.
  uint64_t *shared_offsets = static_cast<uint64_t *>(
      map_shared(sizeof(uint64_t) * 20));
  std::memcpy(shared_offsets, allocated.data(), sizeof(uint64_t) * 20);

  pid_t pid = fork();
  if (pid == 0) {
    // Child: attach as host 1 (no init), then free the parent's records.
    // Note: host 1 is freeing records that belong to host 0's segment — that
    // is the cross-host free path we want to test.
    CxlFuseeKvPairPool child_pool;
    int rc = child_pool.attach(region, need, kTotalRecords, 1, kHosts, false);
    assert(rc == 0);
    for (int i = 0; i < 20; i++) {
      child_pool.free(shared_offsets[i]);
    }
    _exit(0);
  }
  assert(pid > 0);
  int status = 0;
  waitpid(pid, &status, 0);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  // Parent reclaims its segment — should see all 20 offsets.
  uint32_t harvested = parent_pool.reclaim_pass();
  assert(harvested == 20);

  munmap(shared_offsets, sizeof(uint64_t) * 20);
  munmap(region, need);
  std::printf("test_cross_process_free PASS\n");
}

}  // namespace

int main() {
  test_attach_basic();
  test_alloc_distinct();
  test_write_read();
  test_free_reclaim();
  test_exhaust_then_reclaim();
  test_cross_process_free();
  std::printf("ALL TESTS PASS\n");
  return 0;
}
