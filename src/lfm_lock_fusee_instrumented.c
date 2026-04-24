// FUSEE-local instrumented LFM mutex (Phase-1 LFM anatomy, plan §1.1).
//
// Drop-in replacement for $CXL_SHM_PROFILING_DIR/locks/lfm_lock.c with
// four clock_gettime probes around the Lamport's Fast Mutex state
// machine, emitting per-stage nanoseconds into the shared DecompProbe
// via decomp_record_lfm_stage() (defined in cxl_latency_decomp_probe.cc).
//
//   t_a = entry
//   t_b = after first b[id] = 1 + x = id+1 (local-side publish)
//   t_c = after peer scan (first y-observation + its retry / drain)
//   t_d = after x/y negotiation done, immediately before CS entry
//
// Stage interpretation:
//   local_store  = t_b - t_a   : writer's own cacheline publish
//   peer_scan    = t_c - t_b   : cost of observing peer state (y load
//                                in the fast path; y-drain in slow path)
//   cont_wait    = accumulated time in the retry loops (both the
//                  y-drain after a b[id]=0 rollback, and the peer-b[j]
//                  drain before re-checking y); captures queueing
//   enter_cs     = t_d - t_c   : final x-confirmation before CS entry
//
// Build with -DFUSEE_LFM_INSTRUMENT=1 AND -DFUSEE_LATENCY_DECOMP=1; when
// either is missing the probe calls collapse to no-ops.
//
// IMPORTANT: this is a drop-in substitute. The struct layout (shm_mutex_t
// in cxl_shm_profiling/locks/lfm_lock.h) is the one we target — do NOT
// re-declare the type here; we include the upstream header directly.

#include "locks/lfm_lock.h"
#include "cxl_latency_decomp_probe_c.h"

#include <stdint.h>
#include <string.h>
#include <time.h>

// Stage ids must match the enum in cxl_latency_decomp_probe.h — encoded
// here as ints because this is plain C and cannot see the C++ enum.
enum {
  LFM_STAGE_LOCAL_STORE = 6,
  LFM_STAGE_PEER_SCAN   = 7,
  LFM_STAGE_CONT_WAIT   = 8,
  LFM_STAGE_ENTER_CS    = 9,
};

static inline uint64_t lfm_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void shm_mutex_init(shm_mutex_t *m) {
  memset(m, 0, sizeof(*m));
  compiler_barrier();
  flush_region(m, sizeof(*m));
  compiler_barrier();
  CACHELINE_STORE(&m->magic, SHM_MUTEX_MAGIC);
  store_fence();
}

uint64_t shm_mutex_lock(shm_mutex_t *m, int id, int num_hosts) {
  if (id < 0 || id >= num_hosts || num_hosts <= 0 || num_hosts > MAX_HOST_NUM) {
    return UINT64_MAX;
  }

  uint64_t attempts = 0;
  uint64_t cont_wait_ns = 0;

  const uint64_t t_a = lfm_now_ns();

retry:
  ++attempts;

  CACHELINE_STORE(&m->b[id], 1);
  CACHELINE_STORE(&m->x, (uint64_t)(id + 1));

  // First-attempt stamp of local-store completion. On later retries we
  // overwrite so the final t_b reflects the attempt that actually wins.
  // peer_scan / enter_cs are likewise the "last attempt" cost; cont_wait
  // aggregates the unsuccessful work in between.
  const uint64_t t_b_attempt = lfm_now_ns();

  if (CACHELINE_LOAD(&m->y) != 0) {
    CACHELINE_STORE(&m->b[id], 0);
    const uint64_t cw_start = lfm_now_ns();
    while (CACHELINE_LOAD(&m->y) != 0) {
      relax_cpu();
    }
    cont_wait_ns += lfm_now_ns() - cw_start;
    goto retry;
  }

  const uint64_t t_c_attempt = lfm_now_ns();

  CACHELINE_STORE(&m->y, (uint64_t)(id + 1));

  if (CACHELINE_LOAD(&m->x) != (uint64_t)(id + 1)) {
    CACHELINE_STORE(&m->b[id], 0);
    const uint64_t cw_start = lfm_now_ns();
    for (int j = 0; j < num_hosts; ++j) {
      if (j == id) continue;
      while (CACHELINE_LOAD(&m->b[j]) != 0) {
        relax_cpu();
      }
    }
    cont_wait_ns += lfm_now_ns() - cw_start;
    if (CACHELINE_LOAD(&m->y) != (uint64_t)(id + 1)) {
      const uint64_t cw2 = lfm_now_ns();
      while (CACHELINE_LOAD(&m->y) != 0) {
        relax_cpu();
      }
      cont_wait_ns += lfm_now_ns() - cw2;
      goto retry;
    }
  }

  const uint64_t t_d = lfm_now_ns();

#if defined(FUSEE_LFM_INSTRUMENT) && FUSEE_LFM_INSTRUMENT
  // Publish per-stage samples. `t_b_attempt - t_a` includes the cost of
  // any preceding retry iterations' local stores + peer-scan polling on
  // the way back to `retry:`; `cont_wait_ns` is our strict accounting of
  // time spent in the two while-loops, so to isolate acquire-physics we
  // subtract it from local_store + peer_scan.
  uint64_t local_store = (t_b_attempt > t_a) ? (t_b_attempt - t_a) : 0;
  if (local_store > cont_wait_ns) local_store -= cont_wait_ns;
  uint64_t peer_scan = (t_c_attempt > t_b_attempt) ? (t_c_attempt - t_b_attempt) : 0;
  uint64_t enter_cs  = (t_d > t_c_attempt) ? (t_d - t_c_attempt) : 0;

  decomp_record_lfm_stage(LFM_STAGE_LOCAL_STORE, local_store);
  decomp_record_lfm_stage(LFM_STAGE_PEER_SCAN,   peer_scan);
  decomp_record_lfm_stage(LFM_STAGE_CONT_WAIT,   cont_wait_ns);
  decomp_record_lfm_stage(LFM_STAGE_ENTER_CS,    enter_cs);
#else
  (void)t_a; (void)t_b_attempt; (void)t_c_attempt; (void)t_d;
  (void)cont_wait_ns;
#endif

  return attempts > 0 ? (attempts - 1) : 0;
}

void shm_mutex_unlock(shm_mutex_t *m, int id) {
  CACHELINE_STORE(&m->y, 0);
  CACHELINE_STORE(&m->b[id], 0);
}
