#ifndef FUSEE_CXL_READ_GUARD_H_
#define FUSEE_CXL_READ_GUARD_H_

// iter-13A Phase 1: cross-host read pointer protection.
//
// Three modes selected at compile time:
//   FUSEE_READ_GUARD == 0  STAGING   (default — current behavior: owner
//                                     copies value bytes from pool to
//                                     staging slot; reader copies from
//                                     staging slot to DRAM)
//   FUSEE_READ_GUARD == 1  RCU       (epoch-based deferred reclamation;
//                                     reader publishes "I'm reading in
//                                     epoch E" to a per-thread CXL slot;
//                                     owner waits for all readers to
//                                     advance past the target epoch
//                                     before freeing)
//   FUSEE_READ_GUARD == 2  HAZARD    (per-thread hazard pointer slot;
//                                     reader publishes "I'm reading
//                                     blk_off X"; owner scans slots
//                                     before freeing)
//
// Both RCU and HAZARD enable the **direct pool read** path: owner only
// writes (blk_off, vlen, lookup_epoch, status) to staging; reader does
// pool_->read(blk_off, out_buf, vlen) directly — saves the owner-side
// pool→staging copy (which iter-13A baselines as ~1× extra CXL data
// bandwidth per cross-host read).
//
// Important note (iter-13A): current CxlKvBlockPool is bump-only
// (free_lazy is a stub — never frees). So reclamation NEVER triggers
// in iter-13A 200k-ops scaling tests. The fast-path overhead (per-read
// publish/release) IS the only cost being measured here. This is
// intentional: we are choosing the SAFER read-side protection
// mechanism for when real GC lands in iter-14A+, while measuring its
// hot-path tax under realistic workloads.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "common.h"  // flush_line, flush_region, store_fence, full_fence

namespace fusee {

#ifndef FUSEE_READ_GUARD
#define FUSEE_READ_GUARD 0
#endif

#define FUSEE_READ_GUARD_STAGING 0
#define FUSEE_READ_GUARD_RCU     1
#define FUSEE_READ_GUARD_HAZARD  2

// iter-13A Phase 2: write-path data-copy elimination selector.
//   FUSEE_WRITE_ALLOC == 0  STAGING  (default — current behavior: worker
//                                     memcpy into forward_staging, receiver
//                                     copies into pool)
//   FUSEE_WRITE_ALLOC == 1  RESERVED (W1: per-host reserved segments in
//                                     blockpool; worker bumps local DRAM
//                                     cursor + writes direct to peer's
//                                     reserved sub-segment; sends blk_off
//                                     in WriteRing; receiver only updates
//                                     bucket pointer)
//   FUSEE_WRITE_ALLOC == 2  BATCHED  (W3: per-worker DRAM queue of pre-
//                                     reserved blk_offs; refill via
//                                     RESERVE_REQUEST to owner)
#ifndef FUSEE_WRITE_ALLOC
#define FUSEE_WRITE_ALLOC 0
#endif

#define FUSEE_WRITE_ALLOC_STAGING  0
#define FUSEE_WRITE_ALLOC_RESERVED 1
#define FUSEE_WRITE_ALLOC_BATCHED  2

// iter-14A Phase 2: cross-host write worker self-invalidate selector.
//   FUSEE_XHOST_WRITE_SELF_INVAL == 0  (default) — receiver broadcasts
//     invalidate to all non-self sharers including the writer's host;
//     the writer's cache is then invalidated by InvalReceiver.
//   FUSEE_XHOST_WRITE_SELF_INVAL == 1  — writer self-invalidates its
//     local cache BEFORE forwarding (cache_pool_set_stale + tls_evict);
//     receiver excludes writer from invalidate broadcast.  Saves one
//     cross-host InvalRing roundtrip per applicable write.  Correctness:
//     §I9 strict-A preserved (writer's cache is authoritatively stale
//     at the moment of write issuance; receiver still invalidates all
//     OTHER sharers).
#ifndef FUSEE_XHOST_WRITE_SELF_INVAL
#define FUSEE_XHOST_WRITE_SELF_INVAL 0
#endif

// iter-14A P4 F2: cache_pool_lookup LRU write-on-read sampling.
// Default = OFF (legacy behavior).  When = 1, only 1/64 hits update
// `lru_epoch` (via rdtsc lower bits).  LRU is approximate by spec so
// this preserves semantics while removing cacheline-0 ping-pong cost.
#ifndef FUSEE_LRU_SAMPLE
#define FUSEE_LRU_SAMPLE 0
#endif

// Hard limits (must agree across all build configs — both RCU and
// HAZARD domains are always laid out in CXL even if disabled, so the
// offsets between the build flavors stay aligned).
constexpr int kReadGuardMaxHosts   = 4;     // current testbed: 2; cap at 4 for headroom
constexpr int kReadGuardMaxThreads = 128;   // includes worker forks; 2× kMaxClients headroom

// ---- RCU domain (epoch-based) -----------------------------------------

struct alignas(64) RcuThreadSlot {
  std::atomic<uint64_t> reader_epoch;  // 0 = idle, else = epoch at enter
  uint64_t pad[7];
};

struct alignas(64) RcuDomain {
  std::atomic<uint64_t> publish_epoch;  // bumped on every retire
  uint64_t hdr_pad[7];
  RcuThreadSlot slots[kReadGuardMaxHosts][kReadGuardMaxThreads];
};

inline std::size_t rcu_domain_bytes() {
  return sizeof(RcuDomain);
}

// Reader: enter critical section. Publishes the current epoch to my slot.
// Optimization: keeps the last-loaded publish_epoch in DRAM TLS to avoid
// a CXL load on every call; the CXL slot store is still required for
// cross-host visibility.
inline void rcu_enter(RcuDomain *d, int host, int tid) {
  static thread_local uint64_t cached_epoch = 0;
  static thread_local int cached_refresh_counter = 0;
  // Refresh cached_epoch every 64 enters (cheap amortization).
  if ((cached_refresh_counter++ & 63) == 0) {
    flush_line(&d->publish_epoch);
    full_fence();
    cached_epoch = d->publish_epoch.load(std::memory_order_acquire);
  }
  d->slots[host][tid].reader_epoch.store(cached_epoch,
                                          std::memory_order_release);
  flush_line(&d->slots[host][tid].reader_epoch);
  store_fence();
}

inline void rcu_exit(RcuDomain *d, int host, int tid) {
  d->slots[host][tid].reader_epoch.store(0, std::memory_order_release);
  flush_line(&d->slots[host][tid].reader_epoch);
  store_fence();
}

// Owner: advance the publish epoch (called before retire). Returns the
// new epoch — caller must wait via rcu_synchronize(d, new_epoch) before
// reusing any blocks retired at this epoch.
inline uint64_t rcu_advance_epoch(RcuDomain *d) {
  uint64_t prev = d->publish_epoch.fetch_add(1, std::memory_order_acq_rel);
  flush_line(&d->publish_epoch);
  store_fence();
  return prev + 1;
}

// Owner: wait until all reader slots are either 0 (idle) or >= target.
inline void rcu_synchronize(RcuDomain *d, uint64_t target_epoch) {
  for (;;) {
    bool clear = true;
    for (int h = 0; h < kReadGuardMaxHosts && clear; ++h) {
      for (int t = 0; t < kReadGuardMaxThreads; ++t) {
        flush_line(&d->slots[h][t].reader_epoch);
        full_fence();
        uint64_t e =
            d->slots[h][t].reader_epoch.load(std::memory_order_acquire);
        if (e != 0 && e < target_epoch) { clear = false; break; }
      }
    }
    if (clear) return;
    __builtin_ia32_pause();
  }
}

// ---- Hazard pointer domain -------------------------------------------

struct alignas(64) HazardThreadSlot {
  std::atomic<uint64_t> hazard_blk_off;  // 0 = idle, else = currently-reading blk_off
  uint64_t pad[7];
};

struct alignas(64) HazardDomain {
  HazardThreadSlot slots[kReadGuardMaxHosts][kReadGuardMaxThreads];
};

inline std::size_t hazard_domain_bytes() {
  return sizeof(HazardDomain);
}

// Reader: publish blk_off to my hazard slot (called after receiving
// blk_off from owner, before pool_->read).
inline void hazard_protect(HazardDomain *d, int host, int tid,
                           uint64_t blk_off) {
  d->slots[host][tid].hazard_blk_off.store(blk_off,
                                            std::memory_order_release);
  flush_line(&d->slots[host][tid].hazard_blk_off);
  store_fence();
}

inline void hazard_release(HazardDomain *d, int host, int tid) {
  d->slots[host][tid].hazard_blk_off.store(0, std::memory_order_release);
  flush_line(&d->slots[host][tid].hazard_blk_off);
  store_fence();
}

// Owner: returns true if no reader has blk_off currently protected.
inline bool hazard_safe_to_free(HazardDomain *d, uint64_t blk_off) {
  for (int h = 0; h < kReadGuardMaxHosts; ++h) {
    for (int t = 0; t < kReadGuardMaxThreads; ++t) {
      flush_line(&d->slots[h][t].hazard_blk_off);
      full_fence();
      uint64_t p =
          d->slots[h][t].hazard_blk_off.load(std::memory_order_acquire);
      if (p == blk_off) return false;
    }
  }
  return true;
}

// Initialize a domain region (write zeros + flush). Idempotent across
// host attaches (per iter-12A Phase 5 stale-cache lesson: always do
// memset+flush regardless of init flag).
inline void rcu_domain_init(RcuDomain *d) {
  std::memset(d, 0, sizeof(*d));
  flush_region(d, sizeof(*d));
  store_fence();
}

inline void hazard_domain_init(HazardDomain *d) {
  std::memset(d, 0, sizeof(*d));
  flush_region(d, sizeof(*d));
  store_fence();
}

}  // namespace fusee

#endif  // FUSEE_CXL_READ_GUARD_H_
