// 2-host hash-diff for Protocol F over real CXL devdax.
//
// Usage:
//   cxl_kv_ops_F_2host_hashdiff <dev_path> <host_id> <num_hosts> <run_id>
//
// Both hosts mmap the same /dev/dax* device.  Host 0 calls attach with
// init_region=true; host 1+ waits on init_done_bit and attaches with
// init_region=false.  Each host inserts a disjoint key slab; after both
// finish, each host scans the entire keyspace and prints its content
// hash.  The two hashes must match.
//
// Sync mechanism: a tail-end cookie cell on CXL.  After finishing inserts,
// each host writes its host_id into its own cookie cell + flushes.  Each
// host spins until it sees the peer's cookie set.  Then both hash.
//
// Stage 4c of docs/protocol_F_design_and_plan.md.

#include "cxl_kv_ops_F.h"
#include "cxl_mm.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
#include "common.h"  // flush_line, store_fence, full_fence
}

using namespace fusee;

namespace {

constexpr uint32_t kNumBuckets    = 4096;      // small for quick test
constexpr uint64_t kTotalRecords  = 16384;
constexpr int      kPerHostKeys   = 2000;
constexpr uint64_t kCookieMagic   = 0x46555345434f4f4bULL;  // "FUSEECOOK"

struct TailCookies {
  std::atomic<uint64_t> host_done[4];  // each host writes 1 + run_id when done
  char pad[256 - 32];
};
static_assert(sizeof(TailCookies) <= 256);

uint64_t mix(uint64_t k, uint64_t v) {
  return (k * 0x9e3779b97f4a7c15ULL) ^ v;
}

uint64_t mk(uint64_t i, int host_id) {
  // Host 0: even keys 2,4,6,...   Host 1: odd keys 1,3,5,...
  return 2 * i + 1 + host_id;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 5) {
    std::fprintf(stderr,
        "usage: %s <dev_path> <host_id> <num_hosts> <run_id>\n", argv[0]);
    return 1;
  }
  const char *dev_path = argv[1];
  int host_id          = std::atoi(argv[2]);
  int num_hosts        = std::atoi(argv[3]);
  uint64_t run_id      = std::strtoull(argv[4], nullptr, 0);

  if (host_id < 0 || host_id >= num_hosts || num_hosts > 4) {
    std::fprintf(stderr, "bad host_id/num_hosts\n");
    return 1;
  }

  std::size_t store_bytes =
      CxlKvStoreF::bytes_for(kNumBuckets, kTotalRecords, num_hosts);
  std::size_t total_bytes = store_bytes + sizeof(TailCookies);

  CXLRegion r{};
  if (cxl_region_init(&r, dev_path, total_bytes) != 0) {
    std::perror("cxl_region_init");
    return 1;
  }

  // Quick sanity print of region pointer + bytes_for footprint.
  std::fprintf(stderr,
      "host %d: dev=%s base=%p map_size=%zu store_need=%zu\n",
      host_id, dev_path, r.base, r.size, store_bytes);

  CxlKvStoreF store;
  int rc = store.attach(r.base, store_bytes, kNumBuckets, kTotalRecords,
                        host_id, num_hosts,
                        /*init_region=*/(host_id == 0));
  if (rc != 0) {
    std::fprintf(stderr, "host %d: attach failed rc=%d\n", host_id, rc);
    // Dump first 32 bytes of header for postmortem
    uint64_t *p = reinterpret_cast<uint64_t *>(r.base);
    flush_line(p);
    full_fence();
    std::fprintf(stderr,
        "  hdr[0..4]: %016lx %016lx %016lx %016lx\n",
        p[0], p[1], p[2], p[3]);
    return 1;
  }
  std::fprintf(stderr, "host %d: attach OK\n", host_id);

  // Cookies sit at the very tail of the region.
  TailCookies *cookies = reinterpret_cast<TailCookies *>(
      reinterpret_cast<uint8_t *>(r.base) + store_bytes);
  if (host_id == 0) {
    std::memset(cookies, 0, sizeof(*cookies));
    flush_region(cookies, sizeof(*cookies));
    store_fence();
  }

  // Insert this host's keys.
  for (int i = 0; i < kPerHostKeys; i++) {
    uint64_t k = mk(i, host_id);
    uint64_t v = k * 100 + host_id;
    int r2 = store.insert(k, v);
    if (r2 != 0) {
      std::fprintf(stderr, "host %d: insert %d (k=%lu) rc=%d\n",
                   host_id, i, k, r2);
      return 1;
    }
  }
  std::fprintf(stderr, "host %d: inserts done\n", host_id);

  // Publish completion cookie.
  cookies->host_done[host_id].store(kCookieMagic + run_id,
                                    std::memory_order_release);
  flush_line(&cookies->host_done[host_id]);
  store_fence();

  // Wait for peer hosts.
  for (int h = 0; h < num_hosts; h++) {
    if (h == host_id) continue;
    for (;;) {
      flush_line(&cookies->host_done[h]);
      full_fence();
      if (cookies->host_done[h].load(std::memory_order_acquire) ==
          kCookieMagic + run_id) break;
      for (int p = 0; p < 1024; p++) __builtin_ia32_pause();
    }
  }
  std::fprintf(stderr, "host %d: peer sync done\n", host_id);

  // Scan and hash the entire keyspace from this host's perspective.
  uint64_t hash = 0;
  uint64_t found = 0, missing = 0, bad_value = 0;
  for (int h = 0; h < num_hosts; h++) {
    for (int i = 0; i < kPerHostKeys; i++) {
      uint64_t k = mk(i, h);
      uint64_t expected_v = k * 100 + h;
      uint64_t v = 0;
      int r2 = store.search(k, &v);
      if (r2 != 0) { missing++; continue; }
      if (v != expected_v) { bad_value++; continue; }
      hash ^= mix(k, v);
      found++;
    }
  }

  std::printf("host %d hash=%016lx found=%lu missing=%lu bad_value=%lu\n",
              host_id, hash, found, missing, bad_value);

  cxl_region_destroy(&r);
  return (missing == 0 && bad_value == 0) ? 0 : 2;
}
