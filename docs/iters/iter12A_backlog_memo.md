# iter-12A backlog memo (priority order)

**Source**: end-of-iter-11A (2026-05-11)
**Branch**: `feat/cxl-migration`
**Predecessor**: `iter11A_summary_20260511.md`

iter-11A delivered correctness (bimodal RCA, hash-diff PASS across phases) but **did not deliver throughput gains in the headline sweep** (3 of 5 workloads regressed -4 to -8%; 2 of 5 small improvements +1.4% / +3.2%). 0/5 workloads at the 20 Mops/s bar. iter-12A's job is to recover the gap between Phase 1's verification cell number (workload-c best 18.92 Mops/s) and the Phase 6 sweep result (11.19 Mops/s) — that single discrepancy is the most actionable signal of the iter.

---

## Tier 1 — must-fix from iter-11A (correctness or carve-outs)

### #1: Bisect Phase 1 verification 18.92 vs Phase 6 sweep 11.19 on workload-c

Phase 1's commit message (a27dfda) reports workload-c best 11.7 → 18.92 Mops/s on the post-Phase-1 build with forwarder-pool-direct + ReadStaging. The iter-11A Phase 6 sweep on the post-Phase-4 build shows workload-c best 11.19 Mops/s — a **regression of 7.7 Mops/s** between Phase 1 and Phase 4.

Code changes in between:
- 455379e Phase 2 InvalDispatcher + 8 InvalWorkers (PASSed hash-diff)
- 5664945 Phase 2 revert + Phase 3 doc-only + Phase 4 deferred

Bisect plan:
- Re-test Phase 1's specific verification cell (workload-c T=? cache=? kv=?) on each subsequent commit
- If 455379e (Phase 2 ship) introduces the regression → revert was incomplete; investigate residual code paths
- If 5664945 (revert) introduces the regression → revert removed something that was actually load-bearing in Phase 1; root cause and patch
- If neither → the Phase 1 number was a measurement artifact (single-best-rep with warmup luck); re-validate

**Effort estimate**: ~2-3 h focused bisect.

### #2: Bimodal cell count = 13 (gate-12 FAIL, iter-10A baseline 8) — root cause + targeted fix

iter-11A Phase 6.B 5-rep verify on 53 problematic cells: **13 bimodal** (vs gate-12 ≤ 8). Gate-12 (NEW iter-11A) FAILS; the introducing phase must be identified.

Bimodal cells (full list in iter11A_summary §Bimodal):
- 7 workload-a cells (T ∈ {4, 16, 32, 64}, cache ∈ {on, off}, kv ∈ {256, 512})
- 2 workload-b cells (T={8, 32}, kv=256)
- 3 workload-d cells (T={4, 8, 16}, cache=on, kv=256)
- 1 workload-f cell (T=64, cache=on, kv=512)

**Pattern**: 7/13 at kv=256 (small KV), 11/13 at T ≥ 4, concentrated in workload-a + workload-d. Strongly suggests **hot-bucket retry cascade at small KV** under concurrent Zipf load — exactly the pathology iter-11A Phase 1 (forwarder-pool-direct + ReadStaging + epoch validation) could introduce, since epoch flips on a hot bucket force receiver-side re-reads that cascade through the worker pool.

Root cause hypothesis (most likely → least): (a) Phase 1 epoch retry storm on hot buckets, (b) Phase 0 ReadReceiver null-guard inadvertently changed timing, (c) Phase 2 revert (5664945) left residual InvalDispatcher header code that no-ops most of the time but stalls occasionally.

Plan: targeted fix instead of full Phase 1 revert (since revert kills the only positive sweep deltas on workload-b/d):
1. Bisect commits a27dfda → 5664945 on the 2 most reliably bimodal cells (workloada_T64_off_kv256, workloadd_T8_on_kv256)
2. Add epoch-mismatch counters to forwarder-pool-direct + count per-cell, confirm hypothesis
3. If hypothesis confirmed: bound retry count + fall back to ReadStaging copy on retry-budget-exhausted (worst case == iter-10A behavior)
4. Re-verify 5-rep on the 13 bimodal cells

**Effort estimate**: ~4-6 h (bisect + counter instrumentation + fix + verify).

### #3: 2nd-round path_decomp on Phase 6 new-best cells

Phase 6 sweep produced different best cells than iter-10A for **5 of 5 workloads** (all moved to cache=off at T=64 for at least one workload, vs iter-10A's cache=on mix). Plan §6.C required a 2nd path_decomp round but was deferred to iter-12A.

The 5 new cells:
- workloada T=64 cache=off kv=1024
- workloadb T=64 cache=off kv=512
- workloadc T=64 cache=on kv=512  (cache changed from cache=on kv=1024 to cache=on kv=512)
- workloadd T=64 cache=off kv=256
- workloadf T=64 cache=off kv=256

Why this matters: iter-10A's path_decomp identified R3 / W10 / R1 / I6 as the top-3 stages. The new-best cells (mostly cache=off) shift the read path entirely toward forward_read (not local cache hit), changing the per-stage µs distribution.

**Effort estimate**: ~1.5 h (reuse Phase 5 infrastructure).

---

## Tier 2 — performance ideas deferred from iter-11A in-flight

### #4: Phase 2 parallel-inval-drain redesign (was iter-11A #8)

Phase 2 shipped (455379e) with InvalDispatcher + 8 InvalWorkers + per-bucket FIFO. G1 hash-diff 20/20 PASS but w_p99 regressed 26× → reverted (5664945).

Root cause analysis from the revert commit: per-bucket FIFO (required by C14.a) forced bucket-stripe serialization across the 8 InvalWorkers, re-introducing the contention parallel-drain was supposed to break. Plus 8 InvalWorkers added cache-line ping-pong on the dispatcher's local state.

Redesign options:
- **Bucket-affinity batching**: each InvalWorker owns a fixed bucket-id range; no cross-worker bucket coordination needed. Trade-off: requires hash-stable bucket-to-worker mapping; if iter-12A introduces hot-bucket rebalancing (#5 below), this gets messier.
- **Pure SPSC fanout**: dispatcher writes per-target-host ring; each InvalReceiver on the target host drains. No worker pool needed. Trade-off: doesn't actually help our case where the bottleneck is one host's broadcast → many sharers (1-to-N), not the network (N-to-N).

### #5: Hot-key replication within owner (was iter-11A Phase 3 deferred → iter-12A)

iter-11A Phase 3 investigated 4→16 entries/bucket; reverted because seqlock-CAS already eliminated per-bucket spinlock contention so extra entries only add scan cost (+2.4 µs/insert measured).

Better fix: per-CPU value copies for hot keys + adaptive detection. Write amp = 8× memcpy per insert (one per CPU), so it must be selective (only top-K hot keys per epoch). Read benefit: zero cross-CPU MESI traffic on R1.

Design open questions:
- Hot-key detection: per-epoch lookup-count threshold (e.g., > 10× per-bucket mean count)?
- Replication count: per-CPU (66 copies/key on g3), per-NUMA-node (2 copies), or adaptive?
- Stale-replica policy: what happens when an insert/evict races with a replica write?

### #6: RCU cache_pool re-evaluation post-R3-fix (was iter-11A Phase 4 deferred → iter-12A)

Phase 4.A measured W10 mean 5.99 µs / p99 18.24 µs — not the dominant tail. Deferred.

After iter-12A #1 fixes the R3 regression, re-measure W10. If R3 drops 40% (the original Phase 1 target), then W10 may become top-1 again and RCU's 3-5 µs savings matter.

### #7: Probe-overhead attribution (new from iter-11A Phase 5 analysis)

Phase 5 consolidated delta shows R3 mean +22% slower in iter-11A vs iter-10A on the FUSEE_PROBE=1 build. iter-10A's reference path_decomp ALSO used PROBE=1, so the comparison should be apples-to-apples — yet the regression is large.

Hypothesis: forwarder-pool-direct (Phase 1) adds more probe-instrumented stages on the read path (PROBE_OP("R3"), PROBE_OP("R4"), plus new stages for pre-fetch + epoch validate), so probe overhead is higher in iter-11A than iter-10A even though both have PROBE=1.

Verify: re-run Phase 5 with PROBE=0 build + sampling instrumentation (PROBE_SAMPLE_RATE=1/100 or similar). If PROBE=0 R3 is < iter-10A's PROBE=1 R3, the regression IS probe artifact and Phase 1's perf gain is real.

---

## Tier 3 — long-standing items (still open from iter-10A backlog)

### #8: Variable-length keys

Independent of value-length work. Required for full FUSEE compatibility; orthogonal to perf.

### #9: BucketLockTable removal (-2.5 GB)

Long-standing item from iter-7A backlog. v2 path doesn't use BucketLockTable; ~2.5 GB CXL region wasted.

### #10: Living-docs full §II.3-II.6 narrative rewrite

Plan §iter-11A C7 had this in the Phase 5 living-docs sync table but it was deferred. Original from iter-9A — blueprint Part II §II.3-II.6 still describes pre-iter-9A ring layout in places.

### #11: True fetch_add(N) batching for B0 worker-direct path

iter-10A Phase 3 deferred this (ring-corruption-on-batch-timeout corner case requires receiver-side gap-tolerance). Re-evaluate priority once iter-12A #1 R3 is fixed — if B0 then saturates, batching is the next lever.

---

## How to prioritize iter-12A scope

If iter-12A deadline is < 1 day: **Tier 1 only** (#1 bisect + #2 bimodal + #3 path_decomp = ~6-9 h).

If iter-12A deadline is 2-3 days: Tier 1 + **#7 probe-overhead attribution** (clears the most likely "the perf was actually fine all along, we just mismeasured" hypothesis) + **#4 Phase 2 redesign** (recovers the parallel-inval win without the w_p99 cost).

If iter-12A is meant to actually break 20 Mops/s: Tier 1 + #4 + #5 + #6 + #7 — but realistically, until #1 is bisected and the iter-11A regression is unwound, no other Phase will demonstrate net wins.

The single most actionable item for iter-12A is **#1 (bisect Phase 1 → Phase 6 regression)** — it gates whether iter-11A's correctness work + Phase-1 algorithm change was net-positive at all.
