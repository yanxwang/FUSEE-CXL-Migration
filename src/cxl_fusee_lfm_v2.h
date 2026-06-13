// FUSEE LFM v2 — Lamport Fast Mutex tuned for CXL Type-3 no-coherence-domain.
//
// Status: design sketch (no implementation yet).
//
// Backing studies (read in order):
//   1. docs/study_cxl_write_atomicity/FINDINGS.md
//      Upper-bound: tried to find the LARGEST atomic publish unit. Found
//      a ~1000× rate cliff at the 3-CL ↔ 4-CL boundary but ALSO that
//      "strict atomic" doesn't exist at K=10⁶ — every size has some
//      non-zero interleave rate.
//   2. docs/study_cxl_write_atomicity/FINDINGS_lower_bound.md
//      Lower-bound: 1B / 8B / 64B aligned single-CL writes are
//      strictly atomic at K=10⁶+ trial bound (P < 10⁻⁶). Cross-CL
//      writes (even 8B straddling a CL boundary) tear at ~10 %.
//   3. docs/study_cxl_write_atomicity/FINDINGS_concurrency.md
//      Concurrency: (a) memcpy + clflushopt does write-allocate and
//      destroys a same-CL false-sharer's data ~75 % of the time;
//      movnti is safe. (b) concurrent readers of a single-CL slot
//      never observe a torn mid-publish state (~7 M reads, 0 tears);
//      multi-CL slots show 0.25–1 % tears.
//
// =============================================================================
// Why LFM v2
// =============================================================================
//
// LFM v1 (cxl_shm_profiling/lfm_lock.c, src/cxl_fusee_slot_lock.cc)
// assumes hardware-coherent shared memory. On g1/g2 CXL Type-3 there is
// NO CPU-level cache coherence across hosts. v1 patches around this by
// pumping every flag write through clflushopt + mfence + peer-read-back,
// costing ~600 ns per primitive operation and ≥4 of them per lock
// acquire (~3 µs uncontended; 9 µs p50 at T=64, p99=691 µs — measured
// iter-9A path_decomp Phase 0).
//
// v2's optimization target is the FENCE COUNT, not the data structure.
// The three studies above proved LFM v1's data layer (1B b[id] + 8B x
// ticket, both aligned single-CL) is strictly atomic and tear-free. The
// algorithm doesn't need a redesigned "big atomic publish"; it needs
// fewer redundant flushes per acquire.
//
// Earlier v2 sketch (committed and then retracted; see "RETRACTED
// DESIGN" section below) proposed a 192 B / 3-CL slot with
// publish_seq torn-publish detection. Both moves were wrong:
//   - 192 B is ~24× over-sized for LFM's actual state (1 B + 8 B)
//   - publish_seq is unnecessary for single-CL slots (FINDINGS_concurrency
//     §4) AND insufficient for multi-CL slots (FINDINGS.md showed even
//     3-CL writes tear at ~1 %, which the seq scheme doesn't fix)
//
// =============================================================================
// Two viable layouts
// =============================================================================
//
// Both are correct per the studies. Pick on memory budget.
//
// LAYOUT A — One cacheline per host slot (RECOMMENDED DEFAULT)
// ----------------------------------------------------------------------
//
//   struct alignas(64) PerHostSlot {  // exactly 64 B
//     uint64_t b;             // Lamport "entering" flag (1 B; padded
//                             // to 8 B for natural alignment + bit ops)
//     uint64_t x;             // Lamport ticket counter
//     uint64_t epoch;         // ABA guard for release/re-acquire
//     uint64_t version;       // increments per release; debugging aid
//     uint64_t last_grant_ns; // when this host last entered CS, fairness
//     uint64_t magic;         // 0x4C464D5632 for slot-attach sanity
//     uint64_t _rsv[2];
//   };
//   static_assert(sizeof(PerHostSlot) == 64, "exactly one cacheline");
//
//   Write path: memcpy(slot, new_state, 64) + clflushopt(slot) + sfence
//   Read path:  clflushopt(slot) + mfence + memcpy(local, slot, 64)
//
//   Why safe:
//   - single-CL aligned write = 1 PCIe txn (FINDINGS.md §3.2)
//   - read while peer writes never tears (FINDINGS_concurrency §4)
//   - no false sharing (each host owns its CL exclusively)
//
//   Memory: 64 B × num_hosts. For 8 hosts = 512 B. Negligible.
//
// LAYOUT B — Packed 8 B per host in shared cacheline (COMPACT)
// ----------------------------------------------------------------------
//
//   struct alignas(64) PackedRing {  // exactly 64 B; holds 8 hosts
//     uint64_t slot[8];   // slot[host_id] = that host's 8 B state
//                         // bit layout in 8B is flexible:
//                         //   bit 0:    b (entering flag)
//                         //   bits 8..63: ticket / epoch / etc.
//   };
//   static_assert(sizeof(PackedRing) == 64, "exactly one cacheline");
//
//   Write path: movnti(slot[host_id], new_state) + sfence
//   Read path:  clflushopt(packed) + mfence + load packed[host_id]
//
//   Why safe:
//   - movnti issues a single 8 B PCIe write with byte enables; covers
//     only host_id's 8 B (FINDINGS_concurrency §3.3, 900 K trials, 0
//     interference)
//   - reader sees consistent state per slot (FINDINGS_concurrency §4,
//     N=8 single-CL = 0 tears in 7 M+ reads)
//
//   Memory: 64 B total for up to 8 hosts.
//
//   *** CRITICAL ***: Layout B writers MUST use movnti, NOT memcpy.
//   memcpy + clflushopt on a shared CL destroys peers' data ~75 % of
//   the time (FINDINGS_concurrency §3.3 — write-allocate + full-CL
//   writeback). This is a hard requirement, not a perf tuning knob.
//
// =============================================================================
// Lamport's algorithm reuses without change
// =============================================================================
//
// Classical Lamport bakery:
//
//   acquire(p):
//     entering[p] = true
//     b[p] = 1 + max(b[q] for all q)
//     entering[p] = false
//     for each q != p:
//       while entering[q]: wait
//       while b[q] != 0 and (b[q], q) < (b[p], p): wait
//
//   release(p):
//     b[p] = 0
//
// On Layout A or B:
//   - "entering[p] = true / b[p] = X / entering[p] = false" maps to one
//     publish_slot(self) — all three fields live in the slot, fit
//     in one CL, are observed atomically by peers
//   - "wait while peer.entering OR peer.b satisfies cond" maps to
//     read_slot_xhost(peers[q]) — one clflushopt + mfence + load
//   - because reads/writes of a single-CL slot are individually
//     atomic and tear-free, NO version-stamp / torn-detection layer
//     is needed
//
// =============================================================================
// Fence count vs LFM v1
// =============================================================================
//
// LFM v1 acquire issues ≥4 clflushopt+mfence per acquire:
//   1. clflushopt(self.entering) := true
//   2. clflushopt(self.b) := my_ticket  (max-scan over peers in between)
//   3. clflushopt(self.entering) := false
//   4. loop: for each peer, clflushopt(peer.slot) + load
//
// LFM v2 acquire:
//   1. memcpy(slot.b, slot.x) into one CL → clflushopt(slot) + sfence
//      (everything in one publish)
//   2. loop: for each peer, clflushopt(peer.slot) + mfence + load
//
// The savings are in step (1): v1 does 3 separate flushes (entering=T,
// b=ticket, entering=F), v2 does 1. At ~600 ns per PCIe round-trip,
// that's ~1.2 µs / acquire saved at uncontended (3 µs → 1.8 µs, ~40 %).
//
// Under contention, v2's tightened publish reduces the window in which
// peers see "entering=T but b not yet final", which also reduces wasted
// loop iterations. Expected:
//   uncontended:    ~3 µs → ~1.2-1.5 µs        (~2-2.5×)
//   contended T=64: p50 9 µs → ~3-4 µs          (~2-3×)
//   contended T=64: p99 691 µs → ~80-150 µs     (~5-8×, from reduced
//                                                contention-loop time)
//
// These are projections from the v1 path_decomp data, NOT measurements.
// v2 implementation + microbench is required to validate.
//
// =============================================================================
// Hard rules (derived from the 4 studies)
// =============================================================================
//
//   R1. Every LFM field must be aligned to its natural width AND fit
//       in a single 64 B cacheline.
//       (FINDINGS_lower_bound Phase A 1.8 M trials = 0 INTL aligned;
//        Phase B 1.2 M = 10 %+ INTL straddle_cl.)
//
//   R2. No field may cross a cacheline boundary.
//       (Same as R1, restated for emphasis.)
//
//   R3. When multiple hosts share a cacheline (Layout B), writers MUST
//       use movnti + sfence. memcpy + clflushopt is forbidden.
//       (FINDINGS_concurrency §3.3, 64-89 % data loss with memcpy.)
//
//   R4. Single-host-per-cacheline (Layout A) is the simpler default;
//       use memcpy + clflushopt freely on the host's own slot.
//
//   R5. Readers ALWAYS clflushopt + mfence + load before reading peer
//       state. The reader's L1 may hold a stale copy from a prior read.
//       (Inherent to no-coherence CXL.)
//
//   R6. Single-CL writes need no version-stamp / torn-publish detection.
//       (FINDINGS_concurrency §4, 0 tears in 7 M+ concurrent reads at
//        N ≤ 64.)
//
//   R7. Do NOT use movnti to write multiple cachelines at once.
//       (FINDINGS.md §3.2, movnti large N can break per-CL atomicity
//        when the WCB drains partially. Use it only for ≤ 8 B
//        targeted at one CL.)
//
// =============================================================================
// RETRACTED DESIGN (kept here for reference; do not implement)
// =============================================================================
//
// The earlier v2 sketch in this header proposed a 192 B (3-cacheline)
// FuseeLfmV2Slot with publish_seq fields on each cacheline. The reader
// would verify all three seq values matched before trusting the
// snapshot. Rationale at the time: "exploit the 3-CL atomic window."
//
// Why it was wrong:
//   - The "3-CL atomic window" claim was based on a 10 K-trial sample
//     that showed 0 events at N=192. At 100 K trials, the same cell
//     shows ~0.013 % median / 1.68 % worst-case interleave rate —
//     not strictly atomic. See FINDINGS.md §3.1.1.
//   - publish_seq retry would have hidden some of those tears at a
//     ~50 ns / acquire amortized cost. But for LFM, the publish doesn't
//     need to be 3 CLs — it only needs to be 1 CL (the actual state
//     is < 32 bytes). Stretching it to 3 CLs introduced a tear risk
//     that the smaller design simply doesn't have.
//   - For LFM specifically: 1-CL or smaller writes are strictly atomic
//     (lower-bound + concurrency studies, ~20 M trials, 0 single-CL
//     tears), so no retry mechanism is needed at all.
//
// The retracted design has been left here as a cautionary example of
// why focusing on the upper bound was the wrong direction for LFM.
//
// =============================================================================
// Open questions for iter-22+ implementation
// =============================================================================
//
// 1. Pick Layout A or B. Layout A is simpler and fits the existing v1
//    code's mental model. Layout B is more compact but requires the
//    movnti discipline. Default to A unless memory profiling shows
//    the 512 B / 8-host overhead matters.
//
// 2. Wire LFM v1 fence audit. Before implementing v2, walk the v1
//    acquire/release code path and count which clflushopt operations
//    are algorithmically necessary vs over-conservative defensive ones.
//    The v2 redesign is the result of this audit, not its driver.
//
// 3. Layout A self-test. Slot includes a magic field so an attaching
//    host can verify the layout is correct on this hardware. (Layout B
//    needs the same but per-host bit-encoded.)
//
// 4. Microbench v1 vs v2 under contention. The projected 2-8× speedups
//    above are extrapolations from v1 path_decomp; verify with a real
//    side-by-side under T=1, 4, 16, 64.
//
// 5. Multi-host (>2) validation. The studies were on 2 hosts. Layout
//    A's correctness at 4-8 hosts should follow from "single-CL = single
//    PCIe txn", but Layout B's correctness depends on each host's
//    movnti targeting its own 8 B chunk being byte-enable-isolated when
//    N hosts contend. Run a 4-host atomicity probe before committing to
//    Layout B in production.

#ifndef FUSEE_CXL_FUSEE_LFM_V2_H_
#define FUSEE_CXL_FUSEE_LFM_V2_H_

#include <cstdint>
#include <cstddef>

namespace fusee {

constexpr std::size_t kFuseeLfmV2MaxHosts = 8;
constexpr uint64_t    kFuseeLfmV2Magic    = 0x4C464D5632000000ULL;  // 'LFMV2\0\0\0'

// LAYOUT A: one cacheline per host slot.
struct alignas(64) FuseeLfmV2SlotA {
  uint64_t b;              // Lamport entering flag
  uint64_t x;              // Lamport ticket
  uint64_t epoch;          // ABA guard
  uint64_t version;        // increments per release
  uint64_t last_grant_ns;  // fairness debug
  uint64_t magic;          // sanity
  uint64_t _rsv[2];
};
static_assert(sizeof(FuseeLfmV2SlotA) == 64,
              "Layout A slot must be exactly 1 cacheline");

// LAYOUT B: packed 8 B per host in shared 64 B cacheline.
struct alignas(64) FuseeLfmV2PackedRing {
  uint64_t slot[8];  // slot[host_id] = host_id's 8 B Lamport state
};
static_assert(sizeof(FuseeLfmV2PackedRing) == 64,
              "Layout B ring must be exactly 1 cacheline");

struct alignas(64) FuseeLfmV2RegionHeader {
  uint64_t magic;
  uint64_t cookie;
  uint64_t num_hosts;
  uint64_t layout;     // 0 = Layout A, 1 = Layout B
  uint64_t _rsv[4];
  uint64_t _pad[8];
};
static_assert(sizeof(FuseeLfmV2RegionHeader) == 128, "header is 2 CL");

// Public API stub for iter-22+ implementation:
//
//   int  fusee_lfm_v2_attach(void *region_base, std::size_t region_bytes,
//                            int host_id, int num_hosts, int layout, bool init);
//   void fusee_lfm_v2_lock(int host_id);
//   void fusee_lfm_v2_unlock(int host_id);
//   int  fusee_lfm_v2_self_test();   // verify single-CL atomicity on this
//                                    // hardware (small subset of the
//                                    // cxl_write_atomicity_probe sweep)

}  // namespace fusee

#endif  // FUSEE_CXL_FUSEE_LFM_V2_H_
