# iter-14A backlog memo (from iter-13A)

**Date**: 2026-05-17
**Source**: `docs/iters/iter13A_summary_20260517.md`
**Branch**: `feat/cxl-migration`

iter-13A delivered data-copy elimination on both read + write paths but the
aggregate effect on throughput was approximately break-even (workloada -4 %,
workloadb +0.7 %, workloadc -0.7 %, workloadd +1.6 %, workloadf -0.7 %). This
strongly suggests the iter-12A baseline was not bandwidth-bound; **latency**
is the binding constraint. iter-14A should attack latency.

---

## Tier 1 — primary attack on the 20 Mops/s gap

### 1. Hot-key replication (per-host owned copy)

For Zipf workloads (a, b, d, f) the top-K keys account for >50 % of access.
If those top-K keys are replicated at each host (with epoch-coherent
invalidation), cross-host forward_read / forward_write are eliminated entirely
for those keys → reads & writes hit local cache → no CXL roundtrip on the hot
path.

Sketch:
- Track per-key hit count over a sliding window (per-bucket counter)
- When hit count crosses threshold, sender broadcasts "I'm replicating key K";
  receivers commit local copy + add to sharer set
- Writers must invalidate replicas (current InvalRing already does this for
  cache_pool entries; extend to "owned-replica" entries)
- Eviction: LRU or weighted by hit count

Expected gain on workloada: top 10 % of keys = 90 % of accesses (Zipf θ=0.99).
If 50 % of those become local-only, workloada cross-host op count drops 45 %.

Effort: medium-large. Touches cache_pool + invalidate channel + worker
fast-path. Needs careful §I9 ordering (must invalidate all replicas before
visibility of new value).

### 2. CXL roundtrip reduction via send-batching

The current forward_write_direct + forward_read_direct each cost:
- 1 worker fetch_add(tail) + flush_line + sfence
- 1 publish req_op_id + flush_line + sfence
- spin on resp_op_id (multiple CXL polls)

Total ~5-8 CXL ops per cross-host op. If we batch K ops into one CXL "burst":
- Worker batches K requests in local DRAM
- Send batch via 1 large memcpy + sfence
- Receiver processes batch atomically
- Worker waits for K-element ack

Trade-off: latency for individual ops increases (must wait for batch fill),
but throughput per CXL transaction goes up K×. Best for high-T workloads.

Effort: medium. Needs new batched WriteRing path; existing
forward_write_direct stays as fallback for low-T.

### 3. Tier-1 retro: revisit Phase 2 W3 reservation overhead

The W3 dual-track lost because of:
(a) low-T workloada T=4 35 % regression (round-trip cost dominates)
(b) K<64 hang (ring wraparound slot-reuse race)

If hot-key replication (item 1) is implemented, W3 may become viable because
cross-host writes drop substantially → fewer reservation refills → round-trip
cost amortized over more useful work.

Investigate at the same time as W3 hang fix (Tier 2 below).

---

## Tier 2 — iter-13A unfinished / parking lots

### 4. W3 K<64 ring-wraparound slot-reuse hang

`scripts/iter13A_phase2_w3_ksweep.sh` reports K=16 and K=32 hang at high
wraparound count. Likely the worker's "wait-for-slot-free" loop in
forward_write_direct's W3 branch has a race with the receiver's ack
sequence. Investigate:
- Add probes around the reservation handler's spin to characterize hang state
- Likely fix: ensure handler clears slot fields *before* publishing
  resp_op_id, so worker's free-check (req_op_id == 0) only succeeds after
  handler is fully done

### 5. iter-12A Bug B (worker resp_op_id 5 ms timeout)

From iter-12A backlog item 4b. 2 of 102 cross-host writes on workloada T=64
off kv=1024 still 5 ms timeout despite receiver acking. Receiver write
visible to local memory but worker's spin_wait misses it within budget. Add
P5W_ACK_OBSERVED probe to measure ack-propagation delay distribution. Fix
options: extend budget, explicit flush in worker spin, separate cacheline for
resp_op_id.

### 6. Real block reclamation (full G1 GC)

Current pool is bump-only. iter-13A W1 ships the retire-list scaffold but
the actual reclaim path (`rcu_advance_epoch` + `rcu_synchronize` + free) is
never exercised at 200k-op scale because bump never wraps. For production
deployment / longer benchmarks, need:
- Concrete retire-list data structure per (allocator-host, segment)
- Triggered scan when bump approaches segment end
- ABA protection (generation tag in encoded slot value, free 16 bits in
  cxl_slot_pack)

### 7. RCU module unused

iter-13A `cxl_read_guard.h` ships both RCU and HAZARD. HAZARD won the read
fast-path. RCU code is dead unless re-activated. Either:
(a) preserve as alternative for future workloads that prefer epoch model
(b) remove if it bit-rots
Decision deferred — module is small, leave as-is unless space tight.

---

## Tier 3 — long-standing items (still open)

### 8. Variable-length keys

Carryover from iter-10A+. Required for full FUSEE compatibility; orthogonal
to perf. Low priority.

### 9. BucketLockTable removal

Long-standing from iter-7A backlog. v2 path doesn't use it; ~2.5 GB CXL
region wasted.

### 10. Workload e (scan) implementation

Spec §3 has workload-e skipped. Adding it would broaden coverage; not on
the critical path to 20 Mops/s.

---

## Constraints carry-over (from iter-12A precedents — still binding)

- **CLAUDE.md cautionary precedents #1, #2, #3, #4**: descope discipline,
  observation-first RCA, anomaly scan after every sweep, full delivery audit
- **C16 observe-first**, **C17 narrow-targeted fix** (for fixes; iter-13A C5
  exemption for new modules)
- **§I9 strict-A linearizability** — every winner needs hash-diff PASS
- **20 Mops/s target** — binding on workload-a AND workload-c
- **C7 plot after every sweep** — mandatory

---

## Suggested iter-14A first task

**Hot-key replication (item 1)**. Most leveraged for the workloada gap.
Run path_decomp first to confirm cross-host hot-key reads/writes are the
dominant cost; then design replication protocol with §I9-compatible epoch
invalidation. Allow ~2-3 sub-phases (sketch + impl + benchmark).
