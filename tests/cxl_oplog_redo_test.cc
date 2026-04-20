// Phase 6 integration: OpLog::recover_redo drives replay of InProgress ops.
//
// Test outline:
//   1. Host writes 5 oplog entries by hand (InProgress state), simulating a
//      crash mid-insert.
//   2. recover_redo invokes a callback that "applies" each op into a
//      std::map<uint64_t, uint64_t> simulated store and returns 0.
//   3. After recover_redo returns, scan_in_progress must find 0 entries
//      (all transitioned to Committed), and the simulated store must have
//      the right keys.

#include "cxl_mm.h"
#include "cxl_oplog.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>

using fusee::CXLRegion;
using fusee::cxl_region_destroy;
using fusee::cxl_region_init;
using fusee::OpLog;
using fusee::OpLogEntry;
using fusee::OpLogKind;

struct ApplyState {
  std::map<uint64_t, uint64_t> store;
};

static int apply(const OpLogEntry *e, void *user) {
  auto *s = static_cast<ApplyState *>(user);
  switch (e->kind) {
    case OpLogKind::Insert:
    case OpLogKind::Update:
      s->store[e->key] = e->new_value;
      return 0;
    case OpLogKind::Delete:
      s->store.erase(e->key);
      return 0;
    default:
      return -1;
  }
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <dev_path>\n", argv[0]); return 2; }
  const char *dev = argv[1];

  CXLRegion r{};
  if (cxl_region_init(&r, dev, 4UL * 1024 * 1024) < 0) {
    fprintf(stderr, "cxl_region_init failed\n");
    return 1;
  }

  {
    OpLog log;
    log.attach(r.base, 0, 1, /*init=*/true);
    // Emit 5 InProgress entries.
    (void)log.begin(OpLogKind::Insert, 100, 0, 0, 0, 1000);
    (void)log.begin(OpLogKind::Insert, 101, 0, 1, 0, 1001);
    (void)log.begin(OpLogKind::Update, 102, 0, 2, 99, 1002);
    (void)log.begin(OpLogKind::Delete, 103, 0, 3, 33, 0);
    (void)log.begin(OpLogKind::Insert, 104, 0, 4, 0, 1004);
    // Do NOT commit any.
  }

  ApplyState st;
  {
    OpLog log;
    log.attach(r.base, 0, 1, /*init=*/false);
    uint64_t before = log.scan_in_progress(nullptr, nullptr);
    uint64_t acted = log.recover_redo(apply, &st);
    uint64_t after = log.scan_in_progress(nullptr, nullptr);
    printf("before=%lu acted=%lu after=%lu\n", before, acted, after);
    if (before != 5) { fprintf(stderr, "FAIL: before != 5\n"); return 1; }
    if (acted != 5)  { fprintf(stderr, "FAIL: acted != 5\n");  return 1; }
    if (after != 0)  { fprintf(stderr, "FAIL: after != 0\n");  return 1; }
  }

  // Expected after replay: Insert(100,101,104), Update(102), Delete(103).
  // Delete on a never-inserted key is a no-op, leaving 4 keys.
  if (st.store.size() != 4) {
    fprintf(stderr, "FAIL: expected 4 keys after redo, got %zu\n", st.store.size());
    return 1;
  }
  if (st.store[100] != 1000 || st.store[101] != 1001 ||
      st.store[102] != 1002 || st.store.count(103) != 0 ||
      st.store[104] != 1004) {
    fprintf(stderr, "FAIL: replay contents wrong\n");
    return 1;
  }

  cxl_region_destroy(&r);
  printf("OK: OpLog recover_redo on %s\n", dev);
  return 0;
}
