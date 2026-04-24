#ifndef FUSEE_CXL_LATENCY_DECOMP_PROBE_C_H_
#define FUSEE_CXL_LATENCY_DECOMP_PROBE_C_H_

// Plain-C shim for the decomp probe so C sources (e.g.
// src/lfm_lock_fusee_instrumented.c) can push per-stage samples without
// pulling in the C++ standard library. The C++ side lives in
// cxl_latency_decomp_probe.h.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(FUSEE_LATENCY_DECOMP) && FUSEE_LATENCY_DECOMP
void decomp_record_lfm_stage(int stage_id, uint64_t ns);
#else
static inline void decomp_record_lfm_stage(int stage_id, uint64_t ns) {
  (void)stage_id; (void)ns;
}
#endif

#ifdef __cplusplus
}
#endif

#endif // FUSEE_CXL_LATENCY_DECOMP_PROBE_C_H_
