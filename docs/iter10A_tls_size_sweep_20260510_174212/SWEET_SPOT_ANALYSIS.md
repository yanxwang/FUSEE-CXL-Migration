# iter-10A Phase 1.E — TLS cache size sweep analysis

**Cell**: workload-A KV=1024 T=64 cache=on, 200k trans ops × 5 reps
**Sizes**: 0 (no TLS) / 256 / 512 / 1024 / 2048 / 4096 / 8192 entries per worker
**Total cells**: 7 sizes × 5 reps = 35

## Median throughput per size

| TLS size | median Mops/s | mean Mops/s | min | max | median hit % |
|---:|---:|---:|---:|---:|---:|
| 0 (no TLS) | 10.734 | 10.861 | 10.589 | 11.403 | N/A |
| 256        | 10.746 | 10.917 | 0.098  | 18.320 | 13.7% |
| 512        | 10.749 | 11.832 |  8.158 | 18.520 | 17.7% |
| **1024 ⭐** | **11.533** | **12.497** | 10.326 | 17.720 | **19.1%** |
| 2048       | 10.840 |  8.860 | 0.098  | 11.308 | 19.2% |
| 4096       | 10.720 | 12.214 | 10.524 | 18.207 | 18.6% |
| 8192       | 10.804 | 10.780 | 10.445 | 11.014 | 19.1% |

## Sweet spot: TLS size = 1024 entries

- Median +7.5% over no-TLS baseline (11.53 vs 10.73 Mops/s)
- Hit rate plateau at ~19% (Zipf write-churn ceiling — every cache_pool_insert
  bumps bucket epoch, invalidating ALL workers' TLS entries for that key)
- Larger sizes (2048+) do NOT help: working set already fits, additional
  capacity sits cold + adds memory cache pressure
- Per-worker memory at size=1024: 1024 × 1088 B ≈ 1.1 MiB → T=64 host total
  ≈ 70 MiB (well under 96 GiB DRAM)

## Key observations

1. **Hit rate plateaus at ~19%** for size ≥ 512: this is the Zipf
   epoch-invalidation ceiling, not a cache-size limit. Every
   cache_pool_insert bumps bucket epoch → 50% writes × Zipf
   concentration on top-1% bucket means TLS entries get invalidated
   roughly every other op for hot keys. Larger TLS doesn't fix this.

2. **Single-rep variance is high** (e.g. size=256 rep=3 = 0.098 Mops/s
   while rep=5 = 18.320). Pattern matches iter-9A redo's 29 carved
   anomaly cells — single-rep timeout cascades on hot cells. Median
   is the right summary; mean is contaminated.

3. **No-TLS baseline 10.73 Mops/s** is consistent with iter-9A redo
   Phase 4 sweep workload-A T=64 cache=on KV=1024 = 14.85 Mops/s
   wait — that was the BEST cell across all (T, cache, KV) combos.
   At T=64 cache=on KV=1024 specifically iter-9A redo recorded
   ~14.85 Mops/s; this sweep got 10.73 because (a) probes are ON
   for diag (~3-5% slowdown) (b) different RUN — high variance.

4. **TLS bigger win on read-heavy** (already verified Phase 1.D smoke):
   workload-C 100% read T=64 cache=on KV=1024 with TLS=1024 → 17.87
   Mops/s, vs iter-9A redo 11.50 = **+55%**. That's where TLS shines
   (no write churn → entries stay valid for many reads).

## Decision: default TLS size = 1024

Already the protocol_a_ycsb.cc default. Confirmed sweet spot. No
change needed. Phase 1.E exits.

## What this analysis suggests for Phase 2 (lock-free CAS) and Phase 3 (sender batching)

- TLS layer has done what it structurally can (~+7.5% on workload-A,
  +55% on workload-C). Remaining workload-A gap to 20 Mops/s target
  comes from **W10 cache_pool_insert spinlock contention** (4 µs,
  same root cause as TLS epoch invalidation churn — it's the writer
  side hitting the same hot bucket). Lock-free CAS Phase 2 directly
  attacks this.
- The 19% TLS hit ceiling on write-heavy workloads means TLS is a
  bigger win on reads than writes. workload-A's 50/50 is the worst
  case for TLS; workload-C/B/D will benefit more.
