// Protocol F-local LFM, hardcoded to 2 hosts.  Mirrors
// cxl_shm_profiling/locks/lfm_lock.c with rename + smaller arrays.

#include "cxl_fusee_lfm.h"

#include <string.h>
#include <stdint.h>
#include <time.h>

void fusee_lfm_init(fusee_lfm_t *m) {
  memset(m, 0, sizeof(*m));

  compiler_barrier();
  flush_region(m, sizeof(*m));
  compiler_barrier();

  CACHELINE_STORE(&m->magic, FUSEE_LFM_MAGIC);
  store_fence();
}

uint64_t fusee_lfm_lock(fusee_lfm_t *m, int id, int num_hosts) {
  if (id < 0 || id >= num_hosts || num_hosts <= 0 ||
      num_hosts > FUSEE_LFM_MAX_HOSTS) {
    return UINT64_MAX;
  }

  uint64_t attempts = 0;
  uint64_t backoff_ns = 0;

retry:
  ++attempts;

  // Livelock backoff (identical to the 200-host version).  After 8
  // retries, sleep 10 µs, doubling up to 10 ms.
  if (attempts > 8) {
    if (backoff_ns == 0) backoff_ns = 10000;  // 10 µs
    struct timespec ts = { (time_t)(backoff_ns / 1000000000UL),
                           (long)(backoff_ns % 1000000000UL) };
    nanosleep(&ts, NULL);
    if (backoff_ns < 10000000UL) backoff_ns *= 2;
  }

  CACHELINE_STORE(&m->b[id], 1);
  CACHELINE_STORE(&m->x, (uint64_t)(id + 1));

  if (CACHELINE_LOAD(&m->y) != 0) {
    CACHELINE_STORE(&m->b[id], 0);
    while (CACHELINE_LOAD(&m->y) != 0) relax_cpu();
    goto retry;
  }

  CACHELINE_STORE(&m->y, (uint64_t)(id + 1));

  if (CACHELINE_LOAD(&m->x) != (uint64_t)(id + 1)) {
    CACHELINE_STORE(&m->b[id], 0);
    for (int j = 0; j < num_hosts; ++j) {
      if (j == id) continue;
      while (CACHELINE_LOAD(&m->b[j]) != 0) relax_cpu();
    }
    if (CACHELINE_LOAD(&m->y) != (uint64_t)(id + 1)) {
      while (CACHELINE_LOAD(&m->y) != 0) relax_cpu();
      goto retry;
    }
  }

  return attempts > 0 ? (attempts - 1) : 0;
}

void fusee_lfm_unlock(fusee_lfm_t *m, int id) {
  CACHELINE_STORE(&m->y, 0);
  CACHELINE_STORE(&m->b[id], 0);
}
