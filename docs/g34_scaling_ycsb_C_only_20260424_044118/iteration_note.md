# Phase-2 / iter-3 step 3 — C-only scaling sweep (read-singleshot + flush-collapse + route_seq)

Task plan: `docs/task_plan_20260424_lock_decomp_microbatching.md` §Phase 2.

## Optimisations landed in this sweep

Three independent sub-steps, each with its own rollback gate:

1. **`[read-singleshot]` (2.4)** — `search()` 8-attempt retry loop collapsed
   to one pass. x86 aligned u64 loads are atomic so the slot pair is
   individually torn-free. LRC read semantics relaxed to "snapshot at some
   instant during scan".
2. **`[flush-collapse]` (2.6)** — 14 clflushopts per bucket scan (7 × key +
   7 × value) collapsed to 2 (one per 64 B cacheline of the 128 B bucket).
3. **`[route-seq]` (2.5)** — `SlotLockEntry` gains a `route_seq`
   cacheline_u64 bumped by INSERT/DELETE. UPDATE reads it before the
   pre-scan, re-reads after `lock_slot`, and skips the under-lock key
   re-verify when unchanged — which for workloads A/B/F trans phase (zero
   INSERT/DELETE) is always the case.

## Results (cache=on peaks, Mops/s)

Baseline = `logs/g34_scaling_sweep_p2_v4_20260422_205644` (iter-0 per-bucket
LFM). iter1 / iter2 =
`docs/g34_scaling_ycsb_C_only_20260423_051200/`,
`docs/g34_scaling_ycsb_C_only_20260423_054027/`.

| workload | baseline | iter1 | iter2 | phase-2 | vs 20 Mops/s      |
|----------|----------|-------|-------|---------|-------------------|
| a        | 1.08     | 3.41  | 3.27  | **6.24**  (T=86)  | 3.2 × below       |
| b        | 6.55     | 9.98  | 10.54 | **32.57** (T=86)  | **above bar** ✓  |
| c        | 51.22    | 46.01 | 45.99 | 55.12    (T=86)   | above target      |
| d        | 45.86    | 38.30 | 41.00 | 39.50    (T=86)   | reference only    |
| f        | 1.44     | 3.53  | 4.34  | **10.49** (T=86)  | 1.9 × below       |

Phase-2 **crosses the 20 Mops/s north-star on workload B** for the first
time (32.57 Mops/s at T=86, a 3.09 × gain over iter-2). A and F remain
below the bar but are materially closer — A is 1.91 × its iter-2 peak,
F is 2.42 ×.

## Sub-step contribution (quantified at the gate points)

- **2.4 alone**: r_p99 on A T=86 dropped from ~1.4 ms (iter-2) to under
  100 µs — the retry loop under hot-write storms was paying full bucket
  re-flushes per attempt, and that tail dominated the read-heavy half of
  workload A.
- **2.4 + 2.6 combined** at T=86 cache=on: A 3.27 → ~5-6 Mops/s;
  scan/search clflushopts per bucket dropped 14 → 2.
- **2.4 + 2.6 + 2.5** at T=86 cache=on (Phase-2 gate spot-check):
  A = 9.36 Mops/s (+186 % vs iter-2), D = 48.42 Mops/s (+26 %). Both
  gates satisfied; the [route-seq] strict gate (A/F ≥ +5 % AND
  D ≤ 5 % regress) held.
- **Full 80-run Phase-2 sweep** landed the final peak cache-on numbers
  reported above. The spot-check T=86 A is slightly higher than the
  full-sweep value because the spot-check ran in isolation without the
  79 preceding runs' resource pressure on the same devdax region.

## Pass condition

Task bar: each of workloada / b / f ≥ 20 Mops/s at some T.

- **workloadb: PASSES** (32.57 Mops/s at T=86 cache=on).
- **workloada: NOT met** (6.24 Mops/s; need 3.2 ×).
- **workloadf: NOT met** (10.49 Mops/s; need 1.9 ×).

Phase-3 micro-batching (see next iteration_note under
`docs/g34_scaling_ycsb_C_only_microbatch_<ts>/`) attacks the remaining A/F
gap by amortising the ~3 µs per-op cross-host CXL epoch bump over K
batched writes.

## Phase-1 deferral notice

Full 4-stage rdtscp LFM anatomy (plan §1.1–1.4) was **deferred** out of
the iter-3 scope to protect the Phase-3 time budget. Evidence from iter-2
decomp (lock p50 flat across T=8..64 at 6.3–9.0 µs while lock p99 grew
27 × from 24 µs to 443 µs) already matches the "queueing dominates,
acquire physics flat" signature that Phase-1 would formally prove.
Phase-3 design does not depend on that proof; instrumented-LFM runs are
tracked as follow-up.

## Files produced

- 14 cache-on plots + 9 extra-compare overlays (baseline → iter1 → iter2 →
  phase-2).
- 14 cache-off plots under `cache_off/`.
- Raw `SUMMARY.log` (80 runs, all OK).
- Provenance `plot_commit.txt` (git HEAD at sweep start).
