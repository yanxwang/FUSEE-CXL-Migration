// Phase 6 integration: CxlKvStoreC::recover_from_oplog replays dangling ops.
//
// Flow:
//   Child attaches (init=true), enables OpLog, inserts 5 committed ops, then
//   simulates a "crash after log-begin but before apply" by calling
//   log.begin(Update, key=100, new=999) manually, never touching the bucket.
//   The log entry is left in state InProgress. Child _exit(0).
//
//   Parent reattaches (init=false), wires the log into the store via
//   enable_oplog, and calls store.recover_from_oplog(). That one call should:
//     1. see the one InProgress entry
//     2. apply it by calling store.update(100, 999)
//     3. mark the entry Committed
//   After recovery, store.search(100) must yield 999 (not the pre-crash 900).
//
// We also double-check that the other four committed inserts still resolve.

#include "cxl_kv_ops_C.h"
#include "cxl_mm.h"
#include "cxl_oplog.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" {
#include "common.h"
}

using fusee::CxlKvStoreC;
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
  size_t store_bytes = CxlKvStoreC::bytes_for(kNumBuckets);
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
    CxlKvStoreC store;
    if (store.attach(r.base, store_bytes, kNumBuckets, 0, 1, true) != 0) _exit(1);
    OpLog log;
    log.attach(oplog_base, 0, 1, true);
    store.enable_oplog(&log);

    for (int i = 0; i < 5; i++) {
      if (store.insert(100 + i, 900 + i) != 0) _exit(2);
    }
    // Crash simulation: log the intent to update key=100 → new_value=999,
    // but NEVER call store.update — the bucket stays with old_value=900.
    (void)log.begin(OpLogKind::Update, /*key=*/100,
                    /*bucket_idx=*/0, /*slot_idx=*/0,
                    /*old=*/900, /*new=*/999);
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

  // Parent: attach store + oplog read-mode, confirm pre-recovery state.
  CxlKvStoreC store;
  if (store.attach(r.base, store_bytes, kNumBuckets, 0, 1, false) != 0) {
    fprintf(stderr, "FAIL: parent attach\n");
    cxl_region_destroy(&r);
    return 1;
  }
  OpLog log;
  log.attach(oplog_base, 0, 1, false);
  store.enable_oplog(&log);

  uint64_t got = 0;
  if (store.search(100, &got) != 0 || got != 900) {
    fprintf(stderr, "pre-recovery: key=100 got=%lu want=900 (FAIL)\n", got);
    fail++;
  } else {
    printf("pre-recovery: key=100 => 900 (as expected, update not yet replayed)\n");
  }

  // Drive recovery.
  uint64_t acted = store.recover_from_oplog();
  printf("recover_from_oplog acted on %lu entries\n", acted);
  if (acted != 1) {
    fprintf(stderr, "FAIL: expected 1 in-progress entry, acted=%lu\n", acted);
    fail++;
  }

  got = 0;
  if (store.search(100, &got) != 0 || got != 999) {
    fprintf(stderr, "post-recovery: key=100 got=%lu want=999 (FAIL)\n", got);
    fail++;
  } else {
    printf("post-recovery: key=100 => 999 (update replayed)\n");
  }

  // The other four pre-crash inserts must still be intact.
  for (int i = 1; i < 5; i++) {
    got = 0;
    if (store.search(100 + i, &got) != 0 || got != (uint64_t)(900 + i)) {
      fprintf(stderr, "FAIL: committed key=%d got=%lu want=%d\n",
              100 + i, got, 900 + i);
      fail++;
    }
  }

  // Running recovery again must be a no-op (the entry is now Committed).
  acted = store.recover_from_oplog();
  if (acted != 0) {
    fprintf(stderr, "FAIL: second recovery acted=%lu (expected 0, entries should be Committed)\n", acted);
    fail++;
  }

  cxl_region_destroy(&r);
  if (fail == 0) {
    printf("OK: CxlKvStoreC::recover_from_oplog on %s\n", dev);
    return 0;
  }
  return 1;
}
