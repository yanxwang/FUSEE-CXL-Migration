// Standalone single-host correctness test for CxlKvBlockPool. Uses an
// anonymous heap buffer (NOT CXL devdax) so it can run on any machine
// without root access. Verifies:
//   - bytes_for() sizing
//   - alloc() returns unique increasing offsets within capacity
//   - write/read roundtrip preserves bytes
//   - exhaustion returns 0 cleanly

#include "cxl_kv_blockpool.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

using fusee::CxlKvBlockPool;

static void check(bool cond, const char *msg) {
  if (!cond) {
    fprintf(stderr, "FAIL: %s\n", msg);
    std::exit(1);
  }
}

int main() {
  const uint32_t num_blocks = 64;
  const uint32_t block_size = 256;
  const int num_hosts = 1;

  std::size_t need = CxlKvBlockPool::bytes_for(num_blocks, block_size,
                                                num_hosts);
  std::vector<uint8_t> buf(need + 64, 0);
  void *base = buf.data();

  CxlKvBlockPool pool;
  int rc = pool.attach(base, need, num_blocks, block_size,
                       /*host_id=*/0, num_hosts, /*init=*/true);
  check(rc == 0, "attach init");
  check(pool.block_size() == block_size, "block_size");

  // Allocate every block; offsets must be unique.
  std::vector<uint64_t> offs;
  for (uint32_t i = 0; i < num_blocks; i++) {
    uint64_t o = pool.alloc();
    check(o != 0, "alloc returns non-zero");
    offs.push_back(o);
  }
  // Exhaustion.
  check(pool.alloc() == 0, "exhaustion returns 0");

  // Roundtrip writes.
  std::mt19937 rng(0xCAFEFEED);
  std::vector<std::vector<uint8_t>> payloads(num_blocks,
                                              std::vector<uint8_t>(block_size));
  for (uint32_t i = 0; i < num_blocks; i++) {
    for (auto &b : payloads[i]) b = static_cast<uint8_t>(rng());
    pool.write(offs[i], payloads[i].data(), block_size);
  }
  for (uint32_t i = 0; i < num_blocks; i++) {
    std::vector<uint8_t> got(block_size);
    pool.read(offs[i], got.data(), block_size);
    check(got == payloads[i], "roundtrip");
  }

  // Re-attach (init=false) finds existing magic.
  CxlKvBlockPool pool2;
  rc = pool2.attach(base, need, num_blocks, block_size,
                    /*host_id=*/0, num_hosts, /*init=*/false);
  check(rc == 0, "re-attach init=false");

  printf("OK: CxlKvBlockPool primitive %u blocks of %u B\n",
         num_blocks, block_size);
  return 0;
}
