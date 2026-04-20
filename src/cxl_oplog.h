#ifndef FUSEE_CXL_OPLOG_H_
#define FUSEE_CXL_OPLOG_H_

// Per-host operation log on CXL for crash recovery.
//
// A writer records an op entry (state = IN_PROGRESS) before mutating the
// KV store, then flips the state to COMMITTED after the mutation returns.
// On restart, a recovery scan walks every host's log tail and identifies
// IN_PROGRESS entries whose committer never came back — these can then be
// replayed (redo) or rolled back depending on the protocol.
//
// Phase 6 scope: log structure + begin/commit helpers + recovery scanner
// prototype. Actual redo/rollback logic is up to each protocol and will
// land as it is needed (C does not strictly need redo today because the
// bucket writes are serialized under the per-bucket lock).

#include <stdint.h>

extern "C" {
#include "common.h"
}

namespace fusee {

enum class OpLogState : uint64_t {
  Free       = 0,
  InProgress = 1,
  Committed  = 2,
  Aborted    = 3,
};

enum class OpLogKind : uint32_t {
  None   = 0,
  Insert = 1,
  Update = 2,
  Delete = 3,
};

// state is the publish word. alignas(64) so each entry starts on its own
// cacheline; layout otherwise is unpadded — cacheline_u64 itself is already
// 64 bytes, the rest adds up to a second (or third) cacheline depending on
// how the compiler lays it out. Not size-sensitive; recovery scans the tail
// counter, not a contiguous byte offset.
struct alignas(64) OpLogEntry {
  cacheline_u64 state;     // OpLogState (published last)
  uint64_t seqno;
  uint32_t host_id;
  OpLogKind kind;
  uint64_t key;
  uint64_t old_value;      // pre-image for possible rollback
  uint64_t new_value;
  uint64_t bucket_idx;
  uint64_t slot_idx;
};

// Per-host ring. Power-of-two capacity simplifies modulo.
constexpr uint32_t kOpLogEntriesPerHost = 4096;
constexpr int     kOpLogMaxHosts = 4;

struct OpLogRing {
  cacheline_u64 tail;                       // producer cursor for this host
  OpLogEntry    entries[kOpLogEntriesPerHost];
};

struct OpLogRegion {
  OpLogRing rings[kOpLogMaxHosts];
};

inline size_t oplog_region_bytes() {
  return sizeof(OpLogRegion);
}

// View over an OpLogRegion placed in a CXL mapping.
class OpLog {
 public:
  void attach(void *base, int host_id, int num_hosts, bool init);

  // Begin an op: allocate next slot in this host's ring, fill all fields,
  // publish state = InProgress (last write), return the entry index. Caller
  // must eventually call commit() or abort() on the returned index.
  uint64_t begin(OpLogKind kind, uint64_t key, uint64_t bucket_idx,
                 uint64_t slot_idx, uint64_t old_value, uint64_t new_value);

  void commit(uint64_t idx);
  void abort(uint64_t idx);

  // Scan every host's ring for entries in state == InProgress. Returns the
  // number found; caller-supplied visitor fn is invoked for each entry so a
  // protocol can decide redo vs rollback.
  using InProgressVisitor = void (*)(const OpLogEntry *e, void *user);
  uint64_t scan_in_progress(InProgressVisitor fn, void *user);

  // Drive recovery: for each InProgress entry, invoke the redo callback
  // (which should re-apply the op, e.g. by calling CxlKvStore::insert
  // again), then transition the entry to Aborted (so we do not replay it
  // twice). Returns the number of entries acted on.
  using RedoFn = int (*)(const OpLogEntry *e, void *user);
  uint64_t recover_redo(RedoFn fn, void *user);

 private:
  OpLogRegion *region_ = nullptr;
  int host_id_ = -1;
  int num_hosts_ = 0;
  uint64_t local_tail_ = 0; // private per-process producer cursor mirror
};

} // namespace fusee

#endif // FUSEE_CXL_OPLOG_H_
