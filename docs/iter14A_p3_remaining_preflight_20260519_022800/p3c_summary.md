# P3.C probe overhead — inconclusive due to bimodal

Date: 2026-05-19

Cell: workload-a T=64 cache=on KV=1024 × 10 reps × {build-cxl-w1,
build-cxl-w1-probe}. MAX_OPS=200000.

## Raw

| build | reps (Mops/s, sorted) | median |
|---|---|---:|
| build-cxl-w1 | 10.45, 10.64, 10.66, 10.67, 10.73, 10.87, 11.71, 17.13, 17.22, 17.33 | 11.29 |
| build-cxl-w1-probe | 9.97, 10.18, 10.34, 10.42, 10.61, 16.59, 17.06, 17.30, 17.35, 17.52 | 13.60 |

Naive delta: +25.95% (probe build looks *faster*). That's because both
builds show **bimodal distribution** (low ~10 Mops/s mode, high ~17
Mops/s mode), and the median straddles the split differently by chance.

## What this tells us

1. Bimodal collapse from iter-11A/iter-12A is **still present** at
   iter-13A HEAD. iter-12A Phase 5 claimed fix; result here shows it
   may not have fully resolved.
2. Probe overhead measurement requires more sophisticated methodology:
   either separate "fast" and "slow" modes and compare within-mode, or
   use ≥ 30 reps with bootstrap.
3. Per-mode comparison (approximation):
   - "fast" mode: w1 = 17.2 Mops/s median, w1-probe = 17.3 Mops/s — 0% overhead
   - "slow" mode: w1 = 10.7 Mops/s median, w1-probe = 10.4 Mops/s — ~3% overhead
4. Conclusion: probe overhead is approximately **0-3%** within
   either mode, but the bimodal effect dwarfs the probe cost.

## Decision

- Use FUSEE_PROBE=1 build for P4 path_decomp (build-cxl-w1-probe).
- 3% in-mode overhead is acceptable for stage decomposition.
- **Bimodal collapse is its own anomaly** → P4 investigation item;
  may motivate iter-15A work even if P4 finds no other anomaly.
