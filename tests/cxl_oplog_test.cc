// Phase 6 verification: OpLog basic functionality + recovery scan.
//
// Test outline:
//   1. Host 0 attaches a fresh OpLog, performs three begin()/commit() pairs
//      and one begin() without a matching commit (simulating a crash).
//   2. A second "recovery" OpLog attaches over the same buffer with
//      init=false, runs scan_in_progress, and must find exactly one entry.
//
// No multi-process here — OpLog is per-host and the recovery scan runs on
// the primary's behalf. Multi-process correctness is covered by the
// per-protocol kv_ops tests; this test only exercises the OpLog primitive.

#include "cxl_mm.h"
#include "cxl_oplog.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using fusee::CXLRegion;
using fusee::cxl_region_destroy;
using fusee::cxl_region_init;
using fusee::OpLog;
using fusee::OpLogEntry;
using fusee::OpLogKind;

namespace {
int collected_count = 0;
void visit(const OpLogEntry *e, void *user) {
  (void)user;
  collected_count++;
  printf("  scanned in-progress: seqno=%lu host=%u kind=%d key=%lu\n",
         e->seqno, e->host_id, static_cast<int>(e->kind), e->key);
}
} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <dev_path>\n", argv[0]);
    return 2;
  }
  const char *dev = argv[1];
  size_t region_size = 4UL * 1024 * 1024;

  CXLRegion r{};
  if (cxl_region_init(&r, dev, region_size) < 0) {
    fprintf(stderr, "cxl_region_init failed\n");
    return 1;
  }

  // Phase 1: clean attach, do 3 committed ops + 1 dangling begin.
  {
    OpLog log;
    log.attach(r.base, /*host=*/0, /*num_hosts=*/1, /*init=*/true);
    for (int i = 0; i < 3; i++) {
      uint64_t idx = log.begin(OpLogKind::Insert, 100 + i, 5, i, 0, 42 + i);
      log.commit(idx);
    }
    (void)log.begin(OpLogKind::Update, 999, 7, 3, 100, 999); // no commit
    printf("phase 1: 3 committed + 1 dangling\n");
  }

  // Phase 2: recovery attach (init=false), scan.
  int fail = 0;
  {
    OpLog log2;
    log2.attach(r.base, /*host=*/0, /*num_hosts=*/1, /*init=*/false);
    uint64_t found = log2.scan_in_progress(visit, nullptr);
    printf("phase 2: scan found %lu in-progress\n", found);
    if (found != 1) { fprintf(stderr, "FAIL: expected 1 in-progress, got %lu\n", found); fail++; }
    if (collected_count != 1) { fprintf(stderr, "FAIL: visitor called %d times\n", collected_count); fail++; }
  }

  cxl_region_destroy(&r);
  if (fail == 0) {
    printf("OK: OpLog basic + recovery scan on %s\n", dev);
    return 0;
  }
  return 1;
}
