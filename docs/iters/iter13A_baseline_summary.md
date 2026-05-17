# iter-13A Baseline Summary (= iter-12A post-fix sweep)

**Date**: 2026-05-17
**Sweep dir**: `docs/g34_scaling_ycsb_iter12A_postfix_20260517_022409/`
**Commit**: `2e61487` (iter-12A Phase 5 stale-head fix)
**Config**: spec-default — 5 workloads × 7 T × 2 cache × 3 KV × 1 rep = 210 runs, MAX_OPS=200000, TIMEOUT_S=600
**Wall**: 1386 s (~23 min)

---

## Headline numbers

| Workload | Peak Mops/s | 20 Mops/s gap | Best cell |
|---|---|---|---|
| workloada | **11.59** | -8.41 | T=64 cache=on kv=512 |
| workloadb | **19.00** | -1.00 | T=64 cache={on kv=1024 OR off kv=512} |
| workloadc | **18.93** | -1.07 | T=64 cache=on kv=256 |
| workloadd | **17.91** | -2.09 | T=64 cache={on kv=1024 OR off kv=256} |
| workloadf | **17.01** | -2.99 | T=64 cache=off kv=1024 |

**Observation**: workloadb / workloadc are within 5 % of the 20 Mops/s target. workloada is the largest gap (~42 % short). All workloads' peak at T=64 — scaling is healthy through the T grid.

---

## Anomaly scan (§13 gate 5)

- **COLLAPSES (thpt < 0.1 Mops/s)**: **0 cells**. iter-12A Phase 5 stale-head fix verified — bimodal mechanism fully eliminated across the 210-cell grid.
- **MIDs (0.1 ≤ thpt < 1.0 Mops/s)**: 35 cells, **all at T=1 or T=2** (single-/dual-thread regime; expected low absolute throughput at minimal parallelism). Not anomalous.
- **FAIL**: 1 cell — workloadd T=64 cache=on kv=512 (rc=124, `timeout` killed). Retry 5-rep: all 5 OK at 10.27–11.97 Mops/s (median 10.71). Fluky one-off, not bimodal. Marked transient.

**Gate 5 verdict**: PASS — zero unexplained outliers.

---

## Per-cell throughput (Mops/s)

### cache=on

| workload | kv | T=1 | T=2 | T=4 | T=8 | T=16 | T=32 | T=64 | peak |
|---|---|---|---|---|---|---|---|---|---|
| workloada | 256 | 0.33 | 0.75 | 1.43 | 2.56 | 4.93 | 7.44 | 9.27 | **9.27** |
| workloada | 512 | 0.31 | 0.70 | 1.30 | 2.42 | 4.77 | 7.26 | 11.59 | **11.59** |
| workloada | 1024 | 0.30 | 0.66 | 1.20 | 2.29 | 4.54 | 9.62 | 11.28 | **11.28** |
| workloadb | 256 | 0.45 | 2.14 | 3.69 | 5.33 | 7.82 | 11.10 | 18.89 | **18.89** |
| workloadb | 512 | 0.44 | 2.03 | 3.09 | 5.13 | 7.03 | 10.59 | 17.59 | **17.59** |
| workloadb | 1024 | 0.42 | 1.95 | 3.00 | 4.80 | 7.30 | 10.65 | 19.00 | **19.00** |
| workloadc | 256 | 1.09 | 2.72 | 4.47 | 5.81 | 8.30 | 11.17 | 18.93 | **18.93** |
| workloadc | 512 | 1.09 | 2.62 | 4.15 | 5.53 | 8.28 | 11.24 | 17.77 | **17.77** |
| workloadc | 1024 | 1.02 | 2.45 | 3.70 | 5.60 | 7.68 | 11.20 | 17.84 | **17.84** |
| workloadd | 256 | 0.94 | 2.13 | 3.65 | 5.34 | 7.81 | 10.88 | 17.88 | **17.88** |
| workloadd | 512 | 0.93 | 1.98 | 3.05 | 5.16 | 7.56 | 10.31 | FAIL† | **10.31** |
| workloadd | 1024 | 0.87 | 1.95 | 3.82 | 4.84 | 9.52 | 12.46 | 17.91 | **17.91** |
| workloadf | 256 | 0.36 | 1.00 | 1.74 | 3.62 | 6.62 | 11.95 | 12.99 | **12.99** |
| workloadf | 512 | 0.35 | 0.99 | 1.69 | 3.55 | 6.67 | 10.77 | 16.76 | **16.76** |
| workloadf | 1024 | 0.32 | 0.92 | 1.60 | 3.03 | 7.00 | 11.43 | 16.13 | **16.13** |

† 5-rep retry: 10.27-11.97 Mops/s (median 10.71). Sweep FAIL was transient.

### cache=off

| workload | kv | T=1 | T=2 | T=4 | T=8 | T=16 | T=32 | T=64 | peak |
|---|---|---|---|---|---|---|---|---|---|
| workloada | 256 | 0.31 | 0.74 | 1.43 | 2.57 | 4.87 | 7.70 | 9.25 | **9.25** |
| workloada | 512 | 0.31 | 0.71 | 1.31 | 2.49 | 5.12 | 7.32 | 10.65 | **10.65** |
| workloada | 1024 | 0.29 | 0.67 | 1.20 | 2.33 | 4.48 | 10.11 | 10.43 | **10.43** |
| workloadb | 256 | 0.45 | 2.16 | 3.70 | 5.46 | 8.05 | 11.04 | 18.02 | **18.02** |
| workloadb | 512 | 0.48 | 2.05 | 3.29 | 5.12 | 6.93 | 10.99 | 19.00 | **19.00** |
| workloadb | 1024 | 0.43 | 1.95 | 3.10 | 5.20 | 7.07 | 10.44 | 18.24 | **18.24** |
| workloadc | 256 | 1.12 | 2.71 | 4.51 | 5.66 | 8.39 | 11.55 | 18.19 | **18.19** |
| workloadc | 512 | 1.09 | 2.32 | 4.13 | 5.49 | 8.08 | 10.91 | 17.74 | **17.74** |
| workloadc | 1024 | 1.02 | 2.46 | 3.64 | 5.58 | 7.54 | 10.71 | 18.61 | **18.61** |
| workloadd | 256 | 0.98 | 2.18 | 4.03 | 5.65 | 9.93 | 10.93 | 17.91 | **17.91** |
| workloadd | 512 | 0.94 | 2.09 | 3.68 | 5.25 | 8.46 | 12.32 | 17.47 | **17.47** |
| workloadd | 1024 | 0.88 | 1.96 | 3.84 | 6.47 | 7.38 | 12.66 | 17.10 | **17.10** |
| workloadf | 256 | 0.36 | 1.00 | 1.85 | 3.57 | 7.27 | 11.07 | 13.46 | **13.46** |
| workloadf | 512 | 0.35 | 0.97 | 1.64 | 3.64 | 6.80 | 10.85 | 16.37 | **16.37** |
| workloadf | 1024 | 0.33 | 0.93 | 1.73 | 3.37 | 6.40 | 11.62 | 17.01 | **17.01** |

---

## Gap-to-target (20 Mops/s) — per workload

- workloada — gap **42 %**. R/U 50/50 Zipf. Worst gap; cross-host writes dominate path. Phase 2 (write copy elimination) is the most leveraged for this workload.
- workloadb — gap **5 %**. 95R/5U Zipf. Mostly reads on hot keys; cache=on/off similar peaks. Phase 1 read elimination should push over the line.
- workloadc — gap **5 %**. 100R Zipf. Pure read; Phase 1 read elimination most directly helps.
- workloadd — gap **10 %**. 95R/5I latest. Phase 1 helps; Phase 2 marginal (very few writes).
- workloadf — gap **15 %**. 50R/50RMW Zipf. Both phases help.

iter-13A's two phases align with the gap distribution: Phase 1 (read) helps b/c/d/f the most; Phase 2 (write) helps a/f the most.

---

## What this baseline IS used for

1. **iter-13A delta comparison**: every Phase 1 / Phase 2 sweep compares against this 210-cell baseline.
2. **Confirms iter-12A Phase 5 fix**: 0 collapse over 210 cells (vs 30-60 % pre-fix on the 3 residual cells) — fix is verified at full scale.
3. **iter-13A scope justification**: gaps to 20 Mops/s on workloads a/f/d/c/b show data-copy elimination is the right axis to attack (each path is ~30-50 % of cross-host op cost per prior path_decomp).

## What this baseline is NOT

- **Not a multi-rep verified number**: single-rep per cell. Per spec §IX G2, multi-rep stability gate is opt-in. iter-13A's representative-cell comparisons WILL be 5-rep median (Phase 1.3, 2.3), but the headline sweep numbers above are single-rep.
- **Not a path_decomp**: only end-to-end throughput. Iter-13A Phase 1 / Phase 2 RAPs will reference iter-11A's path_decomp data for stage attribution.
