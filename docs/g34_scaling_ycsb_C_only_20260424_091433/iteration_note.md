# 2M-ops C-only scaling sweep (iter-3 validation)

Task: user request 2026-04-24 "做一次 scaling_ycsb_C_only 实验,唯一的
变化 ops 使用 2M 而不是 200k" — validates that the Phase-3 batching
numbers hold under a steady-state workload rather than the
short-run 200 k ops cap (spec §11 explicitly notes "short runs
underestimate steady-state by 40-60 % at T=86").

Config: same as Phase-3 final sweep (`FUSEE_BATCH_K=4096`,
`FUSEE_BATCH_T_US=100`, `FUSEE_BATCH_MERGE_SAME_KEY=ON`,
`FUSEE_BATCH_NUM_FLUSHERS=1`), but with `MAX_OPS=2000000`. All 80
runs completed (ok=80, fail=0).

## Results — cache-on peaks (Mops/s)

| workload | 200 k peak  | **2 M peak**            | Δ       | vs 20 Mops/s bar |
|----------|------------:|------------------------:|--------:|------------------|
| a        | 17.05 @T=64 | **15.03** @T=86         | −12 %   | 0.75 × (below)   |
| b        | 33.37 @T=64 | **50.24** @T=64         | +51 %   | **PASS** ✓       |
| c        | 48.73 @T=86 | **64.96** @T=64         | +33 %   | PASS (reference) |
| d        | 33.92 @T=64 | **52.27** @T=64         | +54 %   | PASS (reference) |
| f        | 20.48 @T=64 | **17.03** @T=32         | −17 %   | 0.85 × (below)   |

## Results — cache-off peaks (Mops/s)

| workload | 200 k peak  | **2 M peak**            |
|----------|------------:|------------------------:|
| a        |  3.70 @T=64 | **14.46** @T=86         |
| b        |  ~36       | **53.37** @T=64         |
| c        |  ~42       | **56.90** @T=64         |
| d        |  ~25       | **48.04** @T=64         |
| f        |  ~3.9      | **21.31** @T=86  **PASS ✓** |

## Observations

1. **B, C, D scale up significantly at 2 M** (+33 … +54 %) — their
   throughput was bottlenecked by the 200 k cap's fixed initialisation
   overhead, not by steady-state contention. The 2 M runs reach true
   steady-state.
2. **Workload A drops −12 % at 2 M** (17.05 → 15.03). At higher ops
   count, long-run hot-bucket contention on the single flusher
   dominates more than short-run variance. The peak location also
   moves from T=64 → T=86; T=64 at 2 M shows 14.43 Mops/s. The
   single-flusher bottleneck diagnosed in the Phase-3 iteration_note
   is confirmed more sharply here: at steady state the flusher is
   CPU-pinned on cross-host `bump_epoch`, and adding workers past
   T=64 stops helping.
3. **Workload F drops −17 % at 2 M (cache-on)** but rises to
   **21.31 Mops/s cache-off PASS** at T=86. With cache=off, readers
   always hit CXL (no cache invalidation overhead trading off with
   batching), so the read/write balance is more stable.
4. **Pass condition check at 2 M**:
   - **workloadb: PASSES** (50.24 cache-on, 53.37 cache-off).
   - **workloadf: PASSES** cache-off only (21.31 @T=86).
   - **workloada: STILL FAILS** (15.03 Mops/s peak; 1.33 × below bar).
5. The 2 M data strengthens the Phase-3 conclusion: A's remaining
   gap IS structural (single-flusher cross-host `bump_epoch` serial
   cost), and visible even more clearly at steady state.

## Files produced

Per spec: SUMMARY.log (80 runs, ok=80 fail=0), plot_commit.txt, 14
cache-on plots, 14 cache-off plots, extra/ cross-iter overlays
(baseline → iter1 → iter2 → phase2 → phase3).

## Pointers

- Raw: `logs/g34_scaling_sweep_C_only_2M_20260424_091433/`
- Binary: `~/FUSEE_CXL/build-cxl/tests/cxl_ycsb_runner_C` at commit
  `5189086` (reverted multi-flusher; N=1 flusher stable baseline).
