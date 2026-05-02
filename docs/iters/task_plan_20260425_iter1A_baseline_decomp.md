# Task plan — iter-1A — Protocol A baseline + decomp + per-host-ring optimization

**Author**: Claude
**Date drafted**: 2026-04-25 (v1 = diagnostic-only; v2 expands to
include Solution 1 + Solution 2 implementation per user
2026-04-25 confirmation)
**Status**: DRAFT v2 — Q QA1 reopened with hybrid scope
**Branch**: `feat/cxl-migration` (commit prefixes
`[iter1A-decomp]`, `[iter1A-baseline]`, `[iter1A-perhost]`,
`[iter1A-compress]`)

---

## Open design questions for user (please decide before Phase 1)

| # | Question | Claude default | User position |
|---|----------|----------------|---------------|
| QA1 | Should iter-1A include any **optimization** code, or only baseline + diagnostic? | **v1 = Diagnostic only**. **v2 (revised) = Diagnose first, then implement Solutions 1 + 2** per user direction 2026-04-25. The architectural argument for the per-host-ring transformation (broadcast traffic 41-81 GB/s vs Layer-2 ceiling 12.5 GB/s, see methodology §9.1) is strong enough to commit to within iter-1A. The decomp Phase 3 acts as a **GO/NO-GO checkpoint**: if it confirms broadcast traffic dominates the write path, proceed to Solutions 1+2 (Phases 5–7); if it surprises us with a different dominant stage, halt iter-1A as diagnostic-only and write a revised hypothesis for iter-2A. | ✅ user-revised to hybrid |
| QA2 | Re-enable A at T ≥ 64? Also: B uses the same `PendingRingMatrix` structure as A (share line `rings_->rings[host_id_][dst]`); B's high-T behaviour was never measured (4/22 sweep capped both at T=16). Test B too in iter-1A baseline? | **Yes for A** with `TIMEOUT_S=600 A_SKIP_AT=999`; **also baseline B** — same N²-ring concern, no data above T=16 to date. Run **both A and B** in iter-1A baseline, let timeouts naturally mark FAIL — that failure pattern itself is diagnostic. | ✅ user-confirmed B inclusion |
| QA3 | Run A (and B) under cache=on AND cache=off? | **Both**. cache=off shows raw broadcast cost; cache=on shows whether DRAM cache hides any of it. | ✅ user decided |
| QA4 | Sweep matrix size for the baseline (CORRECTED) | **160 cells theoretical** (5 workloads × 8 thread counts × 2 cache modes × 2 protocols A and B). Expected effective OK count ~60–100; the rest will be FAIL (timeout / ring-exhaustion) and that **failure pattern itself is diagnostic data**. NOT shrunk to 48/60 pre-emptively per methodology §6.7 (don't pre-cut data). FAIL cells get a dedicated table in iter-1A summary showing where each protocol's structural ceiling lands. No vsize sweep — A/B don't support varlen. | ✅ user-corrected math |
| QA5 | Pre-port iter-3-style optimizations to A (per-slot LFM, route-seq, etc.)? "Pre-port" = take a C optimization that worked, copy the analogous code to A **before** decomp data shows it's needed. | **No** for the iter-3-style protocol-internal opts (per-slot LFM, route-seq, etc.) — those targeted C's lock + epoch-bump bottleneck. **Yes** for **architectural building blocks**: the iter-5 V2 MPSC ring code (`src/cxl_batch_ring.{h,cc}`) and Phase 4 `DramInvalQueue` are reused intact in Solution 1 — that is reuse of mechanism, not "pre-port of unverified optimization". | ✅ user decided |
| QA6 | Iter-1A deadline + autonomy mode | undecided; v2 scope is now **8–11 days** (was 5–6 d diagnostic-only): +5 d for Solution 1 implementation + correctness battery, +1.5 d for Solution 2 entry compression, +0.5 d for re-sweep, +0.5 d for revised summary. **Strongly recommend overnight chained sweeps** at Phase 7 to fit a sane wall-clock window. | ⏳ |

Sections 2–8 below assume defaults are accepted.

---

## 1. Motivation

Five iters of C work closed with B & F passing the 20 Mops/s bar
and A at 0.97 × of bar with structural diagnosis. Per
`docs/iters/protocol_c_retrospective.md` § "Lessons", further
C-side gains compete for opportunity cost with **opening
Protocol A optimization** which has not received any iteration
to date.

**Where A stands now** (from `docs/g34_scaling_ycsb_20260422_162908/`,
the last full A+B+C sweep, with current ABC throughput-improvement
plan up to Phase 5 hierarchical):

| workload | A peak (Mops/s) | T at peak | bar | gap |
|----------|----------------:|----------:|----:|----:|
| A | **0.27** | 2 | 20 | **74 ×** |
| B | 1.80 | 4 | 20 | 11 × |
| C | 18.10 | 16 | 20 | 1.1 × |
| D | 2.33 | 16 | n/a | — |
| F | 0.39 | 2 | 20 | 51 × |

A is **74 × short** on workload A and **51 × short** on F. C is
already close (18.10). The numbers above are not directly
comparable to iter-5 C numbers because:
1. They predate the iter-3 phase-3 micro-batching, the iter-4
   variable-KV plumbing, the iter-5 multi-flusher V2.
2. Sweep used `A_SKIP_AT=64` so high-T behaviour for A is
   unknown — A may peak earlier than C does.
3. Different `MAX_OPS=200000` window may not be steady-state for
   A's slow path.

**The first thing iter-1A must do is establish a clean current
baseline** under the same harness as C iter-5 so all subsequent
A iters can compare against an authoritative number.

## 2. Scope

### 2.1 In-scope (iter-1A)

- **Baseline sweep — A AND B**: `scripts/run_g34_scaling_sweep.sh`
  with `OPTS="A B"`, full T range 1..86, cache=on/off,
  `TIMEOUT_S=600 A_SKIP_AT=999`. **160 cells theoretical**;
  expected ~60–100 OK + remainder FAIL — record both. The FAIL
  pattern (which protocol, what T, which workload) is itself
  data: tells us where each protocol's structural ceiling sits.
- **Latency-decomp instrumentation for A** (B added if Phase-1
  budget allows; otherwise B decomp goes to iter-2A): add the
  `FUSEE_LATENCY_DECOMP=1` pattern (same gate as C) to
  `src/cxl_kv_ops_A.cc` covering at least these stages of the
  write path:
  - `S1 lock_acquire` (LFM bucket lock)
  - `S2 local_apply` (slot scan + write to local DRAM bucket)
  - `S3 broadcast` (push to N-1 peer rings + same-host DRAM queues)
  - `S4 ack_wait` (wait for `kAckQuorum` ACKs)
  - `S5 epoch_bump_or_release` (whatever publishes the write)
- **Decomp run for A** at 1–2 representative cells (workload A
  T=4 cache=on; workload F T=4 cache=on; both well below the
  expected FAIL threshold so we get clean numbers). Output:
  `docs/iters/latency_decomp_A_iter1_<date>.md` with per-stage
  p50/p99 table.
- **Hot-path reading**: study `src/cxl_kv_ops_A.cc` end-to-end
  and write a 1-page "A's write path explained" appendix into
  the latency-decomp doc — same depth as iter-3 phase-1 LFM
  anatomy provided for C. Future Claude / contributor needs this.
  Also briefly characterise B's write path (1 paragraph) in the
  iter-1A summary noting its similarities/differences vs A.
- **Ceiling classification**: compute Layer-3 utilisation per the
  methodology §5 stack. **Add a 4th classification for protocols
  A and B**: "ring-exhaustion-bound" (PendingRingMatrix size or
  N² coordination cost is the binding constraint, not BW or
  latency). The methodology stack assumes BW vs latency; A/B
  introduce a third axis that the iter-1A summary documents
  formally so future iters use it.

### 2.2 In-scope (iter-1A v2 — added Solutions 1 & 2)

Implementation of the per-host-ring transformation, applied to
**both A and B** (they share the `PendingRingMatrix` structure,
both benefit from the same change):

- **Solution 1 — Per-host MPSC outgoing ring + replicator
  dispatch via DramInvalQueue**:
  - Replace `rings[total_workers][total_workers]` with
    `rings[num_hosts][num_hosts]`.
  - Per-host outgoing ring: MPSC (N producer clients, 1 logical
    consumer = the cross-host write side). **Reuse
    `src/cxl_batch_ring.{h,cc}` MPSC code from iter-5 V2.**
  - Cross-host transfer: 1 cacheline per (src_host, dst_host)
    per UPDATE.
  - Receiver side: replicator reads incoming ring, applies update
    locally, dispatches invalidation to local clients via existing
    `DramInvalQueue` (Phase 4). **Reuse
    `src/cxl_same_host_queue.{h,cc}` intact.**
  - Per-host ACK aggregation: replicator sends 1 cross-host ACK
    after all local DramInvalQueue pushes complete; writer client
    waits for H-1 host-level ACKs (instead of N-1 client-level).
  - Apply to both `src/cxl_kv_ops_A.cc` and `src/cxl_kv_ops_B.cc`
    (same change site; B doesn't wait for ACK so simpler).
- **Solution 2 — Compress `PendingRingEntry`**:
  - Current: ~64 B (one cacheline) per entry.
  - Compressed: ~32 B (half cacheline) — pack `op_id` flags into
    high bits, narrow `bucket_idx` to u32, narrow `key` field if
    ABI permits.
  - Apply ONLY after Solution 1 is in place and stable; reduces
    cross-host CXL bytes per UPDATE by ~2×.

### 2.3 Out-of-scope (iter-1A)

- **Other architectural changes** to A/B beyond Solutions 1+2
  (e.g. async outbox / B → C convergence / tree replication for
  H ≥ 4) — listed in iter-2A candidates if needed.
- Variable KV value-size for A or B (iter-4-style work for A/B
  would be its own iter; out of scope here).
- Crash recovery extension to per-host ACK semantics — A's
  recovery currently scans per-client ring; per-host ring changes
  the recovery state shape. **In scope: verify existing
  crash-recover-test still passes; out of scope: redesign recovery
  for host-level ACK.** If existing test fails post-Solution-1,
  iter-1A halts and a recovery-redesign sub-iter is queued.
- M-series microbenches specific to A/B. The C iter-5 M1 is
  reusable as the BW ceiling. A-specific microbenches (e.g.
  standalone ACK round-trip latency, standalone PendingRing
  contention bench) are deferred to iter-2A if needed.
- Full B-side latency decomp instrumentation. B baseline sweep
  is in scope; B decomp is best-effort only — if Phase 1
  instrumentation effort fits, do B; otherwise B decomp rolls
  into iter-2A.

## 3. Phased breakdown

| # | Phase | Prefix | Deliverable | Est | Verify |
|---|-------|--------|-------------|----:|--------|
| 0 | Plan review | — | This doc (v2) approved | — | user sign-off |
| 1 | A latency-decomp instrumentation | `[iter1A-decomp]` | `src/cxl_kv_ops_A.cc` gated probes for S1–S5 | 0.5 d | builds with `-DFUSEE_LATENCY_DECOMP=1`, smoke run prints per-stage histogram |
| 2 | A + B baseline sweep | `[iter1A-baseline]` | sweep dirs, 160 cells theoretical | 1.0 d | sweep completes; OK + FAIL counts recorded; FAIL pattern noted |
| 3 | A decomp run + diagnostic writeup | `[iter1A-decomp]` | `docs/iters/latency_decomp_A_iter1_<date>.md` with per-stage table at workload A T=4 cache=on + workload F T=4 cache=on + 1-paragraph B-vs-A write-path comparison | 1.0 d | dominant stage identified quantitatively; "A write path explained" appendix added; B's analogous structure noted |
| **4** | **GO/NO-GO checkpoint** | — | Decomp data confirms broadcast traffic is the dominant write-path stage at high T (S3 in stage instrumentation). If YES → proceed to Phase 5. If NO → halt iter-1A here as diagnostic-only; revised hypothesis goes to iter-2A. | 0 d | brief written justification, in iter-1A summary |
| **5** | **Solution 1 — Per-host MPSC ring + replicator dispatch** | `[iter1A-perhost]` | `src/cxl_batch_ring.{h,cc}` reused as outgoing aggregator; `src/cxl_kv_ops_A.cc` + `src/cxl_kv_ops_B.cc` swap `rings[total_workers][total_workers]` for `rings[num_hosts][num_hosts]`; replicator dispatches via existing `DramInvalQueue`; per-host ACK aggregation for A. **Methodology §9.1 "Aggregate-before-CXL" pattern.** | 5 d | unit test: T=64 / T=86 cells that previously FAIL'd now complete OK; cross-host CXL traffic (per M1-style measurement) drops ~60×; correctness via crash-recover-test + 2-host consistency spot-check |
| **6** | **Solution 2 — `PendingRingEntry` compression** | `[iter1A-compress]` | Compress entry from ~64 B to ~32 B (pack op_id flags + narrow bucket_idx) | 1.5 d | builds; sweep at 1 cell shows ~2× reduction in cross-host CXL bytes/op |
| 7 | Re-sweep with Solutions 1+2 | `[iter1A-perhost]` | Same matrix as Phase 2 (160 cells, A + B × 5 wl × 8 T × 2 cache); chained overnight | 0.5 d | OK count rises substantially (T=64/86 cells now succeed); SUMMARY peaks recorded |
| 8 | iter-1A revised summary + iter-2A candidates | `[iter1A-perhost]` | `docs/iters/iter1A_baseline_summary_<date>.md` — pre-vs-post Solution-1+2 peak table; gap-to-bar table; ring-exhaustion-bound classification added to methodology stack (alongside latency-bound and BW-bound); **3 candidate hypotheses for iter-2A** with expected gain ranges and risks | 0.5 d | all (workload, T) cells classified pre and post; iter-2A directions ranked |
| 9 | progress.md tail + runs_index + memory digest | `[iter1A-perhost]` | append per CONTRIBUTING §1 | 0.25 d | three index updates landed |

**Total v2: ~10 d.** v1 (diagnostic-only) was 3.5 d; +5 d
Solution 1 + 1.5 d Solution 2 + 0.5 d resweep + extra 0.25 d
summary delta. **Buffer to 13 d** for instrumentation debug /
crash-recover-test surprise / per-host ring correctness battery
issues.

## 4. Files touched

### 4.1 NEW

- `docs/iters/latency_decomp_A_iter1_<date>.md` — diagnostic
  output, similar shape to
  `docs/iters/latency_decomp_C_iter1_20260423_050303.md`.
- `docs/iters/iter1A_baseline_summary_<date>.md` — iter summary
  per methodology §7.

### 4.2 MODIFIED — Phase 1 (decomp instrumentation)

- `src/cxl_kv_ops_A.cc` — add `FUSEE_LATENCY_DECOMP`-gated probes
  (additive; default-build behaviour unchanged). ~80 LoC added.
- `scripts/run_g34_scaling_sweep.sh` — change `A_SKIP_AT` default
  from 64 to 999 (or pass `A_SKIP_AT=999 TIMEOUT_S=600` per-run).

### 4.3 MODIFIED — Phases 5–6 (Solutions 1+2; only if Phase 4 GO)

- `src/cxl_kv_ops_A.cc` (~150 LoC delta):
  - `PendingRingMatrix` typedef change: `[total_workers]` →
    `[num_hosts]` for the rings array; `local_tail_` and
    `ack_timeouts_` arrays move to per-host indexing.
  - Replace per-client outgoing path with `cxl_batch_ring`
    enqueue (one MPSC per dst-host, host-internal aggregator
    drains and writes the cross-host CXL ring).
  - Replicator: dispatch incoming entries to local clients via
    existing `DramInvalQueue`.
  - ACK path: receive per-host ACKs (H-1 instead of N-1); writer
    waits accordingly.
- `src/cxl_kv_ops_B.cc` (~80 LoC delta):
  - Same `PendingRingMatrix` typedef change. B has no ACK so
    simpler; just replicator + DramInvalQueue dispatch.
- `src/cxl_kv_ops_A.h` / `src/cxl_kv_ops_B.h` — adjust array
  sizes from `kMaxHosts` (= total_workers ceiling) to
  `kMaxPhysicalHosts` (typically 4) where appropriate.
- `src/kv_utils.h` (or wherever `PendingRingEntry` lives):
  Phase 6 entry compression — ~30 LoC delta. Field layout change
  is **breaking** for any consumer that includes the header;
  audit needed before commit.

### 4.4 NOT TOUCHED

- `src/cxl_kv_ops_C.{h,cc}` — C is closed for this iter.
- `src/cxl_batch_ring.{h,cc}` (iter-5 V2 MPSC) — **reused intact
  as the outgoing aggregator data structure**.
- `src/cxl_same_host_queue.{h,cc}` (`DramInvalQueue`, Phase 4)
  — **reused intact for replicator → local client dispatch**.
- `tests/cxl_dualhost_bw_bench.cc` — M1 microbench reused for
  ceiling reference.
- All `docs/refs/*.md` except `optimization_methodology.md` (added
  §9 design pattern).

## 5. Verification plan

### 5.1 Phase-1 instrumentation correctness

Build A with `-DFUSEE_LATENCY_DECOMP=1`, run a 100-op smoke run,
verify:
- All 5 stages produce non-zero counts.
- Sum of per-stage means ≈ total `w_avg_ns` (within 10 %).
- Same build with `-DFUSEE_LATENCY_DECOMP=0` is byte-for-byte
  identical to current (regression check via `cmp` on stripped
  binary).

### 5.2 Phase-2 baseline reproducibility

Run the baseline sweep twice on consecutive nights; the two A
peaks per workload should agree to ±10 % (per methodology §6.3
the variance discipline). If > 10 % drift, investigate before
proceeding to Phase 3.

### 5.3 Phase-3 latency-decomp self-consistency

The dominant stage's p50 should account for ≥ 50 % of the total
`w_p50_ns`. If no single stage dominates, the decomp is
mis-instrumented or A's bottleneck is elsewhere (e.g., scheduling,
fence cost not captured) — iter-1A scope expands to investigate.

## 6. Success criteria (v2)

### 6.1 Phase-1–4 (diagnostic-only) success — required to proceed

1. A + B baseline sweep runs all 160 cells (OK or FAIL); FAIL
   pattern recorded.
2. Per-stage decomp at workload A T=4 cache=on identifies a
   dominant stage (≥ 50 % of total `w_p50`). Document the stage.
3. Phase-4 GO/NO-GO checkpoint reached: data confirms (or does
   not confirm) that broadcast traffic dominates the write path.

### 6.2 Phase-5–8 (Solutions 1+2) success — quantitative targets

If Phase 4 GO, additionally:

4. **Solution 1 correctness**: `crash-recover-test/` passes;
   2-host consistency spot-check on workload B at T=64 (pre-
   Solution-1 cells that timed out) now completes OK; per-bucket
   final state matches the per-client-ring run on cells where
   both succeeded.
5. **Solution 1 ceiling progress**: T=64 / T=86 cells that
   previously FAIL'd (timeout) now successfully complete on at
   least one workload. **No specific Mops/s target in iter-1A**
   — the goal is "structural unblock", not "hit 20 Mops/s on A".
   The numeric target is iter-2A.
6. **Solution 1 broadcast-traffic reduction validated**: M1-style
   measurement (or perf uncore counter) confirms cross-host CXL
   write traffic per UPDATE drops by ≥ 30× (target 60×, give
   ourselves 50% margin).
7. **Solution 2 entry compression validated**: per-UPDATE cross-host
   CXL bytes drop ~2× vs Solution-1-only run (kept controlled
   alongside Solution 1 baseline).
8. **No regression** on cells that passed pre-Solution-1: workload
   A/B at T ≤ 16 cache=on/off does not drop more than 10 % vs
   the pre-Solution-1 baseline from Phase 2.
9. iter-1A summary lists **at least 3 candidate hypotheses for
   iter-2A** with target stage, expected gain range, effort
   estimate, risk — including which workloads still miss 20 Mops/s
   and what the next bottleneck is.
10. All 9 deliverables + 2 index updates exist (methodology §7).

### 6.3 If Phase 4 NO-GO

Phases 5–8 are skipped; iter-1A summary documents the unexpected
diagnostic finding and proposes a revised Solution-1-equivalent
for iter-2A. iter-1A still ships diagnostic-only deliverables
(items 1–3 + revised Phase-8 candidate hypotheses).

## 7. Risk analysis

| Risk | Likelihood | Mitigation |
|---|---|---|
| Instrumentation perturbs measurement (stages add 100s ns each) | Med | Standard mitigation: also report `w_avg_ns` on `-DFUSEE_LATENCY_DECOMP=0` build for the same cell, compare overhead |
| Sweep with `A_SKIP_AT=999` times out at T=86 | High | TIMEOUT_S=600 (10 min) is 6 × the default. If still hits, accept partial and document the cells that timed out — that IS the diagnostic data showing where structural ceiling lands |
| A baseline drifts > 10 % from 4/22 numbers (not iter-3+ updates) | Med | Document and accept; the new baseline is the authoritative one going forward |
| Same-host bypass code (Phase 4) not exercised at small num_clients_per_host | Low | Two-host setup with T = num_clients/host always exercises both paths at T ≥ 2 |
| A's `replicator_loop` thread CPU cost shows up as latency at some other stage's accounting | Low-Med | Decomp's S5 (or a new S6) explicitly captures replicator CPU time per op via `getrusage(RUSAGE_THREAD)` if needed |
| **Phase 4 GO/NO-GO surprises us — broadcast is NOT the dominant stage** | Low (architectural argument is strong) | Halt at Phase 4; iter-1A ships as diagnostic-only; revised hypothesis goes to iter-2A. **Documented in §6.3.** |
| **Solution 1 ACK semantics break consensus correctness on A** (host-level ACK vs client-level ACK) | Med | Explicit consensus argument written before Phase 5 commit: "host-level ACK is sufficient because consensus invariant is `host has applied write to local hash table`, not `every client on the host has invalidated its DRAM cache`. The latter is achieved via DramInvalQueue but is not on the consensus critical path." Reviewed against `docs/refs/consensus_transformation_explained.md` |
| **Crash recovery breaks** under per-host ACK semantics | Med-High | Phase 5 verify gate explicitly runs `crash-recover-test/`. If fails → halt iter-1A, queue a recovery-redesign sub-iter before resuming Solutions 1+2 |
| **Solution 1 introduces new MPSC contention bottleneck** at the per-host outgoing aggregator (64 producers on one ring) | Med | iter-5 V2 already proved this MPSC pattern works for C's dirty queue. Reuse `cxl_batch_ring.{h,cc}` intact. If contention is observed in re-sweep, fall back to N=2 sub-shards within the per-host ring (mirror iter-5 multi-flusher) |
| **Solution 2 entry compression breaks ABI for crash-recover-test or other consumers** | Low-Med | Audit all `PendingRingEntry` includes before Phase 6 commit. Crash-recover serialises entries to OpLog with explicit field width — needs adjustment |

## 8. iter-2A teaser (after iter-1A Solutions 1+2 land)

iter-1A v2 already lands the architecturally-justified Solutions
1 and 2. iter-2A picks up from whatever bottleneck **emerges
after** broadcast traffic is removed. Best-guess pre-data
candidates:

a. **ACK round-trip latency** becomes dominant for A: per-host
   ACK is now H-1 = 1 (for 2 hosts) but each ACK still costs
   1 CXL roundtrip ~1.2 µs minimum. If A misses 20 Mops/s after
   Solution 1, this is the next stage. Mitigation: (i) async ACK
   (writer doesn't wait, polls in background) — changes A's
   strict consensus to relaxed; (ii) batched ACK — ACK
   aggregates K writes.
b. **Replicator scan / dispatch cost** becomes dominant for B:
   replicator now reads H-1 = 1 ring per host (instead of N-1)
   but still has to dispatch to N local clients via DramInvalQueue.
   Multi-replicator V2 (mirroring iter-5 multi-flusher) splits
   the dispatch across multiple replicator threads.
c. **Per-slot LFM lock** if Zipf hot-bucket contention shows up
   on A/B (mirror C iter-1 win). Most likely needed for workload
   A's hot bucket.
d. **Variable KV value-size for A/B**: align A/B with iter-4 C
   work; required if YCSB realistic value sizes (≥ 256 B) are in
   scope for the project.
e. **B → C convergence**: if Solution 1 makes B's broadcast cheap
   enough that B's only remaining differentiator (synchronous
   invalidation) provides little value over C's epoch-bump, B
   could be deprecated. Open question for whoever picks up iter-2A.

These are guesses. **iter-1A's Phase 8 summary replaces the
guesses with data-driven ranking.**

---

## Appendix — methodology cross-reference

This plan implements `docs/refs/optimization_methodology.md`:

- §1.2 Diagnose before optimizing → Phase 1+3 GO/NO-GO before
  Solutions 1+2 (Phase 4 checkpoint)
- §1.5 Cite ceiling layer → §6 Layer-3 classification
- §2 Per-iter loop → Phase 0..9 above (extended for v2 hybrid)
- §3.2/3.3 Latency decomp + lock anatomy → Phase 1+3
- §4 Hypothesis discipline → Phase 4 GO/NO-GO + §6.2 measurable
  Solution 1 / 2 success criteria
- §6.5 No compounding → Solutions 1 and 2 split into separate
  phases (5 and 6) with separate verify gates so we can bisect
  later if needed
- §7 Document trail → Phase 5/8/9 deliverables checklist
- **§9.1 "Aggregate-before-CXL" pattern** → Solution 1 is the
  second documented application of this pattern (first was iter-5
  multi-flusher V2 for protocol C); Phase 5 commit message should
  explicitly cite this
