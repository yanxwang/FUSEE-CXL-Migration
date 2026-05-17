#ifndef FUSEE_CXL_READ_STAGING_H_
#define FUSEE_CXL_READ_STAGING_H_

// Protocol A v2 — read-path staging arena (iter-11A Phase 1).
//
// iter-10A read path = req-then-ack-then-pool-read (3 sequential CXL
// operations). iter-11A forwarder-pool-direct fuses the ack and the
// value-bytes transfer:
//
//   reader → fwd_read req on ReadRing[me][owner]
//          ← owner forwarder writes value bytes DIRECTLY to
//            ReadStaging[me][owner][slot_idx], bumps ready_epoch
//   reader spins on ReadStaging[me][owner][slot_idx].ready_epoch
//
// One CXL roundtrip eliminated (the second pool->read after ack).
//
// Per task_plan_iter11A.md §C13 (forwarder-direct §I9 invariant):
//   - Forwarder MUST tag staging slot with bucket epoch AT THE TIME
//     of cache_pool_lookup. ready_epoch carries this value.
//   - Reader MUST validate `staging.ready_epoch >= my_epoch_at_send`
//     after copy. Stale = retry.
//
// Per-host CXL footprint (defaults: kReadMaxHosts=4,
// kReadRingDepth=256, kReadStagingSlotBytes=1024):
//
//   ReadStagingMatrix = 4 * 4 * 256 * (1024 + 64)  =  ~4.4 MiB
//
// Slot layout per ReadStagingSlot (64-aligned for cacheline ownership):
//   - cacheline 0 (control): { ready_epoch (atomic), key, value_size,
//                              status, _pad }
//   - cachelines 1-16: value_bytes[1024]
//
// status: 0 = ok, -1 = key-not-found (no value bytes valid), other
// negative = error.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "cxl_read_ring.h"

namespace fusee {

constexpr int kReadStagingMaxHosts = kReadMaxHosts;
constexpr int kReadStagingDepth    = kReadRingDepth;
constexpr uint32_t kReadStagingSlotBytes = 1024;  // matches max value_len

struct alignas(64) ReadStagingSlot {
  // Cacheline 0 (control) — written by owner forwarder, polled by reader.
  std::atomic<uint64_t> ready_op_id;   // 0 = not ready; = req_op_id when ready
  uint64_t lookup_epoch;                // bucket epoch at owner lookup time (C13)
  uint64_t key;                         // echoed for sanity check
  uint32_t value_size;                  // 0 = miss / not yet written
  int32_t  status;                      // 0 = ok; -1 = key-not-found
  // iter-13A Phase 1: carries blk_off (CXL pool offset) for RCU/HAZARD
  // direct-pool-read builds. For inline-8B fallback (kSizeClassInline),
  // this field carries the inlined 8 bytes directly. STAGING build
  // sets it (for cross-build CXL layout compatibility) but doesn't read.
  uint64_t resp_blk_off;
  uint8_t  _pad_ctl[64 - 8 - 8 - 8 - 4 - 4 - 8];
  // Cachelines 1-16 — payload bytes (only used by STAGING build).
  uint8_t value_bytes[kReadStagingSlotBytes];
};
static_assert(sizeof(ReadStagingSlot) == 64 + kReadStagingSlotBytes,
              "ReadStagingSlot must be 1 control cacheline + 1024B payload");

struct ReadStagingMatrix {
  // slots[req_host][owner_host][slot_idx] = the response staging slot
  // for ReadRing[req_host][owner_host].entries[slot_idx]. The forwarder
  // (owner) writes; the requester (req_host) polls + reads.
  ReadStagingSlot slots[kReadStagingMaxHosts]
                       [kReadStagingMaxHosts]
                       [kReadStagingDepth];
};

inline std::size_t read_staging_matrix_bytes() {
  return sizeof(ReadStagingMatrix);
}

inline ReadStagingSlot *read_staging_slot(ReadStagingMatrix *rsm,
                                          int req_host, int owner_host,
                                          int slot_idx) {
  return &rsm->slots[req_host][owner_host][slot_idx];
}

}  // namespace fusee

#endif  // FUSEE_CXL_READ_STAGING_H_
