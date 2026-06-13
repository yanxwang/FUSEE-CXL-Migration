// FUSEE LFM v2 — Lamport Fast Mutex tuned for CXL Type-3 no-coherence-domain.
//
// Status: design sketch (no implementation yet).
// Backing study: docs/study_cxl_write_atomicity/FINDINGS.md
//
// =============================================================================
// Why LFM v2
// =============================================================================
//
// LFM v1 (see cxl_shm_profiling/lfm_lock.c, src/cxl_fusee_slot_lock.cc)
// was designed assuming hardware-coherent shared memory. On the g1/g2 CXL
// Type-3 testbed there is NO CPU-level cache coherence across hosts. v1
// papers over this by pumping every flag write through clflushopt + mfence
// + peer-read-back, costing ~600 ns per primitive operation and ≥4 of them
// per lock acquire (~3 µs per acquire under no contention; 9 µs p50 under
// contention at T=64, p99=691 µs — measured iter-9A path_decomp Phase 0).
//
// The write-atomicity study at docs/study_cxl_write_atomicity/FINDINGS.md
// established two new facts about g1/g2 hardware:
//
//   (A) cross-host writes of up to 3 cachelines (192 B) issued via
//       cacheable + clflushopt + sfence are STRICTLY ATOMIC — the peer
//       sees either the entire writer's payload or nothing of it. 10K
//       trials at N=192 measured 0 interleave events.
//
//   (B) cross-host non-temporal (movnti) stores are STRICTLY ATOMIC at
//       any size up to a 4 KB page, with atomicity granularity at the
//       8 B chunk level. 10K trials at N={128, 192, 256, 1024} all
//       measured 0 interleave events.
//
// LFM v2 exploits (A): a single 192 B "slot publish" carries all of a
// host's lock-attempt state across hosts in one PCIe burst with strict
// overwrite semantics. Lamport's algorithm collapses from 4+
// clflushopt+mfence per lock to 1 clflushopt + 1 sfence per lock.
//
// Expected microbenchmark gain (estimated from LFM v1 path decomp):
//   - Uncontended lock acquire: 3 µs (v1) → 1.0–1.2 µs (v2)   ~3× speedup
//   - Contended T=64 p50:       9 µs (v1) → 3–4 µs (v2)        ~2-3× speedup
//   - Contended T=64 p99:       691 µs (v1) → 80–150 µs (v2)   ~5-8× speedup
//
// =============================================================================
// Slot layout (192 B per host, "Path A" from FINDINGS §6)
// =============================================================================
//
// Each host owns exactly ONE 192 B slot in the LFM region. Slot layout:
//
//   struct alignas(64) FuseeLfmV2Slot {
//     // ---- Cacheline 0: claim header ----
//     uint64_t magic;          // 0x4C464D 56 32 00 ('LFMV2\0\0\0') for sanity
//     uint64_t seq;            // monotonic acquire counter (Lamport's ticket)
//     uint64_t host_rank;      // == this host's ID; used by Lamport tie-break
//     uint64_t bid;            // local lock-attempt id (b[host] in classical
//                              // Lamport notation); 0 = idle, !=0 = trying or
//                              // in critical section
//     uint64_t epoch;          // optional: ABA guard
//     uint64_t _rsv0[3];       // 64 B alignment
//
//     // ---- Cacheline 1: claim_set bitmap ----
//     // A 512-bit bitmap of which other hosts (slot indices) we observed
//     // as "in critical section" during our last poll. Used by §3.2 below
//     // for a fast "any peer holds the lock?" check.
//     uint64_t claim_set[8];
//
//     // ---- Cacheline 2: reserved / version / metadata ----
//     // For future extension (e.g., adaptive backoff hint, owner address).
//     uint64_t version;        // increments per release; debugging aid
//     uint64_t last_grant_ns;  // when this host last entered CS, for fairness
//     uint64_t _rsv2[6];
//   };
//   static_assert(sizeof(FuseeLfmV2Slot) == 192, "slot must be exactly 3 CL");
//
// All 3 cachelines of a slot are written together as ONE atomic publication:
//
//   // Writer side (called from inside a critical section of the algorithm):
//   void publish_slot(FuseeLfmV2Slot *slot, const FuseeLfmV2Slot &new_state) {
//     // Stage to local DRAM buffer first to ensure the memcpy issues
//     // 3 cacheline writes back-to-back in instruction order.
//     std::memcpy(slot, &new_state, sizeof(*slot));
//     // Now flush all 3 cachelines as one burst. The study showed that
//     // up to 3-CL bursts are atomic across hosts on g1/g2.
//     flush_line(reinterpret_cast<uint8_t *>(slot) + 0);
//     flush_line(reinterpret_cast<uint8_t *>(slot) + 64);
//     flush_line(reinterpret_cast<uint8_t *>(slot) + 128);
//     __asm__ __volatile__("sfence" ::: "memory");
//   }
//
// Atomicity guarantee: when one host reads the slot post-publish (with its
// own clflushopt + mfence to defeat its stale L1 copy), it sees EITHER the
// pre-publish state in full OR the post-publish state in full — never a
// mix. This is the property the study measured at N=192.
//
// =============================================================================
// Lamport's algorithm using LFM v2 publish primitive
// =============================================================================
//
// Classical Lamport bakery lock:
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
// With v2, "entering[p]", "b[p]", and (later) "claim_set[p]" all live in
// ONE 192 B slot. So a single publish_slot() does the algorithm's two
// writes ("set b[p] to ticket", "clear entering[p]") in one wire op.
//
//   acquire_v2(slot_self, slots_all, n_hosts):
//     uint64_t ticket = scan_max_b_plus_1(slots_all, n_hosts);
//     FuseeLfmV2Slot s = *slot_self;
//     s.bid = ticket;
//     s.seq++;
//     publish_slot(slot_self, s);          // <- one atomic 3-CL flush
//
//     for q != self:
//       FuseeLfmV2Slot peer = read_slot_xhost(&slots_all[q]);
//       while (peer.bid != 0 &&
//              (peer.bid, q) < (ticket, self)) {
//         pause();
//         peer = read_slot_xhost(&slots_all[q]);
//       }
//     // Now in critical section.
//
//   release_v2(slot_self):
//     FuseeLfmV2Slot s = *slot_self;
//     s.bid = 0;
//     s.version++;
//     publish_slot(slot_self, s);
//
// "entering[]" guard from classical Lamport is folded into the atomic
// publish: there is no race between "ticket published" and "ticket
// computed" because the publish is atomic, so peers never see a partial
// ticket. This drops one round-trip from the algorithm.
//
// =============================================================================
// Read primitive
// =============================================================================
//
// Cross-host reads must defeat the local L1 to pull fresh CXL state:
//
//   FuseeLfmV2Slot read_slot_xhost(const FuseeLfmV2Slot *slot) {
//     // Flush all 3 cachelines from local L1 so the upcoming load comes
//     // from CXL.
//     flush_line(reinterpret_cast<const uint8_t *>(slot) + 0);
//     flush_line(reinterpret_cast<const uint8_t *>(slot) + 64);
//     flush_line(reinterpret_cast<const uint8_t *>(slot) + 128);
//     __asm__ __volatile__("mfence" ::: "memory");
//     FuseeLfmV2Slot copy;
//     std::memcpy(&copy, slot, sizeof(copy));
//     return copy;
//   }
//
// Atomicity of the READ across the writer's PCIe transactions:
//   - If the writer's 3-CL flush hasn't started, we see the OLD slot.
//   - If the writer's 3-CL flush has fully landed, we see the NEW slot.
//   - The study showed: the in-between state never escapes the wire —
//     either 0 or 3 cachelines have arrived; never 1 or 2.
//
// =============================================================================
// Per-host slot region layout
// =============================================================================
//
// The LFM v2 region holds one 192 B slot per host plus a few cacheline
// of metadata at the head. For up to 8 hosts:
//
//   [64 B  region magic + version]
//   [64 B  num_hosts + region cookie + ...]
//   [192 B slot[0]  (host 0)]
//   [192 B slot[1]  (host 1)]
//   ...
//   [192 B slot[N-1]]
//   [trailing pad to dax page]
//
// All slot[i] are 192 B aligned. Each host writes ONLY to its own slot;
// reads any slot. So per-host write contention does not exist — only
// cross-host atomicity at the slot level matters.
//
// =============================================================================
// Open questions / iter-22 (or wherever LFM v2 lands) work
// =============================================================================
//
// 1. Validate on the contended path. The study measured strict atomicity
//    UNDER the synchronization patterns it used. A real LFM v2 acquire under
//    high contention may issue many more cross-host reads / writes per
//    second. Need a microbench that drives the actual lock-step and counts
//    any spurious "torn slot" reads. Expected: zero.
//
// 2. Fallback when atomicity check fails. The reader could carry a low-
//    overhead sanity check: if magic == 0x4C464D5632 then the slot is
//    valid; if magic is bogus, retry (would only fire if hardware
//    atomicity assumption breaks). One CL of overhead, negligible.
//
// 3. Movnti-based v3 path. If we ever want flag cells > 192 B (e.g.,
//    for a richer consensus structure), the study showed movnti gives
//    arbitrary-size atomicity. v3 would use movnti + sfence on writer,
//    and clflushopt + mfence + load on reader, just like v2.
//
// 4. Measure under load. Replace LFM v1 in src/cxl_fusee_slot_lock.cc
//    with a v2 implementation behind a compile-time flag; run
//    iter-9A redo path_decomp Phase 0 contended-LFM benchmark at T=64
//    and confirm the 2-3× p50 / 5-8× p99 speedup model.
//
// 5. Hardware-bound caveat. Both atomicity guarantees (A) and (B) are
//    specific to g1/g2 hardware (XConn-switched CXL Type-3, this generation
//    of CXL.mem controller). LFM v2 should include a startup self-test
//    that runs a tiny version of the atomicity probe and aborts if the
//    3-CL atomicity invariant doesn't hold. This makes the algorithm
//    self-validating on new hardware.

#ifndef FUSEE_CXL_FUSEE_LFM_V2_H_
#define FUSEE_CXL_FUSEE_LFM_V2_H_

#include <cstdint>
#include <cstddef>

namespace fusee {

constexpr std::size_t kFuseeLfmV2SlotBytes = 192;
constexpr std::size_t kFuseeLfmV2MaxHosts  = 8;
constexpr uint64_t    kFuseeLfmV2Magic     = 0x4C464D5632000000ULL;  // 'LFMV2\0\0\0'

struct alignas(64) FuseeLfmV2Slot {
  // CL 0
  uint64_t magic;
  uint64_t seq;
  uint64_t host_rank;
  uint64_t bid;
  uint64_t epoch;
  uint64_t _rsv0[3];
  // CL 1
  uint64_t claim_set[8];
  // CL 2
  uint64_t version;
  uint64_t last_grant_ns;
  uint64_t _rsv2[6];
};
static_assert(sizeof(FuseeLfmV2Slot) == kFuseeLfmV2SlotBytes,
              "FuseeLfmV2Slot must be exactly 3 cachelines (192 B)");

struct alignas(64) FuseeLfmV2RegionHeader {
  uint64_t magic;
  uint64_t cookie;
  uint64_t num_hosts;
  uint64_t version;
  uint64_t _rsv[4];
  uint64_t _pad[8];
};
static_assert(sizeof(FuseeLfmV2RegionHeader) == 128, "header is 2 CL");

// Public API to implement in iter-22 LFM v2 ship:
//
// int  fusee_lfm_v2_attach(void *region_base, std::size_t region_bytes,
//                          int host_id, int num_hosts, bool init);
// void fusee_lfm_v2_lock(int host_id);
// void fusee_lfm_v2_unlock(int host_id);
// int  fusee_lfm_v2_self_test();   // verify 3-CL atomicity on this hardware

}  // namespace fusee

#endif  // FUSEE_CXL_FUSEE_LFM_V2_H_
