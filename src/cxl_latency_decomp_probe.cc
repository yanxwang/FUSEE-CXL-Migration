#include "cxl_latency_decomp_probe.h"

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
