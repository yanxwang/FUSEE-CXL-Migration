// FUSEE-local patched build of ticket_lock.c.
//
// The in-tree cxl_shm_profiling ticket_lock.c (both on the orchestrator
// and the g3/g4 slaves we deploy to) claims a ticket with a bare
// __atomic_fetch_add, flushing the cacheline AFTER the RMW but not
// BEFORE. That's unsafe across hosts: the CXL memory server does NOT
// participate in the x86 MESI domain, so the very first fetch_add on
// a host whose local cache still holds the zero-initialised value will
// atomically read 0 → set 1 locally, while the peer host independently
// does the same, handing out two tickets 0 and deadlocking.
//
// This file re-implements ticket_mutex_{init,lock,unlock} with the
// missing pre-flush, and is linked into fusee_cxl / fusee_cxl_decomp in
// place of the upstream object. The upstream header is still used — we
// only replace the implementation.

#include "locks/ticket_lock.h"
#include <stdatomic.h>
#include <string.h>
#include <time.h>

static inline uint64_t *value_ptr_fusee(cacheline_u64 *c) {
  return (uint64_t *)&c->value;
}

void ticket_mutex_init(ticket_mutex_t *m) {
  memset(m, 0, sizeof(*m));
  compiler_barrier();
  flush_region(m, sizeof(*m));
  compiler_barrier();
  CACHELINE_STORE(&m->magic, TICKET_MUTEX_MAGIC);
  store_fence();
}

void ticket_mutex_lock(ticket_mutex_t *m) {
  // Cross-host atomic increment. Pre-flush forces host-local cache off
  // so the fetch_add's read-for-ownership re-fetches from the CXL
  // memory server. Post-flush pushes the new value back out so the
  // peer host's CACHELINE_LOAD observes it.
  compiler_barrier();
  flush_line((void *)value_ptr_fusee(&m->next_ticket));
  full_fence();
  uint64_t my_ticket = __atomic_fetch_add(
      value_ptr_fusee(&m->next_ticket), 1ULL, __ATOMIC_ACQ_REL);
  compiler_barrier();
  flush_line((void *)value_ptr_fusee(&m->next_ticket));
  store_fence();

  uint64_t spins = 0;
  while (CACHELINE_LOAD(&m->now_serving) != my_ticket) {
    spins++;
    relax_cpu();
    if ((spins & 0xFFFF) == 0) {
      struct timespec ts = { 0, 1000 };  // 1 us
      nanosleep(&ts, NULL);
    }
  }
}

void ticket_mutex_unlock(ticket_mutex_t *m) {
  // Only the current holder writes now_serving; a plain read+inc is fine.
  // CACHELINE_STORE bundles clflushopt+sfence, so waiters on any host see it.
  uint64_t cur = CACHELINE_LOAD(&m->now_serving);
  CACHELINE_STORE(&m->now_serving, cur + 1);
}
