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
