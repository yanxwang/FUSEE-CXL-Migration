# F2 decision — LRU sampling cache_pool_lookup (REAL implementation)

**Date**: 2026-05-19
**Phase**: P4.3 (path_decomp follow-on)
**Predecessor**: [docs/iter14A_p4_f2_lru_sample_20260519_024400](../iter14A_p4_f2_lru_sample_20260519_024400/decision.md)
— first F2 attempt had the flag declared but **not wired into `cache_pool.cc`**;
the two builds were behaviorally identical. This re-measurement uses the
real implementation (commit pending) where `cache_pool_lookup` actually
gates `lru_epoch.store` on `__rdtsc() & 0x3FULL == 0` when `FUSEE_LRU_SAMPLE=1`.

## Hypothesis (unchanged)

`lru_epoch.store` on every cache_pool_lookup hit creates cross-core MESI
ping-pong on KvCacheEntry cacheline 0. Sampling 1/64 hits via rdtsc low
bits preserves approximate-LRU semantics while removing the write-on-read
cost. P4 path_decomp recorded R2hit p50 = 0.23 µs (7.7× spec 0.03 µs).

## Builds

- **baseline**: `build-cxl-w1` (FUSEE_LRU_SAMPLE=0 default)
- **F2**: `build-cxl-w1-lru` (FUSEE_LRU_SAMPLE=1)

Both rebuilt 2026-05-19 02:56 CDT against the real-implementation source.
Both use FUSEE_READ_GUARD=2 (HAZARD) + FUSEE_WRITE_ALLOC=1 (W1 RESERVED).

## Bootstrap 95% CI (10000 resamples, seed=42)

| Cell | w1 median | lru median | delta% | CI95 | Role | Decision |
|---|---:|---:|---:|---|---|---|
| workloadc T=64 kv=1024 cache=on | 19,062,142 | 18,859,028 | -1.07% | [-12.48, +2.76] | target | FAIL |
| workloadc T=64 kv=256 cache=on  | 18,712,574 | 19,157,088 | +2.38% | [-7.73, +9.06]  | target | FAIL |
| workloadb T=64 kv=1024 cache=on | 18,138,944 | 18,238,190 | +0.55% | [-7.50, +7.43]  | target | FAIL |
| workloada T=64 kv=1024 cache=on | 11,077,263 | 10,957,102 | -1.08% | [-6.40, +6.49]  | guard  | FAIL |
| workloadd T=64 kv=1024 cache=on | 17,964,609 | 18,073,377 | +0.61% | [-9.59, +14.36] | guard  | FAIL |

## Universal fix policy threshold

- **Pass criterion**: target cell delta CI lower bound > +1%.
- **Result**: 0 / 3 target cells pass. workloadc kv=256 has the largest
  point estimate (+2.38%) but CI lower bound is -7.7% — well inside noise.
- Guardrails (workloada, workloadd) within ±1.2% point estimate, both CIs
  cross 0. No regression, no improvement.

## Decision: **ROLLBACK**

`FUSEE_LRU_SAMPLE` default remains 0. Code is preserved under the flag for
future RCA / measurement use. No build-target change shipped.

## Interpretation

The R2hit anomaly (0.23 µs p50 vs spec 0.03 µs from P4 path_decomp) is
real. Removing the cache-line-0 write-on-read does not translate to
throughput gain because:

1. **W10 dominates the cache hot-path under multi-thread load.**
   `cache_pool_insert` ping-pongs the 1088 B KvCacheEntry across cores at
   3.54 µs p50 (3.5× spec). The 8-byte lru_epoch.store removed by F2 is a
   small fraction of the cacheline-traffic budget on this entry.

2. **The "saved" write is replaced by rdtsc() + branch.** Even when
   skipped, the conditional evaluation is non-free at 1/64 frequency
   (the rdtsc still executes every call). Net cost wash within run noise.

3. **Bimodal flat-line state co-exists** at ~50% prevalence (P3.C). Mode
   noise is much larger than the F2 effect size on any single cell.

The R2hit improvement, if any, is below the noise floor of 5-rep
multi-host workload measurements. The probe-mode latency improvement
would be measurable in a single-thread probe-build re-run, but it is not
load-bearing for throughput.

## iter-15A implication

When iter-15A addresses W10 (cache_pool_insert structural fix — true
lock-free hashmap, async write-behind, or layout change reducing the
1088 B contended cacheline footprint), F2 should be re-measured. With
W10 removed, the R2hit ping-pong may become visible at throughput level.

The F2 code stays in tree under `FUSEE_LRU_SAMPLE=1` to make that
re-measurement trivial.
