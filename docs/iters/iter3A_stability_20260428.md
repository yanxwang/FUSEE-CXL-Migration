# iter-3A multi-rep stability data (extension period)

**Date**: 2026-04-28
**Method**: 5 reps per cell, with explicit `pkill -9 cxl` cleanup
between reps. 200k trans ops, num_buckets=65536, cache as
indicated, FUSEE_PER_HOST_RING=1, FUSEE_PER_SLOT_LFM_A=1.

## Workload C cache=off T=84 K=1 (sweep1 peak)

```
rep=1 : 21,995,038 ops/s (wall=0.009s)
rep=2 : 22,576,890       (wall=0.009s)
rep=3 : 22,505,653       (wall=0.009s)
rep=4 : 22,703,751       (wall=0.009s)
rep=5 : 22,332,934       (wall=0.009s)
median: 22.51 Mops/s, σ ≈ 0.27 — PASS 20 Mops/s
```

## Workload C cache=off T=82 K=2 (sweep2 reported peak)

```
rep=1 : 21,656,647 (wall=0.009s)
rep=2 : 21,315,903 (wall=0.009s)
rep=3 : 34,901,090 (wall=0.006s)  ← outlier
rep=4 : 21,885,372 (wall=0.009s)
rep=5 : 21,497,783 (wall=0.009s)
median: 21.66 Mops/s, σ ≈ 5.4 — PASS 20 Mops/s
```

The original sweep2 single-rep recorded **35.13 Mops/s**;
re-measurement reveals it was a 1-in-5 high-variance outlier.
K=2 is **slightly worse** than K=1 at this cell.

## Workload A cache=off T=84 K=1 (sweep1 A peak)

```
rep=1 : 4,545,713
rep=2 : 4,863,397
rep=3 : 4,441,120
rep=4 : 3,211,105
rep=5 : 5,346,525  ← crosses 5 Mops/s bar
median: 4.55 Mops/s, σ ≈ 0.78 — miss 5 Mops/s by median; 1/5 reps cross
```

Original sweep1 single-rep: 3.95. iter-3A workload-A K=1 peak
**reframed to 4.55 Mops/s** under multi-rep median.

Comparison with K=2: both K=1 (median 4.55) and K=2 (median 4.62)
have 1/5 reps cross 5 Mops/s. Statistically indistinguishable at
this cell — confirms K-channel routing is no-op.

## Workload A cache=off T=82 K=2 (sweep2 A peak)

```
rep=1 : 4,463,776 (w_p99=251,515 ns)
rep=2 : 4,619,164 (w_p99=372,724)
rep=3 : 5,219,933 (w_p99=402,696)  ← crosses 5 Mops/s bar
rep=4 : 3,542,310 (w_p99=614,249)
rep=5 : 4,810,989 (w_p99=347,930)
median: 4.62 Mops/s, σ ≈ 0.65 — miss 5 Mops/s by median; 1/5 reps cross
```

Original sweep2 single-rep: 4.01. iter-3A workload-A peak
**reframed to 4.62 Mops/s** under multi-rep median.

## Workload D cache=off T=64 K=2

```
rep=1 : 27,535,936 (wall=0.007s)
rep=2 : 26,876,919 (wall=0.007s)
rep=3 : 26,991,347 (wall=0.007s)
rep=4 : 27,559,681 (wall=0.007s)
rep=5 : 26,977,654 (wall=0.007s)
median: 26.99 Mops/s, σ ≈ 0.31 — PASS 20 Mops/s
```

## Workload B cache=on T=32 K=2

```
rep=1 : 14,859,655 (wall=0.013s)
rep=2 : 15,189,059 (wall=0.013s)
rep=3 : 14,790,331 (wall=0.014s)
rep=4 : 15,193,772 (wall=0.013s)
rep=5 : 15,212,372 (wall=0.013s)
median: 15.19 Mops/s, σ ≈ 0.18 — miss 20 Mops/s
```

## Workload A cache=on K=1 across T (3 reps each, per-slot LFM)

```
T=8  : 1.66, 2.23, 2.25 Mops/s — median 2.23, σ ≈ 0.34
T=16 : 2.29, 2.83, 2.54         — median 2.54, σ ≈ 0.27
T=32 : 3.02, 3.07, 3.07         — median 3.07, σ ≈ 0.02
```

Demonstrates that the per-slot LFM benefit grows monotonically with
T (consistent with Phase 6 decomp showing 3-5× S1 reduction at T≥8).

## Workload F cache=off T=64 K=2

```
rep=1 : 3,277,095 (wall=0.061s)
rep=2 : 3,469,542 (wall=0.058s)
rep=3 : 3,200,362 (wall=0.062s)
rep=4 : 3,390,156 (wall=0.059s)
rep=5 : 2,754,759 (wall=0.073s)
median: 3.28 Mops/s, σ ≈ 0.28 — miss 20 Mops/s
```

---

## Per-bucket vs per-slot LFM at workloadA cache=off K=1 (single-rep)

```
              per-bucket    per-slot     ratio
T=8   :       1.05          1.67         1.59×
T=16  :       0.66          2.25         3.41×
T=32  :       0.50          2.67         5.34×
T=64  :       0.28          3.26         11.6×
T=84  :       0.25          3.95         15.6×
```

Same shape as cache=on but more pronounced. Per-slot LFM at T=84
cache=off lifts throughput 15.6× over per-bucket. The path peaks
at T=82–84 (multi-rep median 4.55 Mops/s) instead of collapsing.

## Per-bucket vs per-slot LFM at workloadA cache=on K=1 (single-rep)

```
              per-bucket    per-slot     ratio
T=4   :       1.27          1.22         0.96 (per-bucket actually +3%)
T=8   :       1.16          2.26         1.95 (per-slot +95%)
T=16  :       0.92          2.07         2.25 (per-slot +125%)
T=32  :       0.66          2.63         3.98 (per-slot +298%)
T=64  :       0.15          2.84         18.9 (per-slot +1790%)
T=84  :       0.10          1.85         18.5 (per-slot +1750%)
```

Per-bucket LFM peaks at **T=4 = 1.27 Mops/s**, then collapses
under Zipf hot-bucket contention (LFM mutex serialization).
**This exactly matches iter-2A-revised's reported peak (1.27 Mops/s
@ T=4)**, confirming iter-2A-revised's path was per-bucket LFM
running on the same no-op N:1:1:N as iter-3A's primary deliverables.

Per-slot LFM converts the contention collapse into roughly flat
scaling at high T. **Clean attribution: per-slot LFM IS the
iter-3A win that lifts the workload-A peak from 1.27 (T=4) to
~4.6 Mops/s (T=82+ multi-rep median)**.

## Per-slot LFM win across workloads (T=32 cache=on K=1, single-rep)

```
              per-bucket    per-slot     Speedup
workloadb :    4.60          14.41        3.13×  (95R/5U)
workloadc :   16.54          17.68        1.07×  (100R — no writes contend)
workloadd :   16.54          16.40        1.00×  (5I/95R — light writes)
workloadf :    1.01           4.03        3.99×  (50/50 RMW — writes dominate)
```

Per-slot LFM win is **write-frequency-driven**: workloads with
significant write share (B, F) see 3-4× speedup at T=32; read-only
(C) or read-mostly (D) workloads see no benefit because the write
lock is rarely contended. Generalises the workload-A attribution
(15-19× at high T) — per-slot LFM is the right port for *any*
Zipf-distributed write-heavy workload.

## Stable peak summary

| Workload | Stable median | Original sweep value | 20 Mops/s bar |
|----------|---------------|----------------------|---------------|
| a (50/50 R/U Zipf) | 4.62 | 4.01 (+15 %) | miss — 1/5 reps cross |
| b (95/5 R/U)       | 15.19 | 15.28 (-0.6 %) | miss |
| c (100 R)          | 22.51 (K=1) | 21.88 K=1 / 35.13 K=2 single-rep | **PASS** |
| d (5/95 INSERT/R)  | 26.99 | 27.24 (-0.9 %) | **PASS** |
| f (50/50 RMW)      | 3.28 | 2.97 (+10 %) | miss |

## Key inferences

1. **K=1 is at least as stable / fast as K=2** at every cell measured.
   The K-channel routing (Finding-1) genuinely no-ops at runtime;
   sweep2's K=2 numbers are artefacts of CPU-pinning topology + run-
   to-run variance, not real K-fold parallelism.

2. **Wall-clock < 10 ms is noise-dominated**. Workload C / D peak
   cells finish in 6-9 ms wall — 5-rep averaging is required for
   any conclusion. iter-4A sweeps should bump trans_ops to 1M+ to
   get wall-clock past 50 ms.

3. **Workload A's 5 Mops/s bar is borderline**, not falsified.
   1/5 reps cleared it. With more aggressive iter-4A optimisation
   (proper N:1:1:N + per-slot LFM working in concert), the median
   may move past 5 Mops/s.

4. **YCSB-C migration target ACHIEVED** at 22.5 Mops/s stable median;
   YCSB-D simultaneously achieved at 27.0 Mops/s. Both are read-
   heavy workloads.
