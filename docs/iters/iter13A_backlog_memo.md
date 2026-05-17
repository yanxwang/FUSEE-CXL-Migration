# iter-13A backlog memo (from iter-12A)

**Date**: 2026-05-16 (drafted alongside iter-12A summary)
**Updated**: 2026-05-17 (Phase 5 complete: V1 falsified, V3 confirmed, fixed; items #2 and #4 now CLOSED)
**Source**: `docs/iters/iter12A_summary_20260516.md` + `docs/iters/iter12A_phase5_rca.md`

> **✅ PULLED-BACK ITEMS CLOSED 2026-05-17**
>
> Items #2 (parent-CPU-0 jitter mitigation) and #4 (CPU isolation escalation)
> were pulled into iter-12A Phase 5 because they presumed V1 is the cause.
> Phase 5 direct-probe RCA **falsified V1** (P5R_PL heartbeat showed g4
> WriteReceiver on CPU 65 polling continuously for 118 s with no preempt
> gaps) and identified the true root cause as **V3: host-1 cache-stale
> `ring->head` on init=false reattach**. Fix landed in 3 functions of
> `src/cxl_kv_ops_A.cc` and verified (60/60 WIN on residual cells +
> 40/40 WIN regression spot-check). Items #2 and #4 are CLOSED — they
> would have addressed a non-cause. See [`iter12A_phase5_rca.md`](iter12A_phase5_rca.md).
>
> iter-13A backlog below now has:
> - **Bug B** (new, from Phase 5 cell C1 capture): 2/102 worker timeouts
>   where receiver acked but worker missed the ack within 5 ms — separate
>   from Bug A, smaller impact, not bimodal-class. See item 4 below.
> - **Hash-diff post-fix verification** as a gate before iter-13A
>   optimization work begins.

iter-12A delivered the bimodal fix (gate-12 PASS with margin: 0 / 13 BIMODAL). With that data-reliability blocker removed, iter-13A's optimization aims become data-driven and pivotable.

---

## Mandatory carry-over

### 1. Phase 3 2nd-round path_decomp on iter-12A new-best cells

iter-11A and iter-12A both deferred this. Rationale for iter-12A deferral: Phase 1.7 gate-12 PASS + Phase 2 R3 stable evidence made the original motivation (validate fix at decomp level) optional. iter-13A picks up: with Phase 4 sweep complete, pick new-best cells per workload (5 cells × healthy + worst), build with `FUSEE_PROBE=1`, run path_decomp, compare against `docs/path_decomp_iter11A_20260511_023247/` to identify the new dominant stage post-bimodal-fix. Output target: `docs/path_decomp_iter12A_<ts>/`.

Implementation note: use `scripts/iter11A_5wl_pathdecomp.sh` template, adapt to iter-12A's new-best cells from Phase 4 sweep.

### 2. Parallel inval drain redesign (iter-11A backlog #8 carry-over)

iter-11A Phase 2 shipped + reverted parallel inval (w_p99 26× regression). Canonical redesign: bucket-affinity sharded queues (each InvalWorker drains a fixed set of buckets, no cross-shard contention). Still a relevant optimization for write-heavy workloads (a, b, d, f).

### 3. Hot-key replication (iter-11A backlog #6 carry-over)

For Zipf workloads (a, d, f), the most-accessed keys dominate cross-host invalidate traffic. Replicating the hot top-K keys at each host (with TTL-based eventual consistency or §I9-compatible epoch ordering) would remove the cross-host bottleneck for hot keys without breaking strict-A semantics.

---

## Open from iter-12A

### 4. ~~Parent-CPU-0 jitter mitigation (escalation past QR2 L1)~~ **CLOSED — V1 falsified, true cause was V3 cache-stale, fixed in iter-12A Phase 5**

V1 falsified by P5R_PL heartbeat probe (g4 WriteReceiver polled continuously
on CPU 65 for 118 s with no preempt gaps). True cause was V3: stale dirty
`ring->head` in host 1's L1/L2/L3 after `init=false` reattach. Fix landed
2026-05-17 (commit pending). No CPU-isolation work needed.

### 4b (NEW). Bug B: 2/102 worker timeouts in cell C1 where receiver did see+ack

Phase 5.1 probe data on cell C1 (workloada T=64 off kv=1024) post-Bug-A-fix
shows 2 of 102 worker writes still timed out at 5 ms despite the receiver
emitting `P5R_VS` + `P5R_AK` for those op_ids. Mechanism is **separate from
Bug A** and likely worker-side `e->resp_op_id` visibility within the 5 ms
budget — receiver wrote+flushed, but worker's spin_wait didn't observe the
write before its budget expired.

Plan for iter-13A:
1. Add `P5W_ACK_OBSERVED` probe in `generic_spin_wait` (TSC when worker
   first reads `resp_op_id == op_id`).
2. Repro on cell C1 with extended FUSEE_PROBE=1 build.
3. Measure ack-propagation delay distribution; if p99 exceeds 5 ms,
   options: (a) extend `kBudgetUs`, (b) add explicit `flush_line(&e->resp_op_id)`
   in worker spin loop, (c) move resp_op_id to a separate cacheline from
   worker-written fields to reduce ping-pong.
4. Effort: ~2-4 h focused diagnostic + targeted fix.

Impact: not bimodal-class (affects ~2 % of writes max), but counts as a
correctness/QoS issue (timeouts surface as -11 returns to caller).

### 5. kBudgetUs / receiver gap-budget Pareto sweep

Phase 1.6 picked `kBudgetUs=5000` (5 ms) + 4096-iter receiver budget (~2.8 ms) empirically. A small parameter sweep along (timeout, budget) might find a Pareto-better point. Low priority — gate-12 already passes.

---

## Held items from iter-11A backlog (still applicable)

### 6. RCU cache_pool (iter-11A backlog #7 carry-over)

W10 stage (write to local cache pool) wasn't dominant in iter-11A Phase 5 path_decomp; iter-12A Phase 3 (deferred) would re-check whether that's still true post-bimodal-fix. Conditional on data from item 1.

### 7. Probe-overhead attribution (iter-11A backlog #6 carry-over)

Quantify how much of the FUSEE_PROBE=1 vs FUSEE_PROBE=0 throughput delta is "actually measured per-stage cost" vs "instrumentation overhead". Useful for sweet-spot configuration.

---

## Constraints carry-over

- **C16 observe-first** + **C17 narrow-targeted fix** + **C18 commit-level bisect** are now CLAUDE.md cautionary precedent #4 + iter-12A spec entries. Apply to all subsequent iters.
- §I9 strict-A linearizability — non-negotiable; every protocol change requires G1 hash-diff PASS.
- 20 Mops/s target — still binding on workload-a AND workload-c per `docs/design_goals.md`.

---

## What iter-13A should NOT do

- **No new architecture absent data** — Phase 1.7 + Phase 2 evidence from iter-12A removed the easy "hot fix" surface area. New optimization must be measured-then-justified, not hypothesized.
- **No "fix N bugs at once"** — C17 narrow-targeted is now permanent. Each iter ships ≤ 3 file × function changes per problem.
- **No "verification cell" measurements without per-cell 5-rep reproduction** — iter-11A Phase 1 lesson (1.4 µs claim unreproducible). Headline numbers must be 5-rep medians.

---

## Suggested iter-13A first task

**Phase 3 path_decomp on iter-12A new-best cells, then decide between (2) parallel inval or (3) hot-key replication based on which stage dominates the post-fix bottleneck**. Both are big architectural changes; let the data pick.
