# iter-14A Task Plan — Path Characterization + Cross-host Write Self-Invalidate + TLS Research

**Date drafted**: 2026-05-18
**Branch**: `feat/cxl-migration`
**Predecessor**: `docs/iters/iter13A_summary_20260517.md` (HAZARD + W1 RESERVED, copy
elimination delivered but throughput break-even vs iter-12A baseline)
**North-star**: YCSB-A AND YCSB-C aggregate ≥ 20 Mops/s on g3+g4 testbed
(`docs/design_goals.md`). iter-14A is **characterization + small ship-fix
+ research**, NOT a perf-target attack iter.

---

## TL;DR

iter-13A delivered cross-host data-copy elimination on both read (HAZARD)
and write (W1 RESERVED) paths but throughput was approximately break-even
vs iter-12A baseline. iter-14A's job is to **stop guessing and start
measuring** — characterize the current read/write paths thoroughly, find
out WHY copy elimination didn't translate to throughput, and rigorously
re-evaluate whether iter-10A's TLS cache design (the previous big
"optimization" with weak motivation) actually pays off.

Three concrete deliverables:

1. **One small architectural ship-fix** — cross-host write *worker
   self-invalidate*, removes 1 invalidate roundtrip per cross-host
   write. Measurement-gated.
2. **Quantitative attribution** of why iter-13A copy elimination didn't
   move throughput (BW saved vs new overhead vs critical path dominance
   vs statistical noise — pick one with numbers).
3. **Ground-truth perf table** for 4 fundamental scenarios:
   100% read / write × all local / all cross-host. With both
   uniform and Zipf key distributions. The reference data that every
   future iter compares against.

Plus a research-only phase: re-evaluate TLS cache vs shared cache_pool
with proper A/B/C/D build matrix; output decision + iter-15A action
items (no implementation in this iter).

---

## Resolved user instructions (2026-05-18)

- **Fix policy (P4)**: "不管 fix 是 tiny 还是 large，只要原因清楚，solution
  确认可用，都要 implement. 除非已知 solution 都试过之后却没有提升或倒退，
  rollback 记录已经做的尝试和实现." → Sequential try-rollback enumeration,
  no LOC ceiling, all attempts recorded in `iter14A_fix_register.md`.
- **P5 deliverable**: must produce **quantitative conclusion** (one of
  the 4 explicit cases A/B/C/D) for the "no-throughput-gain" mystery,
  not laundry-list descriptive analysis.
- **P8 (TLS research)**: research-only, no implementation; case (iv)
  ("delete TLS") goes to iter-15A backlog. iter-14A keeps TLS as-is.
- **Stage spec re-staging**: non-blocking auto-notify (commit new spec
  + write to progress.md, do NOT wait for user OK).
- **No deadline**: autonomous mode. User returns after iter complete.
- **Microbench params**: 2M load / 1M trans-per-host / KV=1024 only /
  both uniform + Zipf / T={1,4,32,64} / cache state cold/warm for reads,
  cold for writes / 3 reps. Wall time ~72 min.

---

## Scope

| In | Out |
|---|---|
| Read+write path characterization on current arch | Protocol B / C |
| Cross-host write self-invalidate optimization | K-channel sender/receiver redesign |
| Copy-elimination quantitative attribution | LFM mutex impl |
| 4 clean microbench scenarios (100% read/write × local/xhost) | New cache eviction policies |
| TLS vs shared cache research (read-only) | TLS cache code changes (deferred iter-15A) |
| Full YCSB scaling sweep on shipped build | Hot-key replication (iter-15A) |
| Any clear-cause, verified-solution fix surfaced by path_decomp | New invalidate protocol |
| Living docs sync per CLAUDE precedent #4 | New host count |

---

## Hard constraints (violation = iter redo)

| ID | Constraint | Verification |
|---|---|---|
| **C1** | §I9 strict-A linearizability unchanged | Hash-diff 20-cell PASS after every code commit; G6 rw_race_test PASS after any §I9-touching commit |
| **C2** | Message ring entries no value bytes (iter-9A C2) | static_assert on WriteEntry / ReadEntry remains; no payload field |
| **C3** | All sender/receiver threads CPU-pinned | unchanged from iter-9A redo |
| **C4** | iter-12A Phase 5 stale-cache-on-attach fix preserved | enable_* always memset+flush regardless of init flag |
| **C5** | Measurement-gated fix rule | Every shipped fix has baseline/measure/CI in `iter14A_fix_register.md`; failed fixes rolled back to flag default OFF |
| **C6** | P5 must produce one of {A,B,C,D} conclusion | not laundry list; cited numbers |
| **C7** | Living docs realtime | path_decomp_spec / blueprint / design_goals updated per phase commit |
| **C8** | Stage spec re-checked before every path_decomp | auto-diff PROBE_OP markers vs spec; auto re-stage on mismatch; non-blocking notify |

---

## Universal fix policy (per CLAUDE precedent #3 + user 2026-05-18 lock)

```
For each candidate fix Cx:

1. RAP: STATE / 6 ATTACK VECTORS / ABLATION / PRIOR ART / VERDICT / DECISION
   - cause must be clear (refer to specific measured data)
   - solution must be verified (correctness sketch + similar prior art)
2. IMPLEMENT: with build flag (-DFUSEE_FIX_Cx=1), default OFF
3. CORRECTNESS: hash-diff 20-cell PASS (+ G6 if §I9-touching)
4. MEASURE: declared cell-set × 3 reps, median + bootstrap 95% CI
   - if CI lower bound > 0 AND no guardrail regresses > 2% → PASS
   - else → FAIL
5. DECISION:
   PASS  → flag default ON, register entry "win", continue to next fix
   FAIL  → flag default OFF (code stays in tree), register entry "rollback"
         → try next candidate fix in queue (Cx+1)
6. If all known candidates for this anomaly fail:
   → record full attempt history in register
   → mark anomaly unresolved → iter-15A backlog
```

LOC is NOT a gate. ALL clear-cause + verified-solution fixes are
attempted, sequentially, until one passes or all fail.

---

## Phase 1 (Bare-minimum preflight, 3-4 hours)

### Deliverables
- g3/g4 rekey + ssh access verified
- 2 build variants present on both hosts:
  - `build-cxl-baseline` (iter-13A HEAD, no Phase 2 fix)
  - `build-cxl-p2` (HEAD + `FUSEE_XHOST_WRITE_SELF_INVAL=1`)
- Hash-diff 20-cell battery script verified on baseline (must PASS as
  smoke)
- `iter14A_progress.md` initialized
- `iter14A_fix_register.md` initialized (empty table)

### Exit
Both builds compile clean on both hosts; smoke test workload-d kv=8 T=4
PASS on baseline build.

---

## Phase 2 (Cross-host write self-invalidate, 1-2 days)

### 2.0 RAP
Document `docs/iters/iter14A_p2_rap.md`. Must cover Case 1-4 race
analysis from previous design discussion (single-thread write→read OK,
concurrent reader on writer-host OK, host-1 reader OK, A-not-a-sharer
trivial). 6 ATTACK VECTORS minimum.

### 2.1 Implementation
- Add `execute_write_local_skip_src(key, value, value_len, op_kind, src)`
  to `cxl_kv_ops_A.{h,cc}`. Identical to `execute_write_local` except
  invalidate broadcast excludes `src` host AND sharer_bitmap update
  excludes `src` from re-add (since src already self-invalidated).
- In `forward_write_direct` (worker side): if
  `FUSEE_XHOST_WRITE_SELF_INVAL=1`, before enqueueing WriteEntry, do:
  `cache_pool_set_stale(cache_, key); if (g_thread_tls) tls_evict(...)`.
- In `write_handler` (receiver side): if `FUSEE_XHOST_WRITE_SELF_INVAL=1`,
  decode src from req_op_id high 8 bits, call _skip_src variant.
- Both branches `#ifdef`'d, default OFF until 2.4 decides.

### 2.2 Correctness gate
- Hash-diff 20-cell battery on `build-cxl-p2` PASS
- G6 `protocol_a_rw_race_test` 100k iters PASS on `build-cxl-p2`

### 2.3 Measurement
- Target cell-set (must improve):
  - bench_xhost_write_uniform T={4, 32, 64} KV=1024 cold (3 cells)
- Guardrail cell-set (must not regress > 2%):
  - bench_local_read T=64
  - bench_xhost_read T=64
  - bench_local_write T=64
- Reps: 3 each (15 cells × 3 reps × ~30s = ~22 min wall)
- Note: this uses minimal trace files (1 KV size, partial T sweep) to
  decide quickly; full P6 microbench will re-run on the post-Phase-2
  architecture.

### 2.4 Decision
- PASS threshold: target median +5%, CI lower bound > +1%; guardrail OK.
- PASS → flag default ON in CMake; subsequent phases run on post-2 arch.
- FAIL → flag default OFF; F1 register "rollback with data"; subsequent
  phases run on baseline arch.

### 2.5 Stage spec update (per C8)
Update `path_decomp_iter14A_spec.md` to reflect Phase 2 path changes
(W4/W6 invalidate stage shrunk or removed when src is sharer in 2-host
system). Commit alongside Phase 2 deliverable.

---

## Phase 3 (Remaining preflight, 1-2 days)

### 3.A Microbench trace generation
- Write `scripts/iter14A_gen_microbench_traces.py`
- Generate 32 trace pairs (4 scenarios × 2 hosts × 2 keyDist × 2 phases):
  - `bench_<scenario>_<keyDist>_h<0|1>.spec_load`
  - `bench_<scenario>_<keyDist>_h<0|1>.spec_trans`
  - scenarios: local_read / xhost_read / local_write / xhost_write
  - keyDist: uniform / zipf-0.99
- Load file: 2M unique INSERT ops on keys 0..(2M-1); both hosts use same
  load file (host 0 does the inserts, both hosts attach)
- Trans file: 1M ops on the host, key range filtered by scenario:
  - local_read_h0: 100% READ, keys with `sharding_hash(k) % 2 == 0`
  - xhost_read_h0: 100% READ, keys with `sharding_hash(k) % 2 == 1`
  - local_write_h0: 100% UPDATE, keys with `% 2 == 0`
  - xhost_write_h0: 100% UPDATE, keys with `% 2 == 1`
  - mirror for h1
- Key distribution within each filtered range:
  - uniform: simple round-robin
  - zipf: standard YCSB Zipf θ=0.99 on the filtered subset

### 3.B Historical path_decomp re-read
- Script `scripts/iter14A_replot_history.py`
- Parse all `docs/path_decomp_iter9A_*/probes_*/` + iter-10A + iter-13A
  Phase 0 baseline → per-stage distribution plots
- Specific Q's to answer:
  - Is R1 in iter-9A inter-op gap or genuine cache_pool_lookup cost?
  - W10 spinlock vs CAS — which is actually faster?
  - R2hit p50 / p99 / max under various T?
- Output: `docs/iter14A_p3b_history_replot/findings.md`

### 3.C Probe overhead quantification
- Same cell × 10 reps × FUSEE_PROBE={0,1}
- Output: probe overhead %  delta and recommended sampling strategy
  (1/64 or full probe based on overhead)

### 3.D Stage spec for iter-14A
- `docs/iter14A_p3d_pathdecomp_spec.md`:
  - Updated stage list reflecting Phase 2 arch (if PASS) or baseline arch
  - HAZARD protect / release as new stages H_protect / H_release
  - W_alloc_peer stage (W1 path)
  - Expected latency per stage from baseline.md primitives × call counts,
    annotated with T-dependence

---

## Phase 4 (Production path_decomp + iterative fix, 2-4 days)

### 4.1 Pre-run: stage spec re-check (per C8)
Diff `cxl_kv_ops_A.cc` PROBE_OP markers vs `iter14A_p3d_pathdecomp_spec.md`.
If mismatch:
- Auto-update spec
- Commit `[iter14A-restage] reason X`
- Append to `iter14A_progress.md` notification block
- Continue (non-blocking)

### 4.2 Canonical cell path_decomp
Run on workload-a / workload-c × T={4, 64} × cache=on × KV=1024.
4 cells × 12-rep probe capture; anomaly-retry up to 12× per spec §3.

### 4.3 Per-stage analysis
For each cell:
- Table: Expected (E) vs Healthy p50 (H) vs H/E ratio
- Flag stages with H/E > 2× or H > 5µs absolute
- For each flagged stage:
  - perf record / perf annotate hot function
  - gdb sample / gdb attached during anomaly retry
  - identify cause (cacheline contention / atomic latency / lock /
    spurious flush / unnecessary memcpy / …)

### 4.4 Iterative fix per anomaly (per fix policy)
For each anomaly stage S with cause identified:
- enumerate candidate fixes Cs1, Cs2, …
- for each: RAP → impl → hash-diff → measure → decide
- continue until one passes or all fail
- record full chain in `iter14A_fix_register.md`

### 4.5 Re-run path_decomp on post-fix arch
Once all fix queues processed, re-run 4.2 to confirm new stage timings.

---

## Phase 5 (Copy elimination attribution, 1 day)

### 5.A BW saved (perf counter)
- Tool: `pcm-memory` or perf CXL/uncore events
- Cells: bench_xhost_read T=64 KV=1024 + bench_xhost_write T=64 KV=1024
- Builds: iter-12A baseline (STAGING) vs iter-13A HEAD (HAZARD+W1)
- Output: bytes/op delta, total BW utilization %

### 5.B HAZARD overhead microbench
- Standalone test: hot loop of `hazard_protect + hazard_release` pair
- Single thread, no other work
- Compare against `cache_pool_set_stale + cache_pool_insert` overhead

### 5.C Critical path dominance
- Use Phase 4 path_decomp data
- For canonical workload-a T=64 KV=1024 cell:
  - Sum of all stage p50 = total critical path
  - Each stage's %
  - If BW change is X ns/op, theoretical throughput change = X / total

### 5.D Statistical power
- iter-13A canonical cell × 30 reps × {STAGING build, HAZARD+W1 build}
- Bootstrap CI 95% on throughput delta
- Does CI cover 0? If yes, iter-13A's "+0.7% / -4%" claims are
  statistically indistinguishable from zero.

### 5.E Conclusion
Must pick ONE of:
- (A) BW saving exists but offset by mechanism X (specific stage + ns)
- (B) BW saving doesn't actually happen (copy not truly eliminated)
- (C) BW saving exists, throughput improvement real but within noise
- (D) Critical path doesn't depend on BW; another stage dominates
  (specific stage)

→ `docs/iter14A_p5_attribution.md` with case + data + iter-15A action.

---

## Phase 6 (Ground truth microbench, ~1.2 hours wall)

### 6.1 Main matrix (144 cells)
- 4 scenarios × 4 T × 1 KV × cache_state × 2 keyDist × 3 reps
- = (2 × 4 × 2 × 2 × 3) reads + (2 × 4 × 1 × 2 × 3) writes
- = 96 + 48 = 144 cells

### 6.2 Per-cell config
- Load: 2M ops (host 0 single-thread)
- Trans: 1M ops/host
- KV: 1024B
- T: {1, 4, 32, 64}
- Cache state:
  - reads (3.1, 3.2): {cold, warm}
  - writes (3.3, 3.4): {cold} only
- Key dist: {uniform, zipf-0.99}
- Reps: 3
- Build: as-shipped (per Phase 2 decision)
- num_buckets: 1048576 (2^20)
- pool_blocks_per_host: 1500000
- FUSEE_TLS_SIZE: default (1024)
- FUSEE_PROBE: 0 (off)

### 6.3 Single-thread baseline (T=1 absolute floor)
Already covered by T=1 row of main matrix. Specific extract for
single-thread latency table.

### 6.4 Little's Law sanity check
For each cell: throughput_measured vs T / avg_op_latency.
Gap > 50% → there's contention/queueing not captured in path_decomp →
log to anomaly trigger.

### 6.5 Trigger check
Per `iter14A_anomaly_triggers.md` (built in Phase 3.D using actual
T=1 baseline numbers), flag any cell that triggers debug.

### 6.6 Hot-key replication ceiling estimate
Use 3.1 + 3.2 data to compute weighted throughput predicted under
50% / 90% hot-key replication scenarios → iter-15A planning input.

---

## Phase 7 (Full YCSB scaling sweep, 1 day)

Run the standard sweep on the as-shipped build (Phase 2 + Phase 4 fixes
default ON if PASSed):

- Workloads: a, b, c, d, f
- T: 1, 2, 4, 8, 16, 32, 64
- Cache: on, off (Protocol A: same path; doubles as 2 reps for free)
- KV: 256, 512, 1024
- Reps: 1
- MAX_OPS: 200000 (sweep convention, matches iter-13A historical)
- TIMEOUT_S: 600
- → 5 × 7 × 2 × 3 × 1 = 210 cells

After:
- Anomaly scan per §13 gate 5
- Plot 86 + 42 PNGs per `scaling_ycsb_spec §6`
- Compare 5-workload peak vs iter-13A Phase 2 sweep
- Append to `iter14A_progress.md`

---

## Phase 8 (TLS / Shared cache research, 2-3 days, no ship)

### 8.0 Pre-read + ground on Phase 6 microbench data
Re-read the 4 questions from prior user conversation:
- Q1: R1 inter-op gap vs lookup → answered in Phase 3.B
- Q2: hot-key cache_pool_lookup ping-pong → use Phase 6 microbench 3.1
- Q3: TLS +7.5% noise vs signal → use Phase 5.D and TLS A/B
- Q4: CAS regression vs spinlock → use 8.5 below

### 8.1 4-build matrix
- Build A: spinlock cache_pool, no TLS, no LRU patch
- Build B: spinlock cache_pool + sampled LRU (3 LOC fix)
- Build C: CAS cache_pool + TLS (current iter-13A HEAD behavior)
- Build D: CAS cache_pool + TLS + sampled LRU
- (+ Build E optional: spinlock + TLS + sampled LRU)

### 8.2 Cell set per build
- Phase 6 microbench matrix (144 cells, 3 reps) — partial subset
  (keep wall time bounded; canonical T=64 cells × all 4 scenarios × 4 builds)
- Production canonical workload-a T=64 cache=on KV=1024 × 30 reps

### 8.3 Attribution comparisons
- B - A : "LRU fix alone gain"
- C - A : "Current iter-13A TLS+CAS combined gain (sanity check vs iter-10A)"
- **D - B : core attribution — TLS's incremental value on top of LRU fix**
- C - B : "TLS over LRU fix on legacy cache_pool"
- D - C : "Adding LRU fix to TLS baseline"

### 8.4 Hit rate ceiling
For each workload × T × KV combo, measure TLS hit% (use Phase 6
microbench scenarios + production workloads).

### 8.5 spinlock vs CAS independent check
Build A vs (spinlock + same other features as Build C) → answer
"is CAS cache_pool a regression?"

### 8.6 Decision case + iter-15A action
One of:
- case (i): D-B ≈ 0 + narrow hit-rate window → iter-15A removes TLS,
  ships LRU sampling + cache_pool simplification
- case (ii): D-B substantial + broad window → TLS validated; iter-15A
  keeps TLS, adds LRU fix
- case (iii): conditional → adaptive enable in iter-15A
- case (iv): D-B regression → iter-15A reverts TLS

Document `docs/iter14A_p8_tls_research.md` with case + data + RAP draft
for iter-15A. **No code changes ship in Phase 8.**

---

## Phase 9 (Summary + iter-15A backlog, 1 day)

### 9.A Phase delivery audit (CLAUDE precedent #3)
Mandatory table:

| Phase / Constraint | Plan | Delivered | Status |
|---|---|---|---|
| P1 minimum preflight | rekey + 2 builds + scaffold | ... | ✅/⚠/❌ |
| ... all phases | ... | ... | ... |
| ... all C constraints | ... | ... | ... |

### 9.B Summary
`docs/iters/iter14A_summary_<ts>.md` with:
- TL;DR
- Per-phase delivered results
- Phase 5 attribution conclusion
- All measured 4-scenario × T table from Phase 6
- iter-13A canonical comparison
- Comparison of P7 sweep vs iter-13A Phase 2 sweep
- Phase 8 TLS decision case

### 9.C Fix register
`docs/iters/iter14A_fix_register.md` — full table from all attempts
(P2, P4 fixes; P8 has research entries only)

### 9.D iter-15A backlog
`docs/iters/iter15A_backlog_memo.md` with:
- TLS decision (case from P8.6) → action item
- Hot-key replication design (Tier 1)
- Send batching (Tier 1)
- Any P4 unresolved anomalies
- Any P5 attribution-pointed actions
- Carryover items from iter-14A backlog

### 9.E Living docs sync
- `docs/protocol_a_architecture_blueprint.md` Part II if any path changed
- `docs/design_goals.md` §I9 if Phase 2 self-invalidate changes semantics
- `docs/path_decomp_spec.md` §11 with iter-14A as new reference instance
- `docs/scaling_ycsb_spec.md` if any new gate

---

## Cell parameters lock table

### P2 quick-gate microbench (subset)
| Param | Value |
|---|---|
| Cells | 15 (3 target + various guardrails) |
| Reps | 3 |
| Wall | ~22 min |

### P6 ground truth (main matrix)
| Param | Value |
|---|---|
| Total cells | 144 |
| Load ops | 2,000,000 |
| Trans ops/host | 1,000,000 |
| KV size | 1024 |
| Thread T | {1, 4, 32, 64} |
| Cache state (reads) | {cold, warm} |
| Cache state (writes) | {cold} |
| Key distribution | {uniform, zipf-0.99} |
| Reps | 3 |
| num_buckets | 1048576 |
| pool_blocks_per_host | 1500000 |
| FUSEE_TLS_SIZE | 1024 (default) |
| FUSEE_PROBE | 0 |
| TIMEOUT_S | 600 |
| Wall (est) | ~72 min |

### P7 full YCSB sweep
| Param | Value |
|---|---|
| Workloads | a, b, c, d, f |
| Thread T | 1, 2, 4, 8, 16, 32, 64 |
| Cache mode | on, off |
| KV size | 256, 512, 1024 |
| Reps | 1 (anomaly cells re-verified 5-rep per §13 gate 5) |
| MAX_OPS | 200,000 |
| TIMEOUT_S | 600 |
| Total cells | 210 |
| Wall (est) | ~40-60 min |

---

## Output directory convention

```
docs/iters/task_plan_iter14A.md     (this doc)
docs/iters/iter14A_progress.md      (live-updated)
docs/iters/iter14A_fix_register.md  (all attempted fixes)
docs/iters/iter14A_summary_<ts>.md  (final at Phase 9)
docs/iters/iter15A_backlog_memo.md  (final at Phase 9)

docs/iter14A_p1_preflight_<ts>/
docs/iter14A_p2_xhost_write_self_inval_<ts>/
docs/iter14A_p3_remaining_preflight_<ts>/
docs/iter14A_p3b_history_replot/
docs/iter14A_p3c_probe_overhead/
docs/iter14A_p3d_pathdecomp_spec.md
docs/iter14A_p4_production_pathdecomp_<ts>/
docs/iter14A_p5_attribution_<ts>/
docs/iter14A_p6_microbench_<ts>/
docs/g34_scaling_ycsb_iter14A_p7_<ts>/    # follows scaling_ycsb_spec §6
docs/iter14A_p8_tls_research_<ts>/
docs/iter14A_anomaly_triggers.md
```

---

## Commit prefix convention

| Phase | Prefix |
|---|---|
| P1 | `[iter14A-p1-preflight]` |
| P2 | `[iter14A-p2-xhost-self-inval]` |
| P3 | `[iter14A-p3-preflight]` |
| P4 | `[iter14A-p4-pathdecomp]` / `[iter14A-p4-fix-Fxx]` |
| P5 | `[iter14A-p5-attribution]` |
| P6 | `[iter14A-p6-microbench]` |
| P7 | `[iter14A-p7-sweep]` |
| P8 | `[iter14A-p8-tls-research]` |
| P9 | `[iter14A-p9-summary]` |
| Stage spec re-stage | `[iter14A-restage]` |
| Living docs sync | `[iter14A-docs]` |

Every commit references `G6` and/or relevant invariant/anti-pattern
(I3/I9/I11/AP15/AP16/C8/etc) per H4 hook.

---

## Open questions

All resolved. iter-14A enters autonomous mode 2026-05-18.
