// Standalone sanity test for src/cxl_mm — no libddckv / no RDMA deps.
//
// Usage:
//   ./cxl_mm_test <dev_path> [size_bytes]
//   ./cxl_mm_test /dev/dax0.0 $((8*1024*1024))
//   ./cxl_mm_test /tmp/cxl_mm_scratch.bin $((4*1024*1024))
//
// Writes a magic pattern, reads it back, verifies, then cleans up.

#include "cxl_mm.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using fusee::CXLRegion;
using fusee::cxl_region_destroy;
using fusee::cxl_region_init;

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <dev_path> [size_bytes]\n", argv[0]);
    return 2;
  }
  const char *dev = argv[1];
  size_t size = (argc >= 3) ? (size_t)strtoull(argv[2], nullptr, 0)
                            : (size_t)(8UL * 1024 * 1024);

  CXLRegion r{};
  if (cxl_region_init(&r, dev, size) < 0) {
    fprintf(stderr, "cxl_region_init failed\n");
    return 1;
  }
  printf("mapped %s: base=%p size=%zu (aligned from %zu)\n", r.dev_path,
         r.base, r.size, size);

  // Write magic at the beginning and near the end.
  constexpr uint64_t kMagic = 0xFEEDFACECAFEBABEULL;
  uint64_t *head = reinterpret_cast<uint64_t *>(r.base);
  uint64_t *tail = reinterpret_cast<uint64_t *>(
      reinterpret_cast<char *>(r.base) + r.size - sizeof(uint64_t));

  *head = kMagic;
  *tail = ~kMagic;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);

  uint64_t got_head = *head;
  uint64_t got_tail = *tail;

  int rc = 0;
  if (got_head != kMagic) {
    fprintf(stderr, "FAIL head: got %016lx want %016lx\n", got_head, kMagic);
    rc = 1;
  }
  if (got_tail != ~kMagic) {
    fprintf(stderr, "FAIL tail: got %016lx want %016lx\n", got_tail, ~kMagic);
    rc = 1;
  }

  if (rc == 0) {
    printf("OK: head=%016lx tail=%016lx\n", got_head, got_tail);
  }

  cxl_region_destroy(&r);
  return rc;
}
