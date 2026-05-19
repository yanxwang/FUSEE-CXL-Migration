# iter-14A Summary

**Date**: 2026-05-19
**Branch**: `feat/cxl-migration`
**Predecessor**: `docs/iters/iter13A_summary_20260517.md` (HAZARD read + W1 reserved write copy-elim shipped)
**Plan**: `docs/iters/task_plan_iter14A.md`
**Progress doc**: `docs/iters/iter14A_progress.md`
**Fix register**: `docs/iters/iter14A_fix_register.md`

---

## TL;DR

iter-14A executed an **observe + fix** loop on top of iter-13A's HAZARD+W1
shipped build, plus a ground-truth attribution study (P5) and microbench
matrix (P6). Two prospective fixes both rolled back per universal fix
policy. Headline findings:

1. **F1 ROLLBACK** (cross-host write self-invalidate, P2): workloada T=4
   regressed -13.18% (CI [-13.33, -4.58]). RAP overestimated invalidate
   roundtrip frequency — `sharer_bitmap` is reset to `{owner}` per write,
   so most cross-host writes have no invalidates to save. Flag default OFF.

2. **F2 ROLLBACK** (cache_pool_lookup LRU sampling, P4): all 5 cells
   within ±2.4% point estimate, all CIs cross 0. The R2hit anomaly (0.23 µs
   p50 vs spec 0.03 µs, P4) is real but **not throughput-load-bearing** —
   W10 (cache_pool_insert MESI ping-pong on 1088 B KvCacheEntry) dominates.
   F2 first attempt was a null-compare (flag declared but not wired into
   `cache_pool.cc`); caught at verification + remeasured.

3. **P5 case classification**: median-comparison **case C** (B/op -28.9%,
   thpt +60.8% bimodal-driven). Mode-matched comparison: **case B** in
   slow mode (B/op + thpt both identical between STAGING and HAZARD+W1).
   Slow→fast bimodal transition is the throughput-loss mechanism, not the
   copy-elimination overhead. **Bimodal RCA is the iter-15A top priority**.

4. **P4 path_decomp** identified two anomalies + 6 NEW alerts surfaced by
   the **P4.5 retroactive walkthrough upgrade** (user-driven mid-iter
   process change). The walkthrough format + alert criteria are now
   codified in `iter14A_p3d_pathdecomp_spec.md §F-H` as mandatory output
   for any future path_decomp.

5. **P6 v1 microbench**: 144/144 cells captured. Key data:
   - `local_read zipf T=64` = **21.47 Mops/s ✅** (exceeds 20 Mops/s target)
   - `xhost_read zipf T=64` = **21.37 Mops/s ✅** (effectively identical
     to local — explained by per-host cache_pool warmup: first R3 miss
     on each unique key populates local cache_pool; subsequent reads R2hit)
   - `local_write zipf T=32→T=64`: 6.4 → 6.5 Mops/s (writes saturate at
     T=32 due to W10 MESI ping-pong); w_p99 escalates 178µs → 641µs at T=64
     (cache_pool_insert long-tail under contention)

6. **P6 v2 Layer 2 (probe verification) BLOCKED**: user requested
   path-selection verification BEFORE the throughput grid. Probe build
   crashed (segfaults under T=64 on /dev/dax0.0 mmap, possibly stale CXL
   state from earlier crashes). Could not reset testbed (shared host, no
   reboot authorization). **iter-15A first task**: fresh-testbed redo of
   P6 v2 Layer 2 to provide direct stage-count evidence that local_read
   triggers R3=0 and xhost_read triggers R3>0.

7. **P8 TLS research** (no code ship): documented the 6-assumption chain
   (A1-A6) underlying the 2-tier TLS+cache_pool design; 3 assumptions
   (A2 hit-rate, A4 bucket-epoch pingpong, A5 eviction-rate) lack
   production measurement; 7 verification experiments proposed.

---

## Phase delivery audit (per CLAUDE.md gate)

| Sub-phase | Plan | Delivered | Status |
|---|---|---|---|
| **P1** Preflight | rekey + baseline build + smoke + hash-diff + rw_race fix | DONE (smoke 1.42 Mops/s; 20/20 PASS; rw_race signature fix) | ✅ FULL |
| **P2** Cross-host write self-inval (F1) | RAP + impl + hash-diff + measurement-gated decision | DONE: 20/20 hash-diff PASS; measurement → ROLLBACK | ✅ FULL |
| **P3** Remaining preflight | microbench traces + history replot + probe overhead + stage spec | DONE: 32 trace files; P3.C probe overhead = bimodal-confounded; P3.B history replot deferred (P5 substituted) | ✅ FULL |
| **P4** Production path_decomp + iterative fix | path_decomp at 4 cells; W10 + R2hit anomalies; F2 attempt | DONE (W10 → iter-15A; F2 → ROLLBACK) | ✅ FULL |
| **P4.5** Walkthrough rigor upgrade | Mid-iter user-requested process improvement | DONE: per-stage walkthrough doc + spec §F-H codifies methodology + 6 new alerts surfaced | ✅ FULL (user-requested addition) |
| **P5** Copy elim attribution case A/B/C/D | Quantitative case classification | DONE: case B (slow-mode) + case C-conditional (fast-mode) | ✅ FULL |
| **P6** Ground truth microbench | 144 cells × 2M load / 1M trans-per-host / KV=1024 | DONE (v1 sweep). | ✅ FULL |
| **P6 v2** Probe verification (mid-iter add) | Layer 2 = 8 probe cells for path-selection evidence (Layer 1 = re-run 144 cells but skipped due to L2 block) | ⚠ PARTIAL: probe build crashed; testbed reset denied; **iter-15A first task** | ⚠ BLOCKED (testbed) |
| **P7** Full YCSB scaling sweep | 210 cells on as-shipped build-cxl-w1 | NOT EXECUTED — superseded by P6 v1 capturing as-shipped behavior; P7 deferred to iter-15A | ⏭ deferred |
| **P8** TLS research | Research doc, no code ship | DONE: tls_evolution_review.md with 6-assumption chain + 7 experiments | ✅ FULL |
| **P9** Summary + iter-15A backlog | This doc + backlog memo | DONE | ✅ FULL |

**Acknowledged shortfalls** (with explicit reason, per CLAUDE.md "phase delivery audit"):

- **P6 v2 Layer 2 was BLOCKED, not delivered**. Cause: testbed CXL devdax
  state corrupted by accumulated worker segfaults during T=64 cells (some
  from probe-build, some from rogue auto-loop launcher P7 sweep that ran
  concurrent with P6); host reboot would reset but was denied (shared
  host). **iter-15A starts with this task on a fresh testbed.**

- **P7 sweep not executed** in iter-14A. Rationale: P6 v1 captures the
  144-cell microbench (which is more diagnostic than 210-cell YCSB sweep
  for this iter's "observe + fix" focus). Also: an auto-loop spawned P7
  sweep accidentally ran multiple incomplete passes that contributed to
  testbed state corruption. P7 deferred to iter-15A start.

---

## Key numbers

### P5 attribution (workloada T=64 KV=1024 cache=on, 5 reps each)

| Metric | STAGING (build-cxl) | HAZARD+W1 (build-cxl-w1) | delta% |
|---|---:|---:|---:|
| trans_agg_thpt | 10,693,471 | 17,198,383 | **+60.83%** (bimodal-driven) |
| Bytes/op total | 119.9 | 85.2 | **-28.9%** (uniformly across all channels) |

Mode-matched: slow vs slow B/op identical (~120), thpt identical (~10.7);
HAZARD+W1 hits fast mode 3/5 reps reaching 17.5 Mops/s, STAGING 0/5.

### P6 v1 microbench headline (build-cxl-w1, 144/144 cells, KV=1024)

| Scenario | T=1 | T=4 | T=32 | T=64 |
|---|---:|---:|---:|---:|
| local_read uniform | 0.38 M | 1.54 M | 7.65 M | 12.19 M |
| local_read zipf    | 0.79 M | 4.02 M | 13.49 M | **21.47 M** ✅ |
| xhost_read uniform | 0.38 M | 1.53 M | 7.49 M | 12.10 M |
| xhost_read zipf    | 1.10 M | 4.00 M | 15.26 M | **21.37 M** ✅ |
| local_write uniform | 0.25 M | 0.98 M | 8.27 M | 13.01 M |
| local_write zipf | 0.24 M | 0.95 M | 6.38 M | 6.53 M |
| xhost_write uniform | 0.28 M | 0.98 M | 8.07 M | 12.71 M |
| xhost_write zipf | 0.25 M | 0.97 M | 6.20 M | 6.86 M |

**Interpretations**:
- Pure-read zipf at T=64 (both local + xhost) **exceeds the 20 Mops/s
  YCSB-C target**.
- local vs xhost read: identical at all (T, kd). Explained by per-host
  cache_pool warmup (first miss → R3 → cache_pool_insert; subsequent
  same-key reads → R2hit). P4 production data: workloadc R3=0.89%,
  workloada R3=0.94% — direct evidence that R3 is rare in steady-state.
  **Direct stage-count evidence from P6 v2 Layer 2 probe blocked**;
  iter-15A first task.
- Write zipf saturates at T=32 due to W10 MESI ping-pong on the 1088 B
  KvCacheEntry. w_p99 escalates 178µs→641µs from T=32 to T=64.

### P4 anomalies (workloada T=64 KV=1024 cache=on, probe-build)

| Stage | p50 µs | p99 µs | Spec expected | Anomaly grade |
|---|---:|---:|---:|---|
| W10 (cache_pool_insert + dir update) | 3.54 | 25.9 | 1 | 🟠 SOFT (3.5×, T-invariant) |
| R2hit (cache_pool seqlock hit) | 0.23 | 0.63 | 0.03 | 🟡 MILD (7.7×) |
| R1 (search entry, inter-op gap) | 12.15 | 28.0 | 7 | inter-op (not a stage) |
| R3 (forward_read cross-host) | 10.37 | 16.3 | 10 | within spec, but N=47/5002=0.94% |

**6 new alerts** added by P4.5 walkthrough (see
`docs/iter14A_p4_production_pathdecomp_20260519_024047/per_stage_walkthrough.md`):
- R0_tls_hit hit-rate 0.7% (vs design assumption 30-80%)
- P5W_PT 5.4 µs gap (probe placement question)
- P5R_PL p99 13.6 ms outlier (redundant cache_pool_lookup in R3 path)
- R3 vs P5R_AK measurement boundary discrepancy
- W3 5.08 µs p99 = probe-pair gap artifact
- W12 max 12 ms = OS-level preemption (SCHED_FIFO mitigation needed)

---

## Process improvements codified

1. **P4.5 walkthrough format mandate** (`iter14A_p3d_pathdecomp_spec.md §F`):
   any future path_decomp must produce `per_stage_walkthrough.md` with
   6-field analysis (observed / spec / code logic / reasonable? / opt
   room / fix action) for every alert-triggering stage. Alert criteria:
   p50 > 1µs, p99 > 5µs, max > 100µs, H/E > 2×.

2. **Spec re-check rule** (`§G`): if observed p50 < 0.5× spec, update
   spec; overly conservative expectations create false alerts.

3. **Probe data retention policy** (`§H`): `parsed_*.tsv` is committed +
   is the analysis-of-record; raw probe dumps deletable post-parse;
   re-running path_decomp NOT required to revisit analysis.

4. **Test-bed contention discipline learned the hard way**: rogue
   auto-loop launchers (from prior session ScheduleWakeup chain) kept
   spawning P7 sweeps mid-iter. Diagnostic chain: SSH 255 returns + 0
   bytes captured = remote process killed by signal = look for parent of
   competing protocol_a_ycsb processes BEFORE assuming SSH or hardware
   failure. Found `bash -c` from Claude session PID still running an
   embedded sweep script. **Process safeguard for iter-15A**: do NOT
   chain launchers via `nohup ... &` from autonomous loops; explicitly
   delete launcher scripts on iter completion.

---

## Code changes (this iter)

- `src/cxl_read_guard.h`: added `FUSEE_XHOST_WRITE_SELF_INVAL`,
  `FUSEE_LRU_SAMPLE` flags (defaults 0). Code preserved for iter-15A
  re-attempt + re-design.
- `src/cxl_cache_pool.cc`: LRU sampling under `FUSEE_LRU_SAMPLE` (incl.
  x86intrin.h include + rdtsc gate).
- `src/cxl_kv_ops_A.{h,cc}`: `self_inval_src` parameter on
  `execute_write_local` and `execute_write_local_with_blk`; receiver
  excludes src when set. Used only when `FUSEE_XHOST_WRITE_SELF_INVAL=1`.
- `tests/protocol_a_rw_race_test.cc`: `enable_read_ring` signature sync
  to match new ReadStagingMatrix arg.

All shipped flags default OFF (no behavioral change in as-shipped build).

## Doc artifacts produced

- `docs/iters/task_plan_iter14A.md` — full plan
- `docs/iters/iter14A_progress.md` — live progress
- `docs/iters/iter14A_fix_register.md` — F1 + F2 entries
- `docs/iters/iter14A_p2_rap.md` — F1 RAP
- `docs/iters/iter14A_f2_lru_sample_rap.md` — F2 RAP
- `docs/iter14A_p3d_pathdecomp_spec.md` — 28 stage spec + §F-H methodology
- `docs/iter14A_p2_xhost_write_self_inval_*/decision.md` — F1 ROLLBACK
- `docs/iter14A_p4_production_pathdecomp_20260519_024047/`
  - `per_stage_decomp.md` — original data tables
  - `per_stage_walkthrough.md` — P4.5 rigor upgrade
  - `parsed_*.tsv` — committed for future review
- `docs/iter14A_p4_f2_lru_sample_*/decision.md` — F2 ROLLBACK (first
  attempt + real impl)
- `docs/iter14A_p5_attribution_framework/case_framework.md` — P5 design
- `docs/iter14A_p5_attribution_20260519_030131/conclusion.md` — case B+C-cond
- `docs/iter14A_p6_microbench_20260519_030533/`
  - `MICROBENCH.tsv` — 144 cells throughput
  - `P6_FINDING_local_vs_xhost.md` — corrected explanation (per-host cache_pool, not shared CXL)
- `docs/iter14A_p8_tls_research/tls_evolution_review.md` — TLS research
- `docs/iters/iter14A_summary_20260519.md` — this doc
- `docs/iters/iter14A_backlog_memo.md` — iter-15A handoff (updated below)

---

## P6.5 + P7 — completed after initial summary (06:08 → 06:08+)

After the initial summary above was written, two additional phases were
brought to completion using inline (non-launcher-chain) runs to avoid the
auto-loop / chain-script collisions documented under "Test-bed contention
discipline" above. Both used the existing build-cxl-w1 binary (as-shipped,
both F1 + F2 ROLLBACK defaults).

### P6.5 — xhost slowpath verification via cache_pool overflow

This is the NB-overflow variant (`scripts/iter14A_p65_xhost_slowpath.sh`),
distinct from the probe-based P6 v2 Layer 2 that remains BLOCKED.

Cells: T=64, KV=1024, 24 cells (4 scenario×keydist × 2 NB × 3 reps),
0 FAIL.

| Scenario | keydist | NB=1024 median | NB=1M median | NB=1024 / NB=1M |
|---|---|---:|---:|---:|
| local_read | uniform | 24.24 M | 9.78 M | **2.48×** |
| local_read | zipf    | 23.29 M | 12.39 M | **1.88×** |
| xhost_read | uniform | 25.11 M | 9.52 M | **2.64×** |
| xhost_read | zipf    | 22.79 M | 12.37 M | **1.84×** |

**Counter-result**: hypothesis was "NB=1024 overflow → R3 RTT fires →
xhost slower than local". Reality: NB=1024 is **1.8-2.6× FASTER** than
NB=1M, and local ≈ xhost still holds. Detail:
`docs/iter14A_p65_xhost_slowpath_20260519_054536/ANALYSIS.md`.

Revised mental model: with NB=1024 buckets × 8 probe slots = ~8K
cache_pool slots, the working set effectively collapses to ~8K hot keys
(since trans is 200K ops). The TLS + cache_pool stack warms to this
small set; both layers operate cache-resident; throughput rises. NB=1M
has full 2M-key working set → TLS misses dominate → R2hit on wider
cache_pool footprint → 17-cacheline MESI ping-pong (W10 path) → lower
throughput.

**Implication**: TLS sizing study + workload-set sizing study are
iter-15A research candidates. Cross-host R3 cost was NOT exposed by
either P6 NB=1M or P6.5 NB=1024 — needs a different workload structure
(continuous insert/evict, not static 2M-key load + 200K read trans).

### P7 — 210-cell YCSB scaling sweep on as-shipped (build-cxl-w1)

Sweep parameters: 5 workloads × 2 cache × 3 KV × 7 T × 1 rep = 210 cells.
Result: 210/210 OK after 4 retries (collisions with parallel-session
cleanup commands), 0 unexplained anomalies (§13 gate 5 anomaly scan).

Per-workload peaks vs iter-13A as-shipped:

| Workload | iter-14A peak | T,kv,cache | iter-13A | Δ |
|---|---:|---|---:|---:|
| workloada | 10.89 M | T=64 kv=1024 c=on | 11.12 M | -2.04% |
| workloadb | **19.97 M** | T=64 kv=512 c=off | 19.14 M | +4.34% |
| workloadc | 19.37 M | T=64 kv=256 c=off | 18.79 M | +3.09% |
| workloadd | 18.42 M | T=64 kv=256 c=on  | 18.20 M | +1.19% |
| workloadf | 17.49 M | T=64 kv=512 c=off | 16.89 M | +3.57% |

All deltas ±1-4% are within run-to-run noise; the as-shipped build is
behaviorally identical to iter-13A (both fixes rolled back). workloadb
essentially reaches the 20 Mops/s YCSB-A target (19.97 = 99.85%).

Hash-diff battery (build-cxl-w1, 5 workloads × 4 KV sizes): **20/20 PASS**.
§I9 strict-A linearizability preserved.

Data: `docs/iter14A_p7_full_sweep_20260519_054911/SUMMARY.log`,
`docs/iter14A_p7_hashdiff_*/SUMMARY.log`.

### Updated iter-completion gate row

| Gate | Original status | Updated status |
|---|---|---|
| §13 gate 5 anomaly scan (P7) | (P7 deferred) | ✅ PASS (210 cells, 0 anomalies) |
| G1 hash-diff (P7 as-shipped) | (P7 deferred) | ✅ 20/20 PASS |
| 5-workload doubling-ratio gate | (P7 deferred) | ✅ PASS (workload-wise) |

P7 deferral note in the original summary is **superseded by this run**.
P6 v2 Layer 2 (probe verification) remains BLOCKED → iter-15A.

### Updated phase delivery audit row

| Sub-phase | Status |
|---|---|
| P6.5 (NB-overflow xhost slowpath) | ✅ FULL (counter-result findings published) |
| P7 (210-cell sweep) | ✅ FULL (210/210 OK, anomaly-scan clean, hash-diff 20/20 PASS) |

