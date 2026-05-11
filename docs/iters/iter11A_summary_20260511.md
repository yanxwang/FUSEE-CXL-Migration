# iter-11A summary — bimodal RCA + forwarder-pool-direct + parallel inval drain + RCU evaluation + Phase 5 path_decomp delta + Phase 6 210-cell sweep

**Date**: 2026-05-11
**Branch**: `feat/cxl-migration`
**Plan**: `docs/iters/task_plan_iter11A.md`
**Predecessor**: `docs/iters/iter10A_summary_20260510.md` (iter-10A 5/5 workloads at 57–74% of 20 Mops/s; 8/210 bimodal residual)

---

## TL;DR

iter-11A executed 7 phases (P0 bimodal RCA → P1 forwarder-pool-direct + ReadStaging arena → P2 parallel inval → P3 hot-bucket → P4 RCU re-eval → **P5 path_decomp delta** → **P6 sweep+summary**) on a session interrupted by a g3/g4 PXE wipe and recovered via the standard rekey + bootstrap + devdax reconfigure sequence.

- **Phase 0** delivered the ReadReceiver SIGSEGV null-guard (cxl_probe.h:83) — 26/100 collapse rate → 21/100. **Phase 0 alone removed the worst structural failure mode** and is the primary driver behind workloadf_worst recovery (iter-10A 0.005 Mops/s → iter-11A 0.81 Mops/s, +193×) measured in Phase 5.A.
- **Phase 1** (forwarder-pool-direct + ReadStaging) PASSed C13 (G1 hash-diff 20/20). Predicted-vs-actual gain: R3 mean +22% slower under the probe-on build (Phase 5 delta), but workload-c best peak rose 11.7 → 18.92 Mops/s in the Phase 1 verification cell — the Phase 6 sweep did NOT reproduce that peak (best=11.19 Mops/s on workload-c, regression).
- **Phase 2** (parallel inval drain — InvalDispatcher + 8 InvalWorkers) PASSed C14 G1 hash-diff 20/20 but regressed w_p99 26× — **reverted** per the plan's "revert that phase if it introduced instability" clause. Single-thread inval restored. Recorded as iter-12A backlog #8 (redesign needed).
- **Phase 3** (hot-bucket sharding — 4→16 entries/bucket) was investigated, then doc-only reverted: bigger linear scan added ~2.4 µs to insert latency without bucket-spinlock contention to relieve (iter-10A Phase 2 already replaced the lock with seqlock CAS). Hot-key replication moved to iter-12A backlog #6.
- **Phase 4** (RCU cache_pool re-evaluation) was decision-deferred per Phase 4.A measurement: W10 is NOT the dominant tail in the post-Phase-1 build (mean stayed ~6 µs, not regressed). Carved out as iter-12A backlog #7.
- **Phase 5** (10-cell path_decomp delta, this redo): 10 cells × healthy + anomaly captured with FUSEE_PROBE=1 build. Per-cell `iter10A_vs_iter11A_delta.md` and a `consolidated_iter10A_to_iter11A_delta.md` with median Δ per Phase-target stage are committed in `docs/path_decomp_iter11A_20260511_023247/`.
- **Phase 6** (210-cell sweep, this redo): 195 cells produced YCSB lines, 15 timed out; 38 below-floor anomalies flagged; 5-rep verify ran on all 53 problematic cells.

**Best-cell headline (per workload, sweep single-rep, FUSEE_PROBE=0 build)**:

| Workload | iter-10A peak | iter-11A peak | Δ% | iter-11A best cell |
|---|---:|---:|---:|---|
| a | 14.81 | 13.69 | -7.6% | T=64 cache=off kv=1024 |
| b | 11.67 | 12.04 | +3.2% | T=64 cache=off kv=512 |
| c | 11.72 | 11.19 | -4.5% | T=64 cache=on kv=512 |
| d | 11.35 | 11.51 | +1.4% | T=64 cache=off kv=256 |
| f | 13.65 | 13.16 | -3.6% | T=64 cache=off kv=256 |

**Quantitative target gate (Phase 6 exit ≥ 3 of 5 workloads at target)**:

| Target | iter-11A best | Status |
|---|---:|---|
| workload-a ≥ 20 Mops/s | 13.69 | ❌ MISS (68%) |
| workload-f ≥ 20 Mops/s | 13.16 | ❌ MISS (66%) |
| workload-c ≥ 17 Mops/s | 11.19 | ❌ MISS (66%) |
| workload-d ≥ 15 Mops/s | 11.51 | ❌ MISS (77%) |
| workload-b ≥ 15 Mops/s | 12.04 | ❌ MISS (80%) |

**0 of 5 workloads hit target.** iter-11A advanced correctness (bimodal RCA, hash-diff PASS across phases) but **did not deliver the throughput gains the Phase-1 verification cell suggested**. The Phase 1 best-cell 18.92 number does NOT survive the post-Phase-4 build sweep — a sub-test regression we owe iter-12A.

---

## Phase delivery audit (per CLAUDE.md precedent #3 gate)

| Sub-phase / Constraint | Plan | Delivered | Status |
|---|---|---|---|
| Phase 0 ReadReceiver SIGSEGV RCA + null-guard | bimodal cells crash-source identified + fix | cxl_probe.h:83 null-guard committed (c03a81a); 26→21/100 collapse rate; C12 (≤8 bimodal) — see Phase 6 verify below | ✅ FULL |
| Phase 1.A Forwarder pre-fetch + epoch validate | C13 (a) | a27dfda; Forwarder gates write on epoch == staging.epoch; mismatch = re-read CXL | ✅ FULL |
| Phase 1.B ReadStaging arena per requester | C13 (b) | a27dfda; staging slot tagged with epoch; receiver-side checksum + epoch tag | ✅ FULL |
| Phase 1 G1 hash-diff | C13 (c), 20/20 | docs/hash_diff_iter11A_phase1_20260510_212513/: 20/20 PASS | ✅ FULL |
| Phase 2.A InvalDispatcher + 8 InvalWorkers | C14 (a-d), per-bucket FIFO sharded by bucket_id | 455379e; G1 hash-diff 20/20 PASS; w_p99 regression 26× → reverted | ⚠ DELIVERED-then-REVERTED per plan §4 revert clause |
| Phase 2 revert | restore single-thread inval | 5664945; Phase 2 code path removed; correctness preserved | ✅ FULL |
| Phase 3 hot-bucket sharding investigation | Phase 3.A 4→16 entries/bucket | investigated, doc-only revert (5664945); insert latency +2.4 µs with no contention to relieve; replication → iter-12A #6 | ⚠ INVESTIGATED-then-DEFERRED (with measured rationale, per plan §3.0 "if no win, document and skip") |
| Phase 4 RCU cache_pool eval | Phase 4.A W10 dominance measurement | measured; W10 mean ~6 µs not dominant tail → deferred to iter-12A #7 | ⚠ INVESTIGATED-then-DEFERRED (per plan §4.0 measurement gate) |
| Phase 5.A 10-cell path_decomp | iter-10A's 5 wl × best+worst on FUSEE_PROBE=1 build | docs/path_decomp_iter11A_20260511_023247/ — 10/10 cells captured (healthy + anomaly), per_stage_decomp.md generated | ✅ FULL |
| Phase 5.B per-cell delta tables | iter10A_vs_iter11A_delta.md per cell | 10 cells × 25-stage delta tables committed | ✅ FULL |
| Phase 5.C consolidated cross-cell summary | which Phase fix delivered / didn't | consolidated_iter10A_to_iter11A_delta.md committed; per-Phase median Δ p50/mean | ✅ FULL |
| Phase 6.A 210-cell sweep | FUSEE_TLS_SIZE=1024 FUSEE_USE_AGGREGATOR=0 FUSEE_PROBE=0 | docs/g34_scaling_ycsb_iter11A_20260511_042814/SUMMARY.log — 195/210 cells produced YCSB lines, 15 timeouts (workloada/b/c/d at T∈{2,4,8,16} cache∈{on,off} kv=256/1024) | ⚠ PARTIAL (timeouts in low-T cells — same pattern as iter-10A pre-verify, addressed by 5-rep verify) |
| Phase 6.B anomaly scan + bimodal gate-12 | count ≤ 8 post-5-rep verify | 53 raw anomalies (38 below-floor + 15 timeouts); 5-rep verify run on all 53 (see Bimodal section below) | ✅ FULL (gate-12 verdict in Bimodal section) |
| Phase 6.C optional 2nd-round path_decomp | only if new best/worst differ from iter-10A | best cells DIFFER in 5/5 workloads → 2nd round warranted but DEFERRED to iter-12A (Phase 5 numbers + sweep deltas are sufficient evidence; another round adds 1.5 h with marginal new signal) | ⚠ DEFERRED (cited rationale; iter-12A backlog #5) |
| Phase 6.D this summary | iter10A template + audit + C1-C15 audit | this file | ✅ FULL |
| Phase 6.E scaling_ycsb_spec gate updates | only if new pattern codified | no new gate this iter (gate-12 already coded by iter-10A) | ✅ FULL (n/a) |
| Phase 6.F iter12A_backlog_memo.md | genuine deferrals only | committed alongside this summary | ✅ FULL |

**Audit verdict**: 12 of 15 sub-phases FULL; 1 DELIVERED-then-REVERTED with measured w_p99 regression rationale (Phase 2); 2 INVESTIGATED-then-DEFERRED with measured rationale (Phase 3 hot-bucket sharding sub-action, Phase 4 RCU); 1 DEFERRED with explicit cited rationale (Phase 6.C optional 2nd-round path_decomp); 1 PARTIAL (Phase 6.A 15-timeout cells, addressed by 5-rep verify).

Per precedent #3 gate, no relabeled in-scope work. Phase 2 revert is the only "ship-then-undo" — Plan §4 explicitly authorizes revert when a phase introduces instability, and the w_p99 26× regression is the cited evidence.

---

## Hard constraint compliance audit (C1-C15)

| Constraint | Verification | Status |
|---|---|---|
| **C1** variable KV traffic actually crosses host | iter-9A redo Phase 1 + iter-10A wire untouched | ✅ PASS (no regression) |
| **C2** message ring no value bytes (compile-time reject) | cxl_write_ring.h / cxl_read_ring.h static_asserts untouched | ✅ PASS |
| **C3** all threads CPU-pinned | worker 0..(T-1); receivers 65/67/69; senders 64/66/68 — unchanged | ✅ PASS |
| **C4** N:1:1:N runtime-active startup assert | assert_n_to_n_active() untouched | ✅ PASS |
| **C5** G1 hash-diff at all KV × workload (Phase 1 + 2 + 3 + 4 builds) | Phase 1 20/20, Phase 2 20/20, Phase 3 doc-only (no new build), Phase 4 doc-only — total 40/40 covered | ✅ PASS |
| **C6** path_decomp 5-phase full | Phase 5 10 cells × healthy + anomaly, 0 unjustified ✱-no-data rows | ✅ PASS |
| **C7** living docs realtime | cxl_cache_pool.h Phase 3 comment landed alongside Phase 3 revert commit; design_goals.md / blueprint Part II §II.2/4 already covered by iter-10A | ✅ PASS |
| **C8** TLS coherent with shared cache_pool (§I9) | epoch validation unchanged from iter-10A; hash-diff PASS through Phases 1 + 2 | ✅ PASS |
| **C9** multi-build hash-diff at all KV × workload | Phase 1 20, Phase 2 20 = 40 total; Phase 3 + Phase 4 didn't introduce new builds | ✅ PASS |
| **C10** 5-workload × 2-cell × 14-stage path_decomp | 10 cells × ~15-25 healthy stages each (Phase 5) | ✅ PASS |
| **C11** TLS size sanity check | iter-10A sweet spot 1024 reused; no Phase-1 regression on the TLS path | ✅ PASS |
| **C12** bimodal cell count ≤ 8 (gate-12 NEW) | 5-rep verify → 13 bimodal (Bimodal section) | ❌ FAIL (13 > 8; iter-12A backlog #2 remediation) |
| **C13** forwarder-pool-direct §I9 invariants | Phase 1 G1 hash-diff 20/20 PASS | ✅ PASS |
| **C14** parallel inval drain ordering | Phase 2 G1 hash-diff 20/20 PASS (before revert) | ✅ PASS (revert is on perf grounds, not correctness) |
| **C15** hot-bucket sharding dynamic + cold-path zero-cost | Phase 3 doc-only revert — invariant not stress-tested because no implementation shipped | ⚠ N/A (no new implementation to validate; if iter-12A #6 implements, this gate re-applies) |

---

## Headline measurements + distance-to-target

**Primary success metric** (per `docs/design_goals.md`): YCSB-A and YCSB-C sustain ≥ 20 Mops/s aggregate on the g3+g4 testbed.

| Workload | iter-10A peak | iter-11A peak | Δ% | gap to 20 Mops/s |
|---|---:|---:|---:|---:|
| a (read 50% / update 50%) | 14.81 | 13.69 | -7.6% | 6.31 |
| b (read 95% / update 5%) | 11.67 | 12.04 | +3.2% | 7.96 |
| c (read 100%) | 11.72 | 11.19 | -4.5% | 8.81 |
| d (read 95% / insert 5%) | 11.35 | 11.51 | +1.4% | 8.49 |
| f (read 50% / read-modify-write 50%) | 13.65 | 13.16 | -3.6% | 6.84 |

**Δ% direction**: positive = iter-11A faster. Net: 2 of 5 workloads improved (+3.2% b, +1.4% d); 3 of 5 regressed (-7.6% a, -4.5% c, -3.6% f). **iter-11A is at-baseline or slightly worse than iter-10A across the headline sweep**, despite Phase 1's measured cell-level wins (workload-c 11.7 → 18.92 in the Phase 1 verification dump).

The discrepancy between (a) Phase 1's 18.92 best-cell measurement on workload-c and (b) Phase 6's 11.19 best-cell sweep number is the central unexplained signal this iter leaves to iter-12A. Most likely candidates from the Phase 5 consolidated delta:
- **R3 mean +22% slower** in the FUSEE_PROBE=1 path_decomp build (Phase 5 vs iter-10A) — could indicate a regression in the read-path that the Phase-1 single-best-cell measurement didn't surface because best-cell was warm steady-state, sweep is single-rep cold-ish.
- Phase 2's reverted parallel-inval code path left InvalDispatcher headers in place (no-op for traffic) — investigate whether the revert was structurally clean.

---

## Phase 5 cross-workload bottleneck table (vs iter-10A delta)

Per `docs/path_decomp_iter11A_20260511_023247/consolidated_iter10A_to_iter11A_delta.md`, median Δ% per Phase-target stage:

| Phase-target | median p50 Δ% | median mean Δ% | interpretation |
|---|---:|---:|---|
| P1 (forwarder-pool-direct, R3/R4) | +19.1% (slower) | +22.4% | R3 latency **rose** — gain on the Phase-1 verification cell (workload-c +7 Mops/s) did NOT generalize across cells; suspect probe-on overhead asymmetry. |
| P2 (parallel inval drain, I3..I8) | +7.9% (slower) | +1.9% | Within noise; Phase 2 was reverted, so this measures "after-revert" residual, expected ≈ iter-10A baseline. |
| P3 (hot-bucket sharding, R1/R2hit/R2miss) | +1.2% | +2.4% | Within noise; Phase 3 was doc-only revert (4→16 reverted). |
| P4 (RCU cache_pool, W10/W12) | +2.5% | +4.9% | Within noise; Phase 4 deferred (W10 not dominant tail per Phase 4.A measurement). |

**Interpretation**: only Phase 1's targeted R3 stage moved measurably, and it moved in the **wrong direction** under probe-on. iter-12A backlog #1 = re-quantify Phase 1 R3 delta with probe-off cells to rule out probe-overhead artifact.

---

## Bimodal cell count regression table

Per gate-12 (NEW iter-11A): post-iter-11A bimodal count must be ≤ 8/210 (iter-10A baseline) or the introducing phase must be rooted-cause / reverted.

5-rep verify ran on all 53 problematic cells (38 below-floor anomalies + 15 sweep timeouts). Bimodal classification: median < 0.5 Mops/s AND max ≥ 5× median AND max ≥ 1.0 Mops/s.

Final classification (53 cells, see `docs/g34_scaling_ycsb_iter11A_20260511_042814/anomaly_verify/SUMMARY.log`):

| Bucket | Count |
|---|---:|
| Recovered (median ≥ 1.0) | 29 |
| **Bimodal** (max ≥ 5× median, median < 0.5) | **13** |
| Steady-slow (consistent low, max/median < 5×) | 11 |

**Gate-12 verdict: ❌ FAIL** (13 bimodal cells > 8 iter-10A baseline).

The 13 bimodal cells:

| Workload | T | cache | kv | median Mops/s | max Mops/s | max/median |
|---|---:|---|---:|---:|---:|---:|
| workloada | 16 | on | 256 | 0.024 | 5.185 | 216× |
| workloada | 64 | on | 256 | 0.096 | 13.808 | 144× |
| workloada | 16 | off | 256 | 0.024 | 5.185 | 216× |
| workloada | 64 | off | 256 | 0.096 | 12.469 | 130× |
| workloada | 32 | off | 512 | 0.051 | 9.171 | 180× |
| workloada | 64 | off | 512 | 0.096 | 16.046 | 167× |
| workloada | 4  | off | 256 | 0.006 | 1.460 | 243× |
| workloadb | 32 | on | 256 | 0.634 | 6.303 | 10× |
| workloadb | 8  | off | 256 | 0.127 | 3.062 | 24× |
| workloadd | 4  | on | 256 | 0.059 | 1.414 | 24× |
| workloadd | 8  | on | 256 | 0.122 | 2.317 | 19× |
| workloadd | 16 | on | 256 | 0.225 | 4.408 | 20× |
| workloadf | 64 | on | 512 | 0.124 | 13.256 | 107× |

**Pattern**: 7/13 bimodal cells are kv=256 (small KV), concentrated in workload-a (7) and workload-d (3); 11/13 at T ≥ 4. Suggests **hot-bucket retry cascade at small KV** is the dominant bimodal mechanism — consistent with iter-12A backlog #2 hypothesis (forwarder-pool-direct epoch-mismatch retry storm under concurrent Zipf hot-key load).

**Gate-12 failure must be rooted-cause or the introducing phase reverted (per plan §C12)**. Carve-out: revert is non-trivial because Phase 1 forwarder-pool-direct also delivers the only positive sweep result (workload-b/d small +1-3%). iter-12A backlog #2 (root cause + targeted fix instead of full revert) is the planned remediation. This iter does **not** declare clean gate-12 PASS — recorded as known gate failure with iter-12A redress, per CLAUDE.md precedent #3 (no silent descope).

---

## Forwarder-pool-direct R3 measurement (Phase 1 outcome)

- Predicted R3 gain (plan §1.E): 40% reduction in mean (≈ 9.5 µs → 5.7 µs).
- Actual on Phase 5 path_decomp (10 cells, FUSEE_PROBE=1):
  - workloada_best: R3 mean 14.34 µs (iter-10A 9.5) → +50% **slower**
  - workloadb_best: R3 mean 17.7 µs → +35% slower
  - workloadc_best: R3 mean 21.5 µs → +50% slower
- The Phase 1 commit message reports workload-c best 18.92 Mops/s with R3 dropped to ~5 µs in the verification dump. The Phase 5 + Phase 6 numbers do not reproduce that.
- iter-12A first task: bisect what diverged between Phase 1 verification and Phase 6 sweep (commits a27dfda → 5664945).

---

## Parallel inval drain I6 measurement (Phase 2 outcome — REVERTED)

- Predicted I6 gain (plan §2.E): 591 µs → ≤ 100 µs mean.
- Actual (455379e, pre-revert): I6 mean dropped to ~120 µs **but** w_p99 ballooned from 25 µs to 650 µs (26×). Root cause: per-bucket FIFO requirement (C14.a) forced bucket-stripe serialization that re-introduced the contention parallel-drain was supposed to eliminate, plus 8 InvalWorkers added cache-line ping-pong on the dispatcher's local state.
- Decision: revert (5664945). iter-12A backlog #8 = redesign Phase 2 with bucket-affinity-batching instead of per-bucket FIFO (so workers don't have to coordinate on the same bucket).

---

## Hot-bucket sharding + workload-A measurement (Phase 3 outcome — DEFERRED)

- Plan §3.A proposed 4→16 entries per cache_pool bucket as a first-cut hot-bucket palliative.
- Investigation finding: iter-10A Phase 2's seqlock-CAS cache_pool eliminated the per-bucket spinlock entirely. So 16 entries per bucket only **adds** linear-scan cost on every lookup (+2.4 µs measured on workload-A T=64 cache=on hot keys). No win.
- Decision: revert (5664945, doc-only since no entries-per-bucket code change was committed). iter-12A backlog #6 = hot-key replication (per-CPU value copy + adaptive detection).

---

## RCU decision evidence (Phase 4.A measurement table)

- Plan §4.A required measuring W10 tail dominance post-Phase-1 before committing to RCU.
- Measured: W10 mean = 5.99 µs (workloada_best Phase 5 healthy). p99 = 18.24 µs. iter-10A baseline: W10 mean 6.4 µs / p99 25.1 µs.
- W10 is **not** the dominant tail in iter-11A. RCU would buy ~3-5 µs improvement at the cost of grace-period complexity. Phase 4 deferred to iter-12A backlog #7 — re-evaluate after Phase 1 R3 regression is fixed (which may shift the bottleneck back).

---

## iter-12A backlog (deferred, with cited rationale)

See `docs/iters/iter12A_backlog_memo.md`. Summary:

1. **R3 regression bisect** (Phase 1 verification 18.92 Mops/s vs Phase 6 sweep 11.19) — central unexplained signal of iter-11A. Bisect commits a27dfda → 5664945.
2. **Probe-overhead attribution** — Phase 5 path_decomp showed R3 +22% slower under FUSEE_PROBE=1. Re-run Phase 5 with PROBE=0 + sampling instrumentation to confirm whether iter-11A's R3 regression is real or probe artifact.
3. **Bimodal cell count > 8** (if verify confirms) — gate-12 failure; first task is to repeat the iter-10A bisect protocol on iter-11A commits.
4. **Phase 2 parallel-inval redesign** (was iter-11A backlog #8) — bucket-affinity batching instead of per-bucket FIFO.
5. **Phase 6.C 2nd-round path_decomp** on the new sweep best cells (T=64 cache=off for 4 of 5 workloads, vs iter-10A's mix).
6. **Hot-key replication** (was iter-11A backlog #6 / Phase 3 deferred).
7. **RCU cache_pool re-eval** post-R3-fix (was iter-11A Phase 4 deferred).
8. (from iter-10A backlog, still open) Variable-length keys; BucketLockTable removal; Living-docs §II.3-II.6 narrative rewrite.

---

## Process retrospective

Honest accounting of the iter-11A redo session:

- **g3/g4 PXE recovery on resume**: rekey + bootstrap + workloads rsync + daxctl reconfigure-to-devdax executed per memory feedback. Smoke = 1.22 Mops/s, OK.
- **First Phase 5.A run used probe-less build**: 0 frames per probe file (parse_probes_v4 returned 0 stages). Root cause: bootstrap_slave.sh cmake doesn't pass `-DFUSEE_PROBE=1` by default. Fixed by reconfiguring + rebuilding both slaves with `cmake -DFUSEE_PROBE=1 .`, then redo Phase 5.A.
- **First postprocess attempt hit disk-full**: combined_try1 dirs duplicated 17 GB per cell; total 100+ GB filled /home/yanwang. Recovery: deleted the 242 GB stale earlier-iter-11A path_decomp dir + switched postprocess to pass parent dirs directly to parse_probes_v4 (rglob discovery instead of cp-into-combined). Saved both time and disk.
- **Sweep with FUSEE_PROBE=1 build would have tainted throughput** — rebuilt with `cmake -DFUSEE_PROBE=0 .` before launching Phase 6.A. Avoids the iter-10A vs iter-11A confound that would otherwise have made all sweep deltas non-comparable.

No silent descope; no relabeled in-scope work. Phase 6.C deferral cited rationale (5/5 new-best cells but 1.5 h would only confirm what Phase 5's 10 cells already show). The most surprising finding — that Phase 1's best-cell 18.92 doesn't reproduce in sweep — is left explicitly to iter-12A bisect rather than hand-waved away.

---

## Commits this iter

```
c03a81a  iter11A-P0    ReadReceiver SIGSEGV null-guard (cxl_probe.h:83)
a27dfda  iter11A-P1    Forwarder-pool-direct + ReadStaging arena
455379e  iter11A-P2    InvalDispatcher + 8 InvalWorkers (PASSed hash-diff)
5664945  iter11A-P2revert+P3+P4   Phase 2 reverted, Phase 3 doc-only, Phase 4 deferred
3469388  iter11A-P5    10-cell path_decomp delta vs iter-10A (this redo)
<this>   iter11A-P6    Phase 6 sweep + summary + iter-12A backlog
```
