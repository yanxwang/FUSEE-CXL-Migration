#include "cxl_oplog.h"

#include <cassert>
#include <cstring>

namespace fusee {

static inline uint64_t set_state(OpLogEntry *e, OpLogState s) {
  uint64_t v = static_cast<uint64_t>(s);
  CACHELINE_STORE(&e->state, v);
  return v;
}

static inline OpLogState get_state(OpLogEntry *e) {
  uint64_t v = CACHELINE_LOAD(&e->state);
  return static_cast<OpLogState>(v);
}

void OpLog::attach(void *base, int host_id, int num_hosts, bool init) {
  assert(base != nullptr);
  assert(num_hosts > 0 && num_hosts <= kOpLogMaxHosts);
  region_ = reinterpret_cast<OpLogRegion *>(base);
  host_id_ = host_id;
  num_hosts_ = num_hosts;
  local_tail_ = 0;
  if (init) {
    std::memset(region_, 0, sizeof(*region_));
    flush_region(region_, sizeof(*region_));
    store_fence();
  } else {
    // Pick up wherever this host's prior instance left off.
    local_tail_ = CACHELINE_LOAD(&region_->rings[host_id_].tail);
  }
}

uint64_t OpLog::begin(OpLogKind kind, uint64_t key, uint64_t bucket_idx,
                      uint64_t slot_idx, uint64_t old_value,
                      uint64_t new_value) {
  OpLogRing *ring = &region_->rings[host_id_];
  uint64_t idx = local_tail_++;
  OpLogEntry *e = &ring->entries[idx % kOpLogEntriesPerHost];

  // Fill all fields first; publish state = InProgress last.
  e->seqno     = idx;
  e->host_id   = static_cast<uint32_t>(host_id_);
  e->kind      = kind;
  e->key       = key;
  e->old_value = old_value;
  e->new_value = new_value;
  e->bucket_idx= bucket_idx;
  e->slot_idx  = slot_idx;
  flush_region(e, sizeof(*e));
  store_fence();

  set_state(e, OpLogState::InProgress);

  // Publish the new tail so recovery scans see this entry.
  CACHELINE_STORE(&ring->tail, idx + 1);
  return idx;
}

void OpLog::commit(uint64_t idx) {
  OpLogRing *ring = &region_->rings[host_id_];
  OpLogEntry *e = &ring->entries[idx % kOpLogEntriesPerHost];
  set_state(e, OpLogState::Committed);
}

void OpLog::abort(uint64_t idx) {
  OpLogRing *ring = &region_->rings[host_id_];
  OpLogEntry *e = &ring->entries[idx % kOpLogEntriesPerHost];
  set_state(e, OpLogState::Aborted);
}

uint64_t OpLog::scan_in_progress(InProgressVisitor fn, void *user) {
  uint64_t found = 0;
  for (int h = 0; h < num_hosts_; h++) {
    OpLogRing *ring = &region_->rings[h];
    uint64_t tail = CACHELINE_LOAD(&ring->tail);
    uint64_t start = (tail > kOpLogEntriesPerHost)
                         ? (tail - kOpLogEntriesPerHost) : 0;
    for (uint64_t i = start; i < tail; i++) {
      OpLogEntry *e = &ring->entries[i % kOpLogEntriesPerHost];
      OpLogState s = get_state(e);
      if (s == OpLogState::InProgress) {
        if (fn) fn(e, user);
        found++;
      }
    }
  }
  return found;
}

} // namespace fusee
