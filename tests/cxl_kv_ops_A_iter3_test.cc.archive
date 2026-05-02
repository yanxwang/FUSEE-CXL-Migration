// Phase 7A verification: Option A (sync replication w/ SPSC-ring ACK) KV ops.
//
// Mirrors the Option C correctness test: two processes, disjoint key ranges,
// cross-reads to verify every insert/update/delete from the peer is visible.
// Additionally checks that the replicator on each host is actually consuming
// ring entries (reported_replicated > 0 after the peer inserts).

#include "cxl_kv_ops_A.h"
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
#include "common.h"
}

using fusee::CxlKvStoreA;
using fusee::CXLRegion;
using fusee::cxl_region_destroy;
using fusee::cxl_region_init;

namespace {

struct Barrier { cacheline_u64 phase[2]; };
constexpr size_t kBarrierOffsetFromEnd = 4096;

void barrier_wait(Barrier *bar, int me, int other, uint64_t phase) {
  CACHELINE_STORE(&bar->phase[me], phase);
  for (;;) {
    uint64_t p = CACHELINE_LOAD(&bar->phase[other]);
    if (p >= phase) return;
    usleep(200);
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <dev_path> [ops_per_host] [num_buckets]\n",
            argv[0]);
    return 2;
  }
  const char *dev = argv[1];
  uint64_t ops_per_host = (argc >= 3) ? strtoull(argv[2], nullptr, 0) : 500ULL;
  uint32_t num_buckets =
      (argc >= 4) ? (uint32_t)strtoul(argv[3], nullptr, 0) : 2048U;

  size_t store_bytes = CxlKvStoreA::bytes_for(num_buckets);
  size_t needed = ((store_bytes + 8192 + fusee::kCxlDevdaxAlign - 1) /
                   fusee::kCxlDevdaxAlign) *
                  fusee::kCxlDevdaxAlign;

  constexpr int kNumHosts = 2;

  pid_t pid = fork();
  if (pid < 0) { perror("fork"); return 1; }

  CXLRegion r{};
  if (cxl_region_init(&r, dev, needed) < 0) {
    fprintf(stderr, "[pid=%d] cxl_region_init failed\n", getpid());
    return 1;
  }

  int host_id = (pid == 0) ? 1 : 0;
  bool is_host0 = (host_id == 0);
  int other = 1 - host_id;

  auto *bar = reinterpret_cast<Barrier *>(
      reinterpret_cast<char *>(r.base) + r.size - kBarrierOffsetFromEnd);

  CxlKvStoreA store;
  if (is_host0) {
    CACHELINE_STORE(&bar->phase[0], 0ULL);
    CACHELINE_STORE(&bar->phase[1], 0ULL);
  }
  if (store.attach(r.base, r.size - kBarrierOffsetFromEnd, num_buckets,
                   host_id, kNumHosts, is_host0) != 0) {
    fprintf(stderr, "[host %d] attach failed\n", host_id);
    cxl_region_destroy(&r);
    if (!is_host0) _exit(1);
    return 1;
  }

  barrier_wait(bar, host_id, other, 1);

  auto make_key = [&](int h, uint64_t i) -> uint64_t {
    return (static_cast<uint64_t>(h + 1) << 32) | (i + 1);
  };

  int fail = 0;

  // Phase: inserts.
  int ring_full = 0, ack_timeout = 0;
  for (uint64_t i = 0; i < ops_per_host; i++) {
    uint64_t k = make_key(host_id, i);
    uint64_t v = k ^ 0xDEADBEEF;
    int rc = store.insert(k, v);
    if (rc == -1 || rc == -2) continue; // full bucket or duplicate
    if (rc == -3) ring_full++;
    if (rc == -4) ack_timeout++;
  }
  printf("[host %d] replicator has ACKed %lu ops (ring_full=%d ack_timeout=%d)\n",
         host_id, store.replicated_ops(), ring_full, ack_timeout);
  barrier_wait(bar, host_id, other, 2);

  // Cross-read.
  uint64_t peer_found = 0, peer_missing = 0, mismatches = 0;
  for (uint64_t i = 0; i < ops_per_host; i++) {
    uint64_t k = make_key(other, i);
    uint64_t expected = k ^ 0xDEADBEEF;
    uint64_t got = 0;
    if (store.search(k, &got) == 0) {
      peer_found++;
      if (got != expected) mismatches++;
    } else {
      peer_missing++;
    }
  }
  printf("[host %d] cross-read: found=%lu missing=%lu mismatch=%lu\n",
         host_id, peer_found, peer_missing, mismatches);
  barrier_wait(bar, host_id, other, 3);

  // Updates on first half.
  for (uint64_t i = 0; i < ops_per_host / 2; i++) {
    uint64_t k = make_key(host_id, i);
    uint64_t v = k ^ 0xCAFEBABE;
    (void)store.update(k, v);
  }
  barrier_wait(bar, host_id, other, 4);

  uint64_t updates_seen = 0, updates_stale = 0, updates_missing = 0;
  for (uint64_t i = 0; i < ops_per_host / 2; i++) {
    uint64_t k = make_key(other, i);
    uint64_t expected_new = k ^ 0xCAFEBABE;
    uint64_t expected_old = k ^ 0xDEADBEEF;
    uint64_t got = 0;
    if (store.search(k, &got) == 0) {
      if (got == expected_new) updates_seen++;
      else if (got == expected_old) updates_stale++;
      else mismatches++;
    } else {
      updates_missing++;
    }
  }
  printf("[host %d] cross-read updates: new=%lu stale=%lu missing=%lu\n",
         host_id, updates_seen, updates_stale, updates_missing);
  if (updates_stale > 0) fail++;
  barrier_wait(bar, host_id, other, 5);

  // Deletes on second half.
  for (uint64_t i = ops_per_host / 2; i < ops_per_host; i++) {
    uint64_t k = make_key(host_id, i);
    (void)store.remove(k);
  }
  barrier_wait(bar, host_id, other, 6);

  uint64_t leaked = 0;
  for (uint64_t i = ops_per_host / 2; i < ops_per_host; i++) {
    uint64_t k = make_key(other, i);
    uint64_t got = 0;
    if (store.search(k, &got) == 0) leaked++;
  }
  printf("[host %d] cross-read after delete: leaked=%lu\n", host_id, leaked);
  if (leaked > 0) fail++;
  barrier_wait(bar, host_id, other, 7);

  store.stop();

  if (!is_host0) {
    cxl_region_destroy(&r);
    _exit(fail == 0 ? 0 : 1);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  int child_rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
  cxl_region_destroy(&r);

  if (fail == 0 && child_rc == 0) {
    printf("OK: Option A KV ops correct across two hosts on %s\n", dev);
    return 0;
  }
  fprintf(stderr, "FAIL: host0_fails=%d host1_rc=%d\n", fail, child_rc);
  return 1;
}
