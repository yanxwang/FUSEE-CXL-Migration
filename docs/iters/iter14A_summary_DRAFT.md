# iter-14A Summary (DRAFT — filled live as phases complete)

**Date**: 2026-05-19
**Branch**: `feat/cxl-migration`
**Predecessor**: `docs/iters/iter13A_summary_20260517.md`
**Plan**: `docs/iters/task_plan_iter14A.md`
**Progress doc**: `docs/iters/iter14A_progress.md`
**Fix register**: `docs/iters/iter14A_fix_register.md`

---

## TL;DR

iter-14A executed an **observe + fix** loop on top of iter-13A's as-shipped
HAZARD+W1 build, with two prospective fixes (F1 cross-host write
self-inval, F2 cache_pool_lookup LRU sampling) and a ground-truth
attribution study comparing STAGING vs HAZARD+W1 at the memory-bandwidth
level. Headline findings:

1. **Both fixes ROLLBACK** under the universal fix policy (CI lower
   bound did not exceed +1%). Flags preserved in tree, defaults OFF.
   - F1 regressed workloada T=4 -13% (sharer_bitmap reset semantics
     meant most cross-host writes have no invalidates to save → no
     roundtrip to save).
   - F2 showed all 5 cells within ±2.5% point estimate, CIs cross 0.
     R2hit anomaly is real but not throughput-load-bearing.

2. **Copy elimination (iter-13A deliverable) works at the data-movement
   level but not at the throughput level in slow mode**. Bytes/op
   dropped -28.9% in HAZARD+W1 vs STAGING (median, across both modes).
   But in mode-matched comparison (slow vs slow), B/op is identical
   (~120 B/op) and thpt is identical (10.7 Mops/s). The copy savings
   show up only when the system enters bimodal-fast mode (17.5 Mops/s).

3. **Bimodal collapse is now the throughput-loss mechanism**, not
   data-copy overhead. iter-12A's bimodal "fix" did not eliminate the
   slow mode; P3.C confirmed it still exists; P5 quantified the cost.
   iter-15A top-priority item.

4. **W10 anomaly identified as structural** — cache_pool_insert MESI
   ping-pong on 1088B KvCacheEntry, T-invariant, 3.5× spec. No
   small-LOC verified fix; iter-15A backlog.

5. **P8 TLS research** (no code ship) documented the 6-assumption chain
   underlying the 2-tier cache design (TLS + shared cache_pool) and
   identified 3 unmeasured assumptions (A2 hit-rate, A4 bucket-epoch
   pingpong, A5 eviction-rate). Proposed 7 experiments (E1-E7), of which
   E2/E4/E6 are research-scope iter-15A candidates.

---

## Phase delivery audit (per CLAUDE.md gate)

| Sub-phase | Plan | Delivered | Status |
|---|---|---|---|
| P1 | rekey + baseline + smoke + hash-diff + rw_race fix | smoke 1.42 Mops/s; hash-diff 20/20 PASS; rw_race signature fix (commit 258f530) | ✅ FULL |
| P2 | cross-host write self-inval ship-fix + RAP + measurement | RAP authored; impl + hash-diff 20/20 PASS; G6 pre-existing-broken caught + documented; measurement: F1 ROLLBACK | ✅ FULL |
| P3 | preflight: traces + history replot + probe overhead + stage spec | 32 trace files; 28-tag stage spec; P3.C probe overhead = bimodal-confounded; P3.B history replot deferred (P5 surfaced R1 directly) | ✅ FULL (P3.B deferral justified by P5 substituting the result) |
| P4 | production path_decomp + iterative fix loop | path_decomp at 4 cells; W10 + R2hit anomalies identified; F2 attempt + ROLLBACK (after catching null-compare in first attempt) | ✅ FULL |
| P5 | copy elim attribution case A/B/C/D | Median-comparison case C; mode-matched case B; quantitative B/op tables; bimodal segmentation analysis | ✅ FULL |
| P6 | 144-cell ground truth microbench | TBD (in progress) | ⏳ |
| P7 | 210-cell full YCSB sweep on as-shipped | TBD (queued after P6) | ⏳ |
| P8 | TLS research (no code ship) | tls_evolution_review.md with 6 assumption chain + 7 experiments | ✅ FULL |
| P9 | this summary + iter-15A backlog | (this doc) | ⏳ |

## Numbers TBD (P6, P7)

(filled after sweeps complete)

### P6 microbench headline

| Scenario | T=1 | T=4 | T=32 | T=64 |
|---|---:|---:|---:|---:|
| local_read uniform | TBD | TBD | TBD | TBD |
| local_read zipf | TBD | TBD | TBD | TBD |
| xhost_read uniform | TBD | TBD | TBD | TBD |
| xhost_read zipf | TBD | TBD | TBD | TBD |
| local_write uniform | TBD | TBD | TBD | TBD |
| local_write zipf | TBD | TBD | TBD | TBD |
| xhost_write uniform | TBD | TBD | TBD | TBD |
| xhost_write zipf | TBD | TBD | TBD | TBD |

### P7 sweep peak Mops/s

| Workload | iter-13A as-shipped | iter-14A as-shipped | delta |
|---|---:|---:|---:|
| workloada | 11.12 | TBD | TBD |
| workloadb | 19.14 | TBD | TBD |
| workloadc | 18.79 | TBD | TBD |
| workloadd | 18.20 | TBD | TBD |
| workloadf | 16.89 | TBD | TBD |

## Iter-completion gates (§13)

| Gate | Status |
|---|---|
| §13 gate 5 anomaly scan (P7 sweep) | TBD |
| G1 hash-diff | ✅ 20/20 PASS (P1) + 20/20 PASS (P2 build) |
| Universal fix policy | ✅ F1 ROLLBACK + F2 ROLLBACK per spec |
| Delivery audit | TBD (after P6/P7) |
| Spec drift audit (P3 trigger) | ✅ stage spec aligned with implementation; 28 PROBE_OP tags categorized |

## iter-15A backlog (top-priority preview, full doc → iter14A_backlog_memo.md)

1. **Bimodal RCA + escape mechanism** (top priority — P5 evidence).
2. **W10 structural fix**: cache_pool_insert MESI ping-pong on 1088B
   KvCacheEntry. Candidate fixes: separate metadata/value cachelines,
   true lock-free hashmap, async write-behind. P4 evidence: T-invariant,
   3.5× spec.
3. **TLS counter dump** (P8 E2): 5-LOC research patch; informs A2/A5.
4. **Cache-pool ablation matrix** (P8 E4): B0/B1/B2/B3 builds; isolates
   TLS vs shared cache_pool contribution.
5. **F1 re-design** with sharer_bitmap retention (RAP follow-up):
   sharer bitmap currently reset to {owner} per write; with retention
   the F1 self-invalidate would save real roundtrips.

## Code changes

- `src/cxl_read_guard.h`: added FUSEE_XHOST_WRITE_SELF_INVAL,
  FUSEE_LRU_SAMPLE flags (defaults 0). Code preserved for iter-15A.
- `src/cxl_cache_pool.cc`: LRU sampling under FUSEE_LRU_SAMPLE.
- `src/cxl_kv_ops_A.{h,cc}`: self_inval_src parameter on execute_write_local
  and execute_write_local_with_blk; receiver excludes src when set.
  Used only when FUSEE_XHOST_WRITE_SELF_INVAL=1.
- `tests/protocol_a_rw_race_test.cc`: enable_read_ring signature sync.

## Doc changes (artifact catalogue)

- `docs/iters/task_plan_iter14A.md` — full plan
- `docs/iters/iter14A_progress.md` — live progress
- `docs/iters/iter14A_fix_register.md` — fix attempts + decisions
- `docs/iters/iter14A_p2_rap.md` — F1 RAP
- `docs/iters/iter14A_f2_lru_sample_rap.md` — F2 RAP
- `docs/iter14A_p3d_pathdecomp_spec.md` — 28 stage spec
- `docs/iter14A_p2_xhost_write_self_inval_*/decision.md` — F1 ROLLBACK
- `docs/iter14A_p4_production_pathdecomp_*/per_stage_decomp.md` — P4
- `docs/iter14A_p4_f2_lru_sample_*/decision.md` — F2 first attempt (null) + real
- `docs/iter14A_p5_attribution_*/conclusion.md` — case framework deliverable
- `docs/iter14A_p8_tls_research/tls_evolution_review.md` — TLS research
- `docs/iter14A_p6_microbench_*/MICROBENCH.tsv` — 144-cell raw + analyzed
- `docs/iter14A_p7_full_sweep_*/SUMMARY.log` — 210-cell raw
- `docs/iters/iter14A_summary_20260519.md` — (this doc, finalized after P7)
- `docs/iters/iter14A_backlog_memo.md` — iter-15A handoff
