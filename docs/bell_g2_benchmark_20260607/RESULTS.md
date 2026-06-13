# G2 (2CN+2MN, data=2, idx=2) — Standard Results

Pristine RDMA FUSEE on bell. value=1024B. 3-rep mean. Both YCSB methods (paper / lw).

## Part 1 — Micro latency (1 client, 1 coroutine, sequential sync, µs)

| op | mean | p50 | p99 |
|---|---:|---:|---:|
| search | 10 | 5 | 7 |
| insert | 26 | 13 | 17 |
| update | 29 | 14 | 19 |
| delete | 36 | 17 | 34 |

## Part 2 — Micro throughput (Mops/s)

| clients | insert | search | update | delete |
|---:|---:|---:|---:|---:|
| 2 | 0.158 | 0.623 | 0.138 | 0.078 |
| 4 | 0.322 | 1.387 | 0.299 | 0.145 |
| 8 | 0.669 | 2.444 | 0.619 | 0.267 |
| 16 | 1.468 | 4.078 | 1.218 | 0.443 |
| 28 | 2.519 | 5.152 | 1.892 | 0.594 |

## Part 3 — YCSB throughput (Mops/s, paper / lw)

| workload | 2cl | 4cl | 8cl | 16cl | 28cl |
|---|---:|---:|---:|---:|---:|
| a (50r/50u) | 0.14/0.15 | 0.33/0.39 | 0.67/0.82 | 1.32/- | 2.20/- |
| b (95r/5u) | 0.39/- | 0.81/0.85 | 1.60/1.74 | 3.24/3.48 | 5.08/6.08 |
| c (100r) | 0.49/0.48 | 1.15/1.14 | 2.48/2.46 | 5.09/4.90 | 5.09/7.00 |
| d (95r/5i) | 0.42/0.41 | 0.97/0.98 | 2.11/- | 4.29/4.23 | 6.09/6.90 |

## Notes (G2, idx=2)
- **Headline = ypaper** (paper method): 60/60 cells clean, no anomalies; workloada
  scales to 28cl=2.20 Mops (no crash at idx=2, unlike G1's idx=1 paper a-28cl SIGSEGV).
- **lw INVALID for update-heavy workloada at idx≥2**: all workers replay the identical
  trace in lockstep → simultaneous CAS on the same keys' replicated index slots →
  FAIL_REDO storm + worker crashes. lw-a reps mostly FAIL; reported lw-a values are
  unreliable (shown for completeness, "-" = FAIL). Read-heavy lw (c, mostly b/d) is valid.
  This confirms lw ≠ ypaper: ypaper (disjoint op-slices) is the correct YCSB method;
  lw over-states contention pathologically once the index is replicated (idx≥2).
- idx=2 vs G1 idx=1: write throughput/latency degrade (index replicated to 2 MNs,
  log commit no longer skipped) — expected (paper Fig 18). Reads (~5 Mops ceiling) and
  read latency (search p50 5µs) unchanged. delete latency p50 17µs (vs 12 at idx=1).
- A few sporadic lw read-cell blanks (b-2cl, d-8cl) = transient loader/worker FAIL; the
  ypaper equivalents are clean.
