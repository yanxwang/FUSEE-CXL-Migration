#include "cxl_latency_decomp_probe.h"

#if defined(FUSEE_LATENCY_DECOMP) && FUSEE_LATENCY_DECOMP
extern "C" void decomp_record_lfm_stage(int stage_id, uint64_t ns) {
  fusee::decomp_record(static_cast<fusee::DecompStage>(stage_id), ns);
}
#endif

namespace fusee {

DecompProbe *decomp_probe() {
  static DecompProbe probe;
  return &probe;
}

void decomp_probe_reserve(std::size_t expected_ops) {
  DecompProbe *p = decomp_probe();
  for (int i = 0; i < kDecompStageCount; i++) {
    p->stage_ns[i].reserve(expected_ops);
  }
}

void decomp_probe_reset() {
  DecompProbe *p = decomp_probe();
  for (int i = 0; i < kDecompStageCount; i++) {
    p->stage_ns[i].clear();
  }
}

} // namespace fusee
