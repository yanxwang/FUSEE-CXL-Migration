// Phase 6 integration test: CxlKvStoreC + OpLog, simulated crash, recovery.
//
// Layout in the CXL region:
//   [0            ..  store_bytes)     CxlKvStoreC
//   [store_bytes  ..  store_bytes+OL)  OpLogRegion
//
// Flow:
//   1. Parent forks a child.
//   2. Child attaches (init=true), enables OpLog, does N inserts, then
//      *abandons* the last op by calling oplog.begin() manually (no commit)
//      and _exit(0) without commit — the state word stays = InProgress.
//   3. Parent waits for child, then attaches (init=false) in its own
//      process, runs OpLog.scan_in_progress, and must find exactly one
//      entry whose kind matches the abandoned op.

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
using fusee::OpLogEntry;
using fusee::OpLogKind;

namespace {
int scan_count = 0;
uint64_t last_key = 0;
OpLogKind last_kind = OpLogKind::None;
void visit(const OpLogEntry *e, void *user) {
  (void)user;
  scan_count++;
  last_key = e->key;
  last_kind = e->kind;
}
} // namespace

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
    // Child: init, run committed ops, then simulate crash mid-op.
    CxlKvStoreC store;
    if (store.attach(r.base, store_bytes, kNumBuckets, 0, 1, true) != 0) _exit(1);
    OpLog log;
    log.attach(oplog_base, 0, 1, true);
    store.enable_oplog(&log);

    for (int i = 0; i < 10; i++) {
      if (store.insert(100 + i, 900 + i) != 0) _exit(2);
    }

    // Simulate "crash mid-op": begin a log entry with a distinctive key and
    // exit without committing. The state word stays InProgress.
    (void)log.begin(OpLogKind::Update, /*key=*/0xCAFEBABEULL,
                    /*bucket_idx=*/7, /*slot_idx=*/3,
                    /*old=*/111, /*new=*/222);
    // Note: do NOT stop the store — we want to simulate abrupt termination.
    _exit(0);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
    fprintf(stderr, "child exited abnormally rc=%d\n", WEXITSTATUS(status));
    cxl_region_destroy(&r);
    return 1;
  }

  // Parent: attach OpLog on top of what the child left, run recovery scan.
  OpLog log;
  log.attach(oplog_base, 0, 1, false);
  uint64_t found = log.scan_in_progress(visit, nullptr);

  int fail = 0;
  if (found != 1) { fprintf(stderr, "FAIL: want 1 in-progress, got %lu\n", found); fail++; }
  if (scan_count != 1) { fprintf(stderr, "FAIL: visitor called %d times\n", scan_count); fail++; }
  if (last_key != 0xCAFEBABEULL) {
    fprintf(stderr, "FAIL: recovered key=%lx want CAFEBABE\n", last_key); fail++;
  }
  if (last_kind != OpLogKind::Update) {
    fprintf(stderr, "FAIL: recovered kind=%d want Update(2)\n",
            static_cast<int>(last_kind)); fail++;
  }

  // Bonus: verify the 10 committed inserts did survive (via the store).
  CxlKvStoreC store;
  if (store.attach(r.base, store_bytes, kNumBuckets, 0, 1, false) != 0) {
    fprintf(stderr, "FAIL: parent reattach\n"); fail++;
  } else {
    for (int i = 0; i < 10; i++) {
      uint64_t out = 0;
      if (store.search(100 + i, &out) != 0 || out != (uint64_t)(900 + i)) {
        fprintf(stderr, "FAIL: committed op %d lost (got %lu, want %d)\n",
                i, out, 900 + i);
        fail++;
      }
    }
  }

  cxl_region_destroy(&r);
  if (fail == 0) {
    printf("OK: Option C + OpLog crash-recover scan on %s\n", dev);
    return 0;
  }
  return 1;
}
