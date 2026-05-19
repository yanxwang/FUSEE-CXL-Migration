# iter-14A Progress (live-updated)

**Started**: 2026-05-18
**Branch**: `feat/cxl-migration`
**Plan**: [task_plan_iter14A.md](task_plan_iter14A.md)
**Fix register**: [iter14A_fix_register.md](iter14A_fix_register.md)

---

## Status

| Phase | State | Notes |
|---|---|---|
| **P1** Minimum preflight | ✅ done | rekey g3/g4 + baseline build + smoke 1.42 Mops/s + hash-diff 20/20 PASS + rw_race_test sig fix (commit 258f530) |
| **P2** xhost write self-inval | ✅ done (F1 ROLLBACK) | RAP + impl + hash-diff PASS + measurement: target T=4 regressed -13%, T=32/64 flat → flag default OFF. Code kept in tree. iter-15A revisit. |
| **P3** Remaining preflight | ✅ done | microbench traces 32 files (2M load + 1M trans-per-host) 1.3GB rsync'd to g3/g4; build-cxl-w1-probe; stage spec written; P3.C probe overhead bimodal-confounded (see p3c_summary.md); P3.B historical replot deferred (P5 will surface the R1 question directly) |
| **P4** Production path_decomp + fix | ✅ done (F2 ROLLBACK) | path_decomp at 4 cells: W10=3.54µs (3.5× spec) SOFT-ANOMALY (cache_pool_insert MESI on 1088B entry) → iter-15A; R2hit=0.23µs (7.7× spec) MILD-ANOMALY → F2 LRU sampling attempted, ROLLBACK (all CIs cross 0, target cells flat). Flag default OFF; code kept. |
| **P5** Copy elimination attribution | ✅ done | Case B (slow-mode) + Case C-conditional (fast-mode). B/op -28.9% (HAZARD+W1 vs STAGING) all-reps; in slow-mode B/op is IDENTICAL (~120 B/op both). Fast mode reaches 17.5 Mops/s (HAZARD+W1 3/5 reps; STAGING 0/5). Bimodal collapse is the throughput gap. iter-15A: bimodal RCA = top priority. |
| **P6** Ground truth microbench | pending | |
| **P7** Full YCSB scaling sweep | pending | |
| **P8** TLS research | pending | |
| **P9** Summary + iter-15A backlog | pending | |

---

## Quick numbers

### P1 baseline (2026-05-19)
- Smoke: workload-d kv=8 T=4 cache=on → **1.42 Mops/s** trans_agg_thpt
- Hash-diff battery (5 workloads × 4 KV): **20/20 PASS** on baseline build
- Both hosts: 6 named/pinned receivers + 3 named/pinned senders verified

### P3 preflight (2026-05-19)
- microbench traces generated: 4 scenarios × 2 keyDists × 2 hosts × {load, trans} = 32 files
  - load: 2M unique INSERT ops (host 0 inserts, both hosts attach)
  - trans: 1M ops per host filtered by sharding partition
- build-cxl-w1-probe added (FUSEE_READ_GUARD=2 + FUSEE_WRITE_ALLOC=1 + FUSEE_PROBE=1)
- probe overhead measurement: bimodal noise dominant (~10 Mops/s and ~17 Mops/s modes coexist in same cell). Within-mode probe overhead ≈ 0-3%, acceptable for P4
- iter-12A bimodal supposed-fix did NOT eliminate bimodal — this is a P4 anomaly to investigate
- stage spec: 28 PROBE_OP tags categorized; W/R/I top-level + P5W/P5R micro-stages

### P4 production path_decomp (2026-05-19)
- Build: build-cxl-w1-probe (HAZARD + W1 RESERVED + FUSEE_PROBE=1)
- 4 cells × 20k ops × 1 rep (MAX_OPS=20k to bound probe-data disk usage)
- Workloads: workloada T=4/64 kv=1024 c=on, workloadc T=64 kv=1024 c=on, workloadb T=64 kv=256 c=on
- Output: [docs/iter14A_p4_production_pathdecomp_20260519_024047/per_stage_decomp.md](../iter14A_p4_production_pathdecomp_20260519_024047/per_stage_decomp.md)
- Anomalies found:
  - **W10 SOFT** = 3.54 µs p50 vs spec 1 µs (3.5×): cache_pool_insert MESI ping-pong on 1088B KvCacheEntry; same finding as iter-9A/10A/13A; no small-LOC verified fix → iter-15A backlog
  - **R2hit MILD** = 0.23 µs p50 vs spec 0.03 µs (7.7×): `lru_epoch.store` write-on-read in cache_pool_lookup → F2 LRU sampling fix attempted
- W10 is T-invariant (T=4: 3.37µs ≈ T=64: 3.54µs) → structural, not contention-driven

### P4.3 F2 measurement (2026-05-19) — F2 ROLLBACK
- Builds: build-cxl-w1 (baseline) vs build-cxl-w1-lru (FUSEE_LRU_SAMPLE=1)
- 5 cells × 5 reps × 200k ops
- **First attempt** (24:0400): flag declared in cxl_read_guard.h but NOT wired in cache_pool.cc — null compare. Implementation gap caught + fixed.
- **Real measurement** (25:5549): real impl rebuilt on both hosts. Bootstrap 95% CI (10000 resamples, seed=42):

| Cell | delta% | CI95 | Role | Decision |
|---|---:|---|---|---|
| workloadc T=64 kv=1024 c=on | -1.07% | [-12.48, +2.76] | target | FAIL |
| workloadc T=64 kv=256 c=on | +2.38% | [-7.73, +9.06] | target | FAIL |
| workloadb T=64 kv=1024 c=on | +0.55% | [-7.50, +7.43] | target | FAIL |
| workloada T=64 kv=1024 c=on | -1.08% | [-6.40, +6.49] | guard | FAIL |
| workloadd T=64 kv=1024 c=on | +0.61% | [-9.59, +14.36] | guard | FAIL |

- 0 / 3 target cells reach +1% CI-lo threshold per fix policy → **ROLLBACK**.
- R2hit anomaly is real, just not throughput-load-bearing (W10 + bimodal dominate).
- Decision: [docs/iter14A_p4_f2_lru_sample_real_20260519_025549/decision.md](../iter14A_p4_f2_lru_sample_real_20260519_025549/decision.md)

### P5 attribution (2026-05-19) — case B (slow) + case C-conditional (fast)
- Builds: build-cxl (STAGING) vs build-cxl-w1 (HAZARD+W1)
- Cell: workloada T=64 KV=1024 cache=on, 5 reps each, with pcm-memory
- Median total memory BW: STAGING 1230 MB/s → HAZARD+W1 1348 MB/s (+9.6%)
- Median trans_agg_thpt: 10.69M → 17.20M (+60.8%) — driven by bimodal-fast mode incidence
- **Bytes/op (load-bearing)**: 119.9 → 85.2 (-28.9% total memory traffic per op)
- All channels uniformly -25% to -31% B/op: copy elim works at data-movement level
- **Bimodal segmentation**:
  - Slow-mode vs slow-mode: B/op identical (~120), thpt identical (~10.7) → case B
  - Fast-mode (HAZARD+W1 only): B/op 80, thpt 17.4 → case C-conditional
  - STAGING 0/5 reps in fast mode; HAZARD+W1 3/5 reps in fast mode
- Conclusion: **In slow mode, copy elim makes no observable difference.** Fast mode reaches 17.5 Mops/s. Bimodal RCA = top iter-15A priority.
- Detail: [docs/iter14A_p5_attribution_20260519_030131/conclusion.md](../iter14A_p5_attribution_20260519_030131/conclusion.md)

### P2 measurement (2026-05-19) — F1 ROLLBACK
- Hash-diff battery on build-cxl-p2: **20/20 PASS** (§I9 preserved)
- G6 rw_race_test: pre-existing-broken at baseline (build-cxl-w1 also fails); documented [docs/iter14A_p2_*/g6_pre_existing_issue.md], iter-15A backlog item
- Measurement vs build-cxl-w1:
  - workloada T=4 c=off kv=1024: **-13.18%** (CI [-13.33, -4.58]) ← target regress
  - workloada T=32: -0.40%, T=64: -1.80% — flat
  - workloadc T=64 c=on: +2.80%, workloadb T=64 c=on: +2.05% — guardrails OK
- Decision: ROLLBACK per universal fix policy. CMake `FUSEE_XHOST_WRITE_SELF_INVAL` default 0.

---

## Auto-notify events (stage spec re-stages, etc.)

(populated as events occur)

---

## Pending decisions (none waiting on user)

iter-14A runs autonomously; all decisions follow plan + fix policy.
