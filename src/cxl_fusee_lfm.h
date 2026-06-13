#ifndef FUSEE_CXL_FUSEE_LFM_H
#define FUSEE_CXL_FUSEE_LFM_H

// Protocol F-local Lamport's Fast Mutex, hardcoded to 2 hosts.
//
// This is a parallel copy of cxl_shm_profiling/locks/lfm_lock.{h,c} with
// MAX_HOST_NUM specialized to 2.  It exists ONLY for Protocol F so the
// per-slot lock table doesn't pay the 200-host budget that A/B/C need
// (their lock arrays are sometimes per-thread, not per-physical-host).
//
// Per-mutex memory:
//   3 cachelines (magic + x + y) + 3 * kFuseeLfmMaxHosts cachelines
//   = 3 + 3*2 = 9 cachelines = 576 B
//   (vs 3*(200+1) = 603 cachelines ≈ 38.6 KB for the shared 200-host
//    shm_mutex_t).  67× smaller.
//
// Wire-protocol-compatible with the 200-host LFM at the algorithmic
// level — Protocol F only ever locks with id ∈ {0, 1}, so the larger
// b/ready/done arrays in the original were unused dead space for F.
//
// ABI / region-compat: a CXL region initialized by a Protocol F binary
// MUST be attached only by Protocol F binaries; the struct layout
// differs from the 200-host shm_mutex_t.  This is fine — Protocol F
// has its own header magic (FUSEEP_H1) and never co-attaches with
// A/B/C regions.

#include "common.h"   // cacheline_u64, CACHELINE_STORE/LOAD, ...
#include <stdint.h>

#define FUSEE_LFM_MAX_HOSTS 2
#define FUSEE_LFM_MAGIC 0x4655534545324c46ULL   /* "FUSEE2LF" */

typedef struct {
  cacheline_u64 magic;
  cacheline_u64 x;
  cacheline_u64 y;
  cacheline_u64 b[FUSEE_LFM_MAX_HOSTS];
  cacheline_u64 ready[FUSEE_LFM_MAX_HOSTS];
  cacheline_u64 done[FUSEE_LFM_MAX_HOSTS];
} fusee_lfm_t;

#ifdef __cplusplus
extern "C" {
#endif

void     fusee_lfm_init(fusee_lfm_t *m);
uint64_t fusee_lfm_lock(fusee_lfm_t *m, int id, int num_hosts);
void     fusee_lfm_unlock(fusee_lfm_t *m, int id);

#ifdef __cplusplus
}
#endif

#endif  /* FUSEE_CXL_FUSEE_LFM_H */
