# Task plan — iter-2A — Solution-1 wire-up + Solution-2 compression + A full sweep

**Author**: Claude
**Date drafted**: 2026-04-26
**Status**: DRAFT v1 — awaiting user review
**Branch**: `feat/cxl-migration` (commit prefixes
`[iter2A-wire]`, `[iter2A-bwire]`, `[iter2A-compress]`,
`[iter2A-sweep]`, `[iter2A-decomp]`)

---

## Open design questions for user (please decide before Phase 1)

| # | Question | Claude default | User position |
|---|----------|----------------|---------------|
| QA1 | A-side and B-side dispatch/replicator rewire — same commit or split? | **Split** per methodology §6.5 no-compounding: A in `[iter2A-wire]`, B in `[iter2A-bwire]`. Each gets its own smoke gate before moving to the next. Bisect-friendly if a regression appears later. | ⏳ |
| QA2 | Solution 2 entry compression — same iter as Solution 1 wire-up, or split to iter-3A? | **Same iter, separate phase** (Phase 6). Cleanest test: re-sweep after Solution 1, then re-sweep after Solution 2 — two data points, isolates each contribution. Per user 2026-04-26: "实现Solution-2 entry 压缩 32B → 16B" is in scope. | ✅ user-included |
| QA3 | Default of `FUSEE_PER_HOST_RING` after iter-2A validation — stay opt-in (=0) or flip to default-on (=1)? | **Stay opt-in (=0) for in-tree default** for the duration of iter-2A. Sweeps explicitly pass `FUSEE_PER_HOST_RING=1` in env. Flip to default-on only after iter-3A wider validation (e.g. crash-recover-test under N=128). | ⏳ |
| QA4 | B-side validation — full sweep, or smoke + correctness only? | **Smoke + correctness only**. User direction "跑协议A only的完整scaling_ycsb实验" means the perf sweep is A-only. B's rewire is correctness-validated (smoke + crash-recover-test) but no dedicated B perf sweep this iter. **B baseline + B perf sweep is iter-3A or beyond.** | ✅ user-confirmed A-only sweep |
| QA5 | Decomp re-run cells — just workload A T=4 cache=on (apples-to-apples vs iter-1A), or also T=16 + T=64 to see how stage breakdown shifts with T after rewire? | **All three: T=4, T=16, T=64**. iter-1A T=64 was near-collapse (workload A 0.01 Mops/s); post-wire T=64 should run cleanly and the decomp will reveal whether S4 ack_wait scaling is now flat (host-level ACK = H-1 = 1) or some new stage emerges. T=16 fills the middle. T=4 is the regression check. | ⏳ |
| QA6 | Iter-2A deadline + autonomy mode | undecided; **estimate 10–12 days**: 3 d for A wire + correctness, 2 d for B wire + correctness, 1 d for A sweep + 1 d decomp at 3 cells, 1 d for Solution 2 compression, 1 d for re-sweep + decomp, 1.5 d for summary + plots. **Recommend overnight chained sweeps for Phases 6 + 9.** | ⏳ |

Sections 2–9 below assume QA1/QA3/QA5 defaults are accepted.

---

## 1. Motivation

iter-1A landed Phase 1–5a (instrumentation + baseline + decomp +
Solution-1 data structures, opt-in env). The data structures are
**inert** — `dispatch_and_wait()` and `replicator_loop()` still
take the legacy `PendingRingMatrix` per-client-pair broadcast path.
Per `docs/iters/iter1A_baseline_summary_20260426.md`:

- A peak: **0.54 Mops/s** at workload A T=4 cache=on; bar 20 Mops/s,
  37 × short.
- Decomp at workload A T=4 cache=on: S3 broadcast = 26 % of
  latency, S4 ack_wait = 48 % → **S3+S4 = 74 % of write path**.
- T=64 cells: workload A near-collapse (0.01 Mops/s), workload B/C/F
  FAIL — exact prediction of broadcast BW saturation.

Solution 1 (per-host MPSC ring + replicator dispatch via
DramInvalQueue) directly attacks S3 (broadcast traffic from O(N)
per UPDATE → O(H)) and S4 (ACK from N-1 client-level → H-1 host-level).
Methodology §9.1 "Aggregate-before-CXL" pattern; `PerHostOutMatrix`
already 330 × smaller than legacy `PendingRingMatrix`.

iter-2A's job: **wire it up, validate correctness, run the perf
re-sweep, quantify the actual gain vs iter-1A's predicted 2.8 ×**
(at workload A T=4) and significantly larger gain at high T where
the legacy path collapses.

After Solution 1 lands, **Solution 2 entry compression** (32 B →
16 B per `PerHostOutEntry`) is a low-risk follow-on that further
halves cross-host CXL traffic per UPDATE.

---

## 2. Scope

### 2.1 In-scope (iter-2A)

#### Solution 1 — dispatch + replicator rewire (A and B)
- **A-side** (`src/cxl_kv_ops_A.cc`):
  - Modify `dispatch_and_wait()`: when `FUSEE_PER_HOST_RING=1`,
    instead of pushing per-client `PendingRingEntry` to N-1 dst
    rings, push **one** `PerHostOutEntry` to the host-level
    aggregator MPSC ring `per_host_rings_->rings[my_host_][dst_host]`
    via `atomic fetch_add` tail. Wait for H-1 = 1 host-level ACK
    (for H=2) instead of N-1 client-level ACKs.
  - Modify `replicator_loop()`: when `FUSEE_PER_HOST_RING=1`, read
    from `per_host_rings_->rings[*][my_host_]` (single-consumer
    side of MPSC); apply update locally; dispatch invalidation to
    each local client via existing `DramInvalQueue` (Phase 4
    same-host bypass mechanism); publish `r.ack_seq` once all
    local DramInvalQueue pushes complete. Estimated ~80 LoC delta
    on dispatch + ~70 LoC on replicator.
- **B-side** (`src/cxl_kv_ops_B.cc`):
  - Same `PerHostOutMatrix` allocation as A (mirror Phase 5a
    `bytes_for()` + `attach()` plumbing).
  - Same dispatch rewire — but B doesn't wait for ACK (fire-and-forget),
    so the dispatch path is simpler.
  - Replicator rewire mirrors A but skips the `ack_seq` publish.
  - Estimated ~80 LoC delta total.

#### Solution 2 — `PerHostOutEntry` compression (32 B → 16 B)
- `src/cxl_per_host_ring.h`:
  - Pack op_id high bits with flag bits (saves 4–8 B).
  - Narrow `bucket_idx` to `uint32_t` (num_buckets ≤ 65536 currently;
    ≤ 4G will always hold).
  - Narrow `key_lo` / `key_hi` split or use a single `uint32_t key_idx`
    + reconstruction from bucket_idx + slot.
  - Verify final layout = 16 B (quarter-cacheline) — 4 entries fit
    in one CXL cacheline write.
- Adjust `dispatch_and_wait` + `replicator_loop` field accesses
  (~30 LoC delta).
- ABI break: any consumer of `PerHostOutEntry` outside the rewire
  needs audit. (Currently: only A.cc and B.cc; iter-2A's own
  changes.)

#### A-only full scaling_ycsb sweep
- Per `docs/scaling_ycsb_spec.md` §3 — full matrix:
  - opts: A only (`OPTS=A`)
  - workloads: a, b, c, d, f (5)
  - threads/host: 1, 2, 4, 8, 16, 32, 64, 86 (8)
  - cache modes: on, off (2)
  - **Total: 80 cells** (no protocols B / C in this sweep)
- Two sweeps end-to-end:
  - **Sweep #1 — Solution 1 only** (`FUSEE_PER_HOST_RING=1`,
    Solution 2 not yet landed, entry still 32 B). Measures
    Solution-1 isolated contribution.
  - **Sweep #2 — Solution 1 + Solution 2** (entry compressed
    16 B). Measures Solution-2's marginal contribution on top.
- Both sweeps with `TIMEOUT_S=600 A_SKIP_AT=999` (lift the legacy
  T-cap; we expect post-rewire that the cap is no longer needed).

#### Per-cell latency decomp at multiple T
- Re-run `tests/cxl_latency_decomp_A` (instrumented from iter-1A
  Phase 1) at:
  - workload A T=4 cache=on (apples-to-apples vs iter-1A 36.30 µs
    baseline)
  - workload A T=16 cache=on (mid-range)
  - workload A T=64 cache=on (the cell that previously near-collapsed)
- Three cells × 5 stages = 15-row table in iter-2A summary.
- Computes per-stage delta vs iter-1A's 36.30 µs decomp (which was
  T=4 only).

### 2.2 Out-of-scope (iter-2A)

- **B perf sweep**: B's rewire is correctness-validated (smoke
  + crash-recover-test) but no dedicated B perf sweep. B baseline
  goes to iter-3A.
- **Default-on for `FUSEE_PER_HOST_RING`**: stays opt-in this iter.
  iter-3A may flip after wider validation.
- **OpLog format change for per-host ACK**: per-host ACK is on the
  consensus path; OpLog still records per-bucket transitions
  (host-agnostic), so OpLog format unchanged. **Verify**:
  crash-recover-test passes with `FUSEE_PER_HOST_RING=1`.
- **Variable-KV value-size for A** (iter-4-style work). Out of
  scope; A's current inline-u64 path is the substrate iter-2A wires.
- **Hierarchical / tree replication for H ≥ 4**. Solution 1 is
  the H × H mesh layer; tree replication is methodology §9.1
  "variant" only worth at H ≥ 8. H=2 here, flat mesh is optimal.
- **PendingRingMatrix removal**. Legacy path stays in tree for
  the duration of iter-2A as the `FUSEE_PER_HOST_RING=0` fallback,
  so we can A/B compare. Removal is iter-3A or later.
- **B ↔ C convergence question**: with broadcast cheap under
  Solution 1, is B's only differentiator (synchronous invalidation)
  worth keeping vs C? Open question; iter-2A ships both. Decide
  in iter-3A teaser.

---

## 3. Phased breakdown

| # | Phase | Prefix | Deliverable | Est | Verify |
|---|-------|--------|-------------|----:|--------|
| 0 | Plan review | — | This doc approved | — | user sign-off |
| 1 | A dispatch + replicator rewire | `[iter2A-wire]` | `cxl_kv_ops_A.cc` ~150 LoC delta + per-host ACK aggregation | 2 d | builds; smoke at T=4 cache=on with `FUSEE_PER_HOST_RING=1` matches legacy result within ±5 % (correctness pre-perf) |
| 2 | A correctness battery | `[iter2A-wire]` | `crash-recover-test/` passes; 2-host consistency spot-check at T=64 (cell that previously FAIL'd) completes OK on at least one workload | 1 d | no consistency violation across 5 runs of 100k ops |
| 3 | B dispatch + replicator rewire | `[iter2A-bwire]` | `cxl_kv_ops_B.cc` ~80 LoC delta; mirror of Phase 1 minus ACK | 1.5 d | smoke at T=4 cache=on matches legacy ±5 %; crash-recover-test passes |
| 4 | A full scaling_ycsb sweep — Solution 1 only | `[iter2A-sweep]` | 80-cell sweep (A × 5 wl × 8 T × 2 cache); chained overnight; output `logs/g34_scaling_sweep_A_only_iter2A_s1_<ts>/` | 0.5 d (chained overnight) | OK count rises substantially from iter-1A baseline (32/40 → ≥ 70/80 expected); SUMMARY peaks recorded |
| 5 | A decomp re-run at 3 cells (post-Solution-1) | `[iter2A-decomp]` | `docs/iters/latency_decomp_A_iter2A_<date>.md` — per-stage table at workload A T={4, 16, 64} cache=on; comparison vs iter-1A's 36.30 µs T=4 baseline | 1 d | dominant stage shifted (S3+S4 should drop substantially); new dominant stage identified for iter-3A target |
| 6 | Solution 2 — `PerHostOutEntry` compression 32 B → 16 B | `[iter2A-compress]` | `src/cxl_per_host_ring.h` field repack; A.cc + B.cc field access updates; sizeof check enforced via `static_assert` | 1 d | builds; smoke at T=4 + T=64 matches Sweep #1 within ±10 % (correctness regression check) |
| 7 | A re-sweep — Solution 1 + Solution 2 | `[iter2A-sweep]` | Same 80-cell matrix; output `logs/g34_scaling_sweep_A_only_iter2A_s12_<ts>/`; chained overnight | 0.5 d | SUMMARY peaks recorded; ≥ Sweep #1 throughput on every (workload, T) cell |
| 8 | A decomp re-run at 3 cells (post-Solution-2) | `[iter2A-decomp]` | Append second decomp table to the same file from Phase 5 | 0.5 d | per-cacheline cost drop visible in S3 stage |
| 9 | Iter-2A summary + plots + index updates | `[iter2A-sweep]` | `docs/iters/iter2A_summary_<date>.md`; **standard scaling_ycsb plot set per spec §6 using Style B** (`docs/tools/plot_style.py`); pre-vs-post Solution 1 + Solution 2 bar chart; gap-to-bar table; iter-3A candidate hypotheses | 1 d | all index updates landed (progress.md tail + runs_index + memory) |

**Total: ~10 d.** Buffer to 13 d for B-side issues +
crash-recover-test surprises + 2 sweeps × overnight wall-clock.

---

## 4. File-by-file change list

### 4.1 NEW

- `docs/iters/latency_decomp_A_iter2A_<date>.md` — decomp tables
  for Phases 5 + 8.
- `docs/iters/iter2A_summary_<date>.md` — iter summary per
  methodology §7.
- `docs/sweeps/g34_scaling_ycsb_A_only_iter2A_s1_<ts>/` — Sweep #1
  finalized deliverable (Style B plots).
- `docs/sweeps/g34_scaling_ycsb_A_only_iter2A_s12_<ts>/` — Sweep #2
  finalized deliverable.
- (raw under `logs/g34_scaling_sweep_A_only_iter2A_*_<ts>/`)

### 4.2 MODIFIED — Phase 1 (A wire-up)

- `src/cxl_kv_ops_A.cc` (~150 LoC delta):
  - `dispatch_and_wait()`: branch on `per_host_rings_enabled_`. New
    path enqueues `PerHostOutEntry` to MPSC ring; waits H-1 ACK.
  - `replicator_loop()`: branch on `per_host_rings_enabled_`. New
    path reads incoming MPSC ring, apply locally, dispatch via
    `DramInvalQueue`, publish per-host ack_seq.
- `src/cxl_kv_ops_A.h` (~10 LoC delta): per-host `local_tail_`,
  per-host `ack_timeouts_`, per-host `last_ack_seen_` arrays
  sized by `kMaxPhysicalHosts`.

### 4.3 MODIFIED — Phase 3 (B wire-up)

- `src/cxl_kv_ops_B.cc` (~80 LoC delta):
  - `bytes_for()` / `attach()` extended same as A's iter-1A Phase
    5a — allocate `PerHostOutMatrix` tail; opt-in env.
  - `dispatch()` analog rewire (no ACK).
  - `replicator_loop()` analog rewire.
- `src/cxl_kv_ops_B.h` (~5 LoC delta).

### 4.4 MODIFIED — Phase 6 (entry compression)

- `src/cxl_per_host_ring.h` (~30 LoC delta):
  - `PerHostOutEntry` field repack to 16 B.
  - `static_assert(sizeof(PerHostOutEntry) == 16)`.
  - Inline accessor helpers if any field needs bit unpacking.
- `src/cxl_kv_ops_A.cc` + `src/cxl_kv_ops_B.cc` (~10 LoC delta
  each): field access updates.

### 4.5 NOT TOUCHED

- `src/cxl_kv_ops_C.{h,cc}` — C is closed.
- `src/cxl_batch_ring.{h,cc}` — iter-5 V2 MPSC code; **reused as
  the conceptual model** for `PerHostOutRing`'s MPSC pattern but
  not a code dependency.
- `src/cxl_same_host_queue.{h,cc}` (`DramInvalQueue`, Phase 4) —
  **reused intact** for replicator → local client dispatch.
- `tests/cxl_dualhost_bw_bench.cc` — M1 microbench reused as
  ceiling reference.
- All `docs/refs/*.md` — refs are stable.

---

## 5. Verification plan

### 5.1 Phase 1 (A wire-up) correctness

- Build A with `FUSEE_PER_HOST_RING=1` AND with `=0`; verify
  default-build behaviour byte-for-byte unchanged via `cmp` on
  stripped binary at `=0`.
- Smoke at workload A T=4 cache=on, 50 k ops:
  - With `FUSEE_PER_HOST_RING=0`: matches iter-1A baseline 0.54
    Mops/s (legacy path, unchanged).
  - With `FUSEE_PER_HOST_RING=1`: completes without hang or
    consistency violation; throughput **≥ legacy** (we expect
    significantly higher; minimum bar is "≥ legacy").
- 2-host consistency spot-check: 100 k ops at T=4 with random
  seed, both hosts dump final hash table state, diff = 0.

### 5.2 Phase 2 (A crash-recover) correctness

- Run `crash-recover-test/` at `FUSEE_PER_HOST_RING=1`. Per scope
  §"Out of scope": OpLog format unchanged because per-host ACK
  is on consensus path, not OpLog path. **If test fails →
  investigate before proceeding to Phase 3 or revisit per-host
  ACK semantics.**

### 5.3 Phase 3 (B wire-up) correctness

- Same battery as Phase 1+2 but for B. B has weaker semantics
  (no ACK), so the consistency check tolerance is "eventual"
  not "immediate" — give the replicator 100 ms to drain after
  the last write before diffing.

### 5.4 Phase 4 (A Sweep #1) — quantitative

- All 80 cells complete (OK or recorded FAIL).
- **Quantitative target**: workload A peak ≥ 10 Mops/s on at
  least one (T, cache) cell. Baseline was 0.54 — 18 × gain
  expected from the bottleneck math (S3+S4 collapse + N→H ACK
  reduction). 10 × is a conservative bar.
- T=64 cells: at least workload A + workload B + workload F
  successfully complete (no timeout). iter-1A had only workload A
  + D succeeding; b/c/f FAIL'd.

### 5.5 Phase 5 (post-Solution-1 decomp) — diagnostic

- New decomp run at 3 cells (T=4/16/64) confirms:
  - S3 broadcast µs has dropped from iter-1A 9.30 (T=4) to ≤ 2.0
    (target: 0.5 µs single DRAM enqueue + sfence).
  - S4 ack_wait µs has dropped from iter-1A 17.57 (T=4) to ≤ 5.0
    (target: ~3 µs single CXL ACK roundtrip).
  - New dominant stage identified (likely S1 lock at high Zipf,
    or S5 unlock + epoch publish — to be discovered).

### 5.6 Phase 6 (Solution 2) correctness

- `static_assert(sizeof(PerHostOutEntry) == 16)` compiles.
- Smoke matches Phase 4 sweep cells within ±10 % (compression
  is a correctness change, not perf change at low T; perf
  benefit shows at high T where CXL bandwidth matters).

### 5.7 Phase 7 (A Sweep #2) — quantitative

- All 80 cells complete; recorded.
- Per-cell throughput **≥ Sweep #1 result** (no regression from
  compression).
- High-T cells (T=64/86): expected modest gain (~1.5 ×) from
  halved per-op CXL bytes. Low-T cells (T≤16): essentially
  unchanged (latency-bound, not BW-bound).

### 5.8 Phase 8 (post-Solution-2 decomp) — diagnostic

- S3 broadcast µs further drop from Phase 5 numbers (per-op
  cacheline write count halved).
- Extrapolate ceiling: at T=86, what fraction of M1 25 GB/s
  aggregate are we using? (Per methodology §5 Layer-3 utilisation.)

---

## 6. Success criteria

iter-2A succeeds when ALL of:

1. Solution 1 wired into A (Phase 1+2) and B (Phase 3) with
   correctness battery passing.
2. A Sweep #1 completes; workload A peak ≥ 10 Mops/s on at
   least one cell **(18 × over iter-1A 0.54 Mops/s baseline)**.
3. Post-Solution-1 decomp confirms S3 + S4 dropped per §5.5
   targets; new dominant stage identified.
4. Solution 2 entry compression landed; smoke + Sweep #2
   correctness intact.
5. A Sweep #2 completes; no per-cell regression vs Sweep #1;
   high-T cells show further gain.
6. **All scaling_ycsb plots produced per `docs/scaling_ycsb_spec.md`
   §7 using Style B** (`docs/tools/plot_style.py` `apply_style()`);
   pre-vs-post bar chart in summary doc.
7. iter-2A summary lists **at least 3 candidate hypotheses for
   iter-3A** with target stage, expected gain, effort, risk.
8. All deliverables + index updates exist (methodology §7).

If criterion 2 fails (workload A < 10 Mops/s):
- Phase 5 decomp diagnoses the unexpected bottleneck → iter-2A
  ships as "Solution 1 wired but partial gain"; iter-3A picks
  up from the new dominant stage.

If Phase 2 (crash-recover) fails:
- Halt iter-2A; queue a recovery-redesign sub-iter; do NOT
  proceed to Phase 3+. Per-host ACK semantics need reconciliation
  with OpLog before any merge.

---

## 7. Risk analysis

| Risk | Likelihood | Mitigation |
|---|---|---|
| MPSC tail contention on the per-host outgoing ring (64 producers fighting one MPSC) | Med | iter-5 V2 `DirtyQueueShard` proved this MPSC pattern works under similar contention (720/720 OK). If observed contention shows in Phase 5 decomp (e.g. S3 µs higher than expected), fall back to per-producer-thread SPSC into a host-level aggregator (extra hop, but eliminates tail-CAS contention). |
| Crash-recover-test breaks under per-host ACK | Med-High | Phase 2 explicit verify gate. Halt iter-2A if fail; reassess. OpLog should NOT need format change because per-host ACK is consensus-path-only. |
| B-side rewire surfaces a B-specific concern not present in A | Med | B's lack of ACK simplifies dispatch — should be strictly easier than A. If smoke fails, bisect via `[iter2A-bwire]` commit alone. |
| Solution 2 16 B field repack truncates necessary state | Low | `static_assert` enforces size; manual audit before commit; smoke at T=4 catches functional issues. |
| A Sweep #1 reveals workload A still misses 20 Mops/s bar | Med | Plan §6 criterion 2 sets bar at 10 Mops/s, not 20 — methodology §4.5 says criteria come from "what's the next material step", not "what we hope". 18 × gain over iter-1A is material progress; 20 Mops/s is iter-3A's job. |
| Cells that legacy path FAIL'd at T=64 still FAIL post-rewire | Low | Solution 1's broadcast traffic reduction is structural (~60 ×); the cells should now run. If they still FAIL, decomp will show the new ceiling. |
| Two separate sweeps × 80 cells × overnight = 16+ hours wall-clock | Med | Chain script per iter-4/5 pattern. Run Sweep #1 evening day-N; analyse during day-N+1; run Sweep #2 evening day-N+1. |

---

## 8. iter-3A teaser (after iter-2A completes)

iter-2A's Phase 5 + 8 decomp identify the new dominant stage.
Pre-data candidates for iter-3A focus:

a. **Replicator dispatch via DramInvalQueue scaling cost**:
   replicator on host B now serially dispatches 63 invalidations
   via DramInvalQueue per cross-host UPDATE. At high cross-host
   UPDATE rate, replicator becomes single-thread bottleneck →
   multi-replicator V2 (mirror iter-5 multi-flusher).
b. **Per-slot LFM lock for A** (mirror C iter-1 win): if Zipf
   hot-bucket contention still dominates after broadcast is fixed,
   per-slot LFM is the next handle.
c. **Async / batched ACK for A**: per-host ACK is 1 round-trip
   ~1.2 µs minimum. If S4 still > 1.5 µs after Solution 1, async
   ACK (writer doesn't wait, polls) is the next optimization.
d. **Variable-KV value-size for A/B** (iter-4-style work): align
   A/B with iter-4 C work; required for realistic value sizes.
e. **B baseline + B perf sweep**: deferred from iter-2A (QA4).
   First task of iter-3A or its own sub-iter.
f. **Default-on for `FUSEE_PER_HOST_RING`** (QA3 deferred): if
   iter-2A wider validation passes, flip default to =1 and remove
   the legacy `PendingRingMatrix` path entirely.

These are guesses. **iter-2A's decomp tables replace the guesses
with data-driven ranking.**

---

## Appendix — methodology cross-reference

This plan implements `docs/refs/optimization_methodology.md`:

- §1.2 Diagnose before optimizing — iter-1A's empirical decomp
  (S3+S4 = 74 %) is the diagnosis; iter-2A is the targeted
  optimization.
- §1.5 Cite ceiling layer — §5.8 explicit Layer-3 utilisation
  computation.
- §2 Per-iter loop — Phase 0..9 above.
- §3.2 Latency decomp — Phases 5 + 8 reuse iter-1A's
  `cxl_latency_decomp_A` instrumentation.
- §4 Hypothesis discipline — primary hypothesis: "Solution 1
  removes S3+S4 = 74 % of write-path latency, lifting workload A
  from 0.54 to ≥ 10 Mops/s." Falsifiable, quantitative.
- §6.5 No compounding — A wire (Phase 1) and B wire (Phase 3)
  separate commits; Solution 1 (Phase 4 sweep) and Solution 2
  (Phase 7 sweep) separate sweeps; one variable per data point.
- §7 Document trail — Phase 9 deliverables checklist.
- **§9.1 "Aggregate-before-CXL"** — Solution 1 is the **third**
  documented application of this pattern. Phase 1 + Phase 3 commit
  messages cite §9.1.
- §6.7 Don't pre-cut data — full 80-cell sweep at A_SKIP_AT=999;
  let timeouts mark FAIL naturally.
- Style B plot standard (CONTRIBUTING + scaling_ycsb_spec §7) —
  Phase 9 every plot uses `from plot_style import apply_style;
  apply_style()`.
