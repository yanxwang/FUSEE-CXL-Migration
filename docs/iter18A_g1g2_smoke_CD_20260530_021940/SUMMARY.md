# g1/g2 smoke C+D: extend coverage of x8-regression validation

**Date**: 2026-05-30

After RCA v3 confirmed both Micron CXL modules at x8 (downgraded from
x16), ran two additional cells on g1/g2 to test whether the ~50 %
throughput reduction holds across the (T, N) space, not just at the
two A/B cells from yesterday.

## Results

g1/g2 (3-rep median, all cache=0, V=1024, zipf-0.99, worker_id routing,
build-cxl-w1-v1024 with iter-18A flags), compared to iter-18A Phase 4
sweep (worker_id reps 1-3):

| Cell | T | N | Today (Mops) | iter-18A median (Mops) | ratio |
|------|---|---|----:|----:|----:|
| A | 32 | 0 | 0.634 | 1.181 | **54 %** |
| B | 16 | 4 | 2.278 | 4.066 | **56 %** |
| **C** | **64** | **4** | **2.777** | **5.203** | **53 %** |
| **D** | **8**  | **0** | **0.578** | **1.108** | **52 %** |

## Implication

**All 4 cells regressed to 52-56 % of historical** — consistent within
4 pp across a 16× range of effective parallelism (T·max(N,1) =
8 → 256). This is much stronger evidence than two cells alone.

Important: cell D (T=8 N=0) is the lowest-concurrency cell I sampled,
which I initially expected to be latency-dominated (i.e., less affected
by BW reduction). It also halved. This indicates **the FUSEE
xhost_read path is BW-bound across the entire sampled regime**, not
just at high T. The bottleneck stage in FUSEE (likely receiver-side
load aggregation against the CXL slab) is the BW-amplifier, and
halving CXL BW directly halves throughput regardless of input
concurrency.

## Predicted post-recovery numbers

If h3-side link retraining recovers Micron to x16, expect each cell's
throughput to come back to ~1.9× its current value (since
x16/x8 = 2, but with some real-world efficiency loss). The 4 cells
form a calibration vector: post-recovery sweep should land at:

| Cell | Predicted (×1.9) | iter-18A historical |
|------|----:|----:|
| A | ~1.20 Mops | 1.18 Mops |
| B | ~4.33 Mops | 4.07 Mops |
| C | ~5.28 Mops | 5.20 Mops |
| D | ~1.10 Mops | 1.11 Mops |

A match across all 4 to within ±5 % will confirm a clean recovery.
Use this as the gate before declaring testbed "back to iter-18A
baseline" — anything else (asymmetric recovery, only some cells
back) needs separate investigation.

## Data
[grid.csv](grid.csv) + [raw/](raw/) (3 reps per cell, all wallclock
21-31 s).
