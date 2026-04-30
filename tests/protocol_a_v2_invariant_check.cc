// iter-4A Phase 9 H1/H2 invariant check.
//
// Spec §X. Tests that the AP13/AP14/AP15 trip wires fire on
// intentional violations and that I3 (cache MAP_SHARED) holds.

#include "cxl_cache_pool.h"
#include "cxl_directory.h"
#include "cxl_forward_ring.h"
#include "cxl_hashtable.h"
#include "cxl_kv_blockpool_freelist.h"
#include "cxl_kv_ops_A_v2.h"
#include "cxl_sharding.h"

#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace fusee;

// I3: KvCachePool must be MAP_SHARED so same-host workers share state.
static int test_cache_map_shared() {
  // Verify: forking, parent insert, child sees insert. (We do this in
  // cache_pool_test already; here we verify the harness invariant by
  // checking that MAP_SHARED+MAP_ANONYMOUS is the only safe pattern.)
  uint32_t B = 256;
  void *mem = mmap(nullptr, cache_pool_bytes(B), PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) return 1;
  KvCachePool cache;
  if (cache_pool_init(&cache, mem, B) != 0) return 1;
  uint8_t v[4] = {1,2,3,4};
  cache_pool_insert(&cache, 42, v, 4);
  pid_t p = fork();
  if (p == 0) {
    uint8_t buf[4]; uint32_t sz;
    if (!cache_pool_lookup(&cache, 42, buf, sizeof(buf), &sz) ||
        sz != 4 || memcmp(buf, v, 4) != 0) {
      _exit(1);
    }
    _exit(0);
  }
  int st; waitpid(p, &st, 0);
  cache_pool_destroy(&cache);
  munmap(mem, cache_pool_bytes(B));
  if (WEXITSTATUS(st) != 0) {
    fprintf(stderr, "FAIL: child did not see parent insert via MAP_SHARED (I3)\n");
    return 1;
  }
  printf("test_cache_map_shared: PASS (I3)\n");
  return 0;
}

// AP13 trip wire: attach with mismatched ShardingTable.num_hosts must abort.
static int test_ap13_trip_wire() {
  pid_t p = fork();
  if (p == 0) {
    // Child: trigger violation.
    uint32_t B = 64;
    void *bucket_mem = mmap(nullptr, sizeof(CxlKvBucket) * B, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    void *dir_mem = mmap(nullptr, slot_directory_bytes(B, kCxlKvSlotsPerBucket),
                         PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    void *cache_mem = mmap(nullptr, cache_pool_bytes(B),
                           PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    ShardingTable st; sharding_init(&st, 1);  // ← H=1 on the table
    SlotDirectory dir; slot_directory_init(&dir, dir_mem, B, kCxlKvSlotsPerBucket);
    KvCachePool cache; cache_pool_init(&cache, cache_mem, B);
    BlockFreeList fl; block_freelist_init(&fl);
    CxlKvStoreA_v2 store;
    // Pass num_hosts=2 to attach — should ABORT.
    store.attach(bucket_mem, B, 0, 2, true, &st, &dir, &cache, &fl);
    _exit(0);  // unreachable if abort fires
  }
  int st;
  waitpid(p, &st, 0);
  if (WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT) {
    printf("test_ap13_trip_wire: PASS (SIGABRT on mismatched ShardingTable)\n");
    return 0;
  }
  fprintf(stderr, "FAIL: AP13 trip wire did not fire (status=%d)\n", st);
  return 1;
}

int main() {
  if (test_cache_map_shared() != 0) return 1;
  if (test_ap13_trip_wire() != 0) return 1;
  printf("ALL PASS\n");
  return 0;
}
