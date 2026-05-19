# F2 decision — LRU sampling cache_pool_lookup

**Date**: 2026-05-19
**Phase**: P4.3 (path_decomp follow-on)
**Hypothesis**: `lru_epoch.store` on every cache_pool_lookup hit creates cross-core
MESI ping-pong on KvCacheEntry cacheline 0; sampling 1/64 hits via `__rdtsc() & 0x3F == 0`
preserves approximate-LRU semantics while removing the write-on-read cost.

## Builds compared

- **baseline**: `build-cxl-w1` (FUSEE_LRU_SAMPLE=0, default)
- **F2**: `build-cxl-w1-lru` (FUSEE_LRU_SAMPLE=1, otherwise identical)

Both builds have HAZARD + W1 RESERVED enabled (iter-13A as-shipped).

## Cells × reps

| Cell type | Workload | T | KV | Cache | Reps |
|---|---|---|---|---|---|
| Target | workloadc | 64 | 1024 | on | 5 |
| Target | workloadc | 64 | 256 | on | 5 |
| Target | workloadb | 64 | 1024 | on | 5 |
| Guard | workloada | 64 | 1024 | on | 5 |
| Guard | workloadd | 64 | 1024 | on | 5 |

200k ops per cell, 2-host runs, h0 throughput aggregated.

## Bootstrap 95% CI (10000 resamples, seed=42)

| Cell | w1 median (ops/s) | lru median | delta% | CI95 | Role | Decision |
|---|---:|---:|---:|---|---|---|
| workloada T=64 kv=1024 cache=on | 10,982,373 | 11,003,521 | +0.19% | [-9.69, +5.78] | guard | FAIL |
| workloadb T=64 kv=1024 cache=on | 17,922,752 | 18,065,215 | +0.79% | [-1.60, +1.88] | target | FAIL |
| workloadc T=64 kv=1024 cache=on | 18,511,662 | 18,363,786 | -0.80% | [-4.11, +0.85] | target | FAIL |
| workloadc T=64 kv=256 cache=on | 18,717,828 | 18,422,991 | -1.58% | [-4.08, +9.97] | target | FAIL |
| workloadd T=64 kv=1024 cache=on | 17,817,371 | 17,542,320 | -1.54% | [-4.11, +2.04] | guard | FAIL |

## Universal fix policy threshold

- **Pass criterion**: target cell delta CI lower bound > +1% (positive shift demonstrably above noise).
- **Result**: 0 / 3 target cells pass. All three target CI lower bounds are negative.
- **Guardrails**: workloada and workloadd CIs cross 0 with point estimates within ±1.5% → no harm,
  but also no improvement.

## Decision: **ROLLBACK**

`FUSEE_LRU_SAMPLE` default remains 0. Code is preserved under the flag for future
RCA / measurement use. No build target change.

## Interpretation

The R2hit anomaly (0.23 µs p50 vs spec 0.03 µs) **does exist** in the path_decomp data
(per_stage_decomp.md SOFT-ANOMALY 2). But under the actual workload, removing the
write-on-read does not translate to throughput gain. Two non-exclusive hypotheses:

1. **Bandwidth-bound, not MESI-bound**: at T=64 the cache_pool hot-path is dominated
   by W10 (cache_pool_insert MESI ping-pong on the 1088 B KvCacheEntry, structural)
   and/or by bimodal flat-line behavior (~50% of runs in slow mode). Eliminating the
   8-byte lru_epoch write per read is a tiny fraction of total traffic on this entry.

2. **Per-hit cost was overstated**: rdtsc() + branch on every hit adds back some of
   the cost we removed; net effect is wash.

The R2hit anomaly itself remains real (probe data is solid). It just isn't
throughput-load-bearing under multi-threaded W10-dominated workloads.

## iter-15A backlog

If iter-15A targets the W10 structural anomaly (cache_pool_insert MESI ping-pong),
this F2 measurement should be re-run after that fix — once W10 stops dominating,
the R2hit lru_epoch ping-pong may become visible.
