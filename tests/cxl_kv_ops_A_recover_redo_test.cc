// Phase 6: CxlKvStoreA::recover_from_oplog replays dangling ops.
//
// Mirror of cxl_kv_ops_C_recover_redo_test.cc but for Option A. Single-host
// scenario (num_hosts=1) sidesteps the inter-host ring entirely; combined
// with recovery_mode_ short-circuit inside dispatch_and_wait, the redo path
// reduces to "local slot write + epoch bump", which is what we want during
// crash recovery anyway.

#include "cxl_kv_ops_A.h"
#include "cxl_mm.h"
#include "cxl_oplog.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
using fusee::OpLog;
using fusee::OpLogKind;

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <dev_path>\n", argv[0]);
    return 2;
  }
  const char *dev = argv[1];

  constexpr uint32_t kNumBuckets = 1024;
  size_t store_bytes = CxlKvStoreA::bytes_for(kNumBuckets);
  size_t oplog_bytes = fusee::oplog_region_bytes();
  size_t needed = ((store_bytes + oplog_bytes + fusee::kCxlDevdaxAlign - 1) /
                   fusee::kCxlDevdaxAlign) *
                  fusee::kCxlDevdaxAlign;

  pid_t pid = fork();
  if (pid < 0) { perror("fork"); return 1; }

  CXLRegion r{};
  if (cxl_region_init(&r, dev, needed) < 0) {
    fprintf(stderr, "cxl_region_init failed\n");
    return 1;
  }
  auto *oplog_base = reinterpret_cast<char *>(r.base) + store_bytes;

  if (pid == 0) {
    CxlKvStoreA store;
    if (store.attach(r.base, store_bytes, kNumBuckets, 0, 1, true) != 0) _exit(1);
    OpLog log;
    log.attach(oplog_base, 0, 1, true);
    store.enable_oplog(&log);

    for (int i = 0; i < 5; i++) {
      if (store.insert(100 + i, 900 + i) != 0) { store.stop(); _exit(2); }
    }
    (void)log.begin(OpLogKind::Update, 100, 0, 0, 900, 999);
    // simulate crash: skip store.stop()
    _exit(0);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
    fprintf(stderr, "child rc=%d\n", WEXITSTATUS(status));
    cxl_region_destroy(&r);
    return 1;
  }

  int fail = 0;
  CxlKvStoreA store;
  if (store.attach(r.base, store_bytes, kNumBuckets, 0, 1, false) != 0) {
    fprintf(stderr, "FAIL: parent attach\n"); cxl_region_destroy(&r); return 1;
  }
  OpLog log;
  log.attach(oplog_base, 0, 1, false);
  store.enable_oplog(&log);

  uint64_t got = 0;
  if (store.search(100, &got) != 0 || got != 900) {
    fprintf(stderr, "pre-recovery key=100 got=%lu want=900 FAIL\n", got); fail++;
  }

  uint64_t acted = store.recover_from_oplog();
  if (acted != 1) {
    fprintf(stderr, "FAIL: expected 1 acted, got %lu\n", acted); fail++;
  }

  got = 0;
  if (store.search(100, &got) != 0 || got != 999) {
    fprintf(stderr, "post-recovery key=100 got=%lu want=999 FAIL\n", got); fail++;
  }
  for (int i = 1; i < 5; i++) {
    got = 0;
    if (store.search(100 + i, &got) != 0 || got != (uint64_t)(900 + i)) {
      fprintf(stderr, "FAIL: committed key=%d got=%lu want=%d\n", 100+i, got, 900+i);
      fail++;
    }
  }

  acted = store.recover_from_oplog();
  if (acted != 0) {
    fprintf(stderr, "FAIL: second recovery acted=%lu, want 0\n", acted); fail++;
  }

  store.stop();
  cxl_region_destroy(&r);
  if (fail == 0) {
    printf("OK: CxlKvStoreA::recover_from_oplog on %s\n", dev);
    return 0;
  }
  return 1;
}
