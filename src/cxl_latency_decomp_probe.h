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
  kDecompStageCount    = 6,
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

#endif // FUSEE_CXL_LATENCY_DECOMP_PROBE_H_
