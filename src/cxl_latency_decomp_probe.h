#ifndef FUSEE_CXL_LATENCY_DECOMP_PROBE_H_
#define FUSEE_CXL_LATENCY_DECOMP_PROBE_H_

// Compile-time opt-in latency decomposition for the C write path.
//
// When the whole translation unit sees -DFUSEE_LATENCY_DECOMP=1, inline
// calls to decomp_record() append nanosecond samples into per-stage
// vectors on a process-local singleton. With the flag off (the default for
// production builds) every macro expands to nothing, so there is zero
// runtime cost.
//
// The harness (tests/cxl_latency_decomp_C.cc) reads out the vectors at the
// end of the TRANS phase, computes per-stage avg/p50/p99, and publishes
// into a cross-process shared stats region for the primary client to
// aggregate across forked workers.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fusee {

enum DecompStage {
  kDecompStageLock     = 0,  // t1 - t0 : BucketLockTable::lock() returns
  kDecompStageScan     = 1,  // t2 - t1 : 7-slot scan (flush+fence + linear scan)
  kDecompStagePublish  = 2,  // t3 - t2 : publish_slot / value-store + flush/fence
  kDecompStageEpoch    = 3,  // t4 - t3 : bump_epoch
  kDecompStageUnlock   = 4,  // t5 - t4 : unlock (incl. cache invalidate before it)
  kDecompStageTotal    = 5,  // t5 - t0 : end-to-end op
  // Phase-1 LFM anatomy (filled only when the instrumented LFM is linked,
  // see src/lfm_lock_fusee_instrumented.c and CMake FUSEE_LFM_INSTRUMENT).
  kDecompLfmLocalStore = 6,  // b[id]=1 + fence done
  kDecompLfmPeerScan   = 7,  // peer-scan over b[j] j!=id done (fast path)
  kDecompLfmContWait   = 8,  // contention wait (retry loop / b-drain)
  kDecompLfmEnterCS    = 9,  // x/y negotiation done, entering CS
  // iter-3A additions: protocol-A N:1:1:N path probes
  kDecompStageA_AggrEnq    = 10,  // S3 sub: aggregator enqueue (incl. backpressure)
  kDecompStageA_SenderBatch = 11, // sender per-batch cycle time
  kDecompStageA_RecvEntry   = 12, // receiver per-entry cycle (CXL load + apply + atomic_store + ack)
  kDecompStageA_RecvAtomic  = 13, // receiver R3 atomic_store cache_epoch_arr (isolated)
  kDecompStageA_LockL1Scan  = 14, // L1: slot_scan (iter-3A Phase 6 anatomy)
  kDecompStageCount    = 15,
};

struct DecompProbe {
  std::vector<uint32_t> stage_ns[kDecompStageCount];
};

DecompProbe *decomp_probe();
void decomp_probe_reserve(std::size_t expected_ops);
void decomp_probe_reset();

#if defined(FUSEE_LATENCY_DECOMP) && FUSEE_LATENCY_DECOMP
inline void decomp_record(DecompStage s, uint64_t ns) {
  if (ns > 0xFFFFFFFFULL) ns = 0xFFFFFFFFULL;
  decomp_probe()->stage_ns[s].push_back(static_cast<uint32_t>(ns));
}
#else
inline void decomp_record(DecompStage, uint64_t) {}
#endif

} // namespace fusee

// For the C-linkage decomp_record_lfm_stage() entry used by the
// instrumented LFM source (plain C), see cxl_latency_decomp_probe_c.h.

#endif // FUSEE_CXL_LATENCY_DECOMP_PROBE_H_
