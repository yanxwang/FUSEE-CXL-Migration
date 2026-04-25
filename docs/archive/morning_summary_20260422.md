# Morning summary — 2026-04-22 (overnight run completed ~04:25 CDT)

> One-page digest. Details under `docs/g34_bench/`,
> `docs/g34_scaling_ycsb/`, and the per-task commit messages.

## Scaling sweep — deliverable

**Exactly what you asked for**, all 240 runs clean, 0 failures:

- Workloads: a, b, c, d, f (e skipped — scan unimplemented)
- Protocols: A, B, C
- Clients per host: 1, 2, 4, 8, 16, 32, 64, 86
- Cache modes: on + off (cache-on batch first, then cache-off)

Plots (exceeding the 30 you asked for, because read/write latencies
were split):

- `docs/g34_scaling_ycsb/` — 42 cache-on plots
  - 15 throughput (A/B/C × 5 workloads)
  - 27 latency (write for all, read where applicable; bar × 3 = avg / p50 / p99)
- `docs/g34_scaling_ycsb/cache_off/` — 42 cache-off plots (same layout)
- `docs/g34_scaling_ycsb/extra/` — 14 additional analysis plots:
  - A/B/C comparison per workload (thpt + write-p99)
  - cache-speedup ratio per opt
  - C scaling efficiency curve
  - `summary_table.md` (peak-T per cell)
- `docs/g34_scaling_ycsb/analysis.md` — interpretation
- `docs/g34_scaling_ycsb/followup_experiments.md` — 7 proposed
  follow-ups ranked P1–P7

## Headline numbers (cache-on, agg across both hosts)

| | workloada (50/50) | workloadb (95R) | workloadc (100R) | workloadd (95R/5I) | workloadf (50R/50RMW) |
|---|---|---|---|---|---|
| A peak (kops/s) | 188 @ T=64 | 1 157 @ T=1 | 3 318 @ T=86 | 1 214 @ T=4 | 262 @ T=8 |
| B peak | 238 @ T=86 | 1 428 @ T=8 | 3 402 @ T=32 | 1 493 @ T=1 | 362 @ T=2 |
| **C peak** | **1 122 @ T=4** | **6 326 @ T=16** | **48 176 @ T=86** | **43 959 @ T=86** | **1 785 @ T=8** |

## Important caveat about A and B

A and B are **clamped to 1 worker per host** in the runner because
their PendingRing replication state is per-host (one ring matrix,
one replicator thread per process). Intra-host client scaling for A/B
needs a per-client ring refactor — documented as P2 in the follow-up
plan. A/B curves are therefore **flat across T by design**, not broken.

C has no per-host replication state and so scales genuinely.

## Follow-up experiments executed (P1, P6 from the proposed list)

- **P1 bucket-count sweep** (T=86, opt C, workloads a/f, buckets 16k-4M):
  bucket count is NOT the bottleneck at high T. Throughput flat across
  256× range in bucket count. w_p50 healthy 10 μs, w_p99 24-45 ms —
  scheduler tail, not contention. Finding + raw log under
  `extra/bucket_sweep_{finding.md,T86.log}`.
- **P6 cross-host vs solo** (opt C, g4-alone, T=1..86, 5 workloads):
  2-host vs 1-host gain is 1.5-2× at T≤8, drops to 1.35× on reads at
  T=86, and goes BELOW 1× for write-heavy workloads at T>32 (cross-host
  coordination overhead eats the gain). Table + plot under
  `extra/cross_host_gain_{finding.md,png}`.

## The interesting findings (beyond the requested plots)

1. **C on pure-read workloads (c, d) scales 14× over A/B** at T=86.
   48 M ops/s on workload-c. A/B capped at ~3.3 M because only 2 total
   workers.
2. **Write-heavy workloads (a, f) peak at T=4-8 for C, then decline**
   as T grows. Common hypothesis (bucket contention) tested and REJECTED
   by the bucket-count follow-up: sweeping buckets from 16 k → 4 M at
   T=86 shows throughput flat at ~230 k kops/s for workload-a. w_p50
   stays ~10 μs healthy, but w_p99 is 24-45 ms — classic scheduler tail.
   Real bottleneck: at 172 client processes TOTAL
   (= 86 per host × 2 hosts), any preempted process stalls everyone
   waiting on its bucket lock.
3. **Cache speedup for C is modest (~1.3×) on read-heavy workloads**
   whereas A/B get 1.5-4× from cache. Because C's read still loads one
   CXL cacheline (the epoch) per op even on cache hit; A/B can serve
   entirely from DRAM.

## One real bug was found + fixed along the way

`CxlKvStore{A,B}::enable_dram_cache()` set `cache_enabled_=true`
*before* allocating `cache_epoch_`. The replicator thread (spawned at
attach time) raced in between and dereferenced the empty vector →
segfault at `+0x78f8` in `cxl_ycsb_runner_A` on every cross-host run
with threads. Commit `9323ac8` fixes the ordering.

Same class of bug (store-then-allocate) might exist elsewhere —
quick grep found no others in the CXL tree.

## Commit trail (today's work)

```
4fc1041  [scaling] cross-host vs solo compare (P6)
2329ea1  [scaling] bucket-count sweep + finding (P1)
129419c  [scaling] extra plots + analysis
f08b6ac  [scaling] 240-run sweep complete
9323ac8  [bug fix] A/B enable_dram_cache race
a0595ef  [scaling] fork-based YCSB runner
```

GitHub: https://github.com/yanxwang/FUSEE-CXL-Migration (private).

## If you want to rerun

```bash
# Ship workloads b, d, f to slaves (a, c are already there)
# ... see scripts/run_g34_scaling_sweep.sh header for env knobs

scripts/rekey_slave.sh g3 && scripts/rekey_slave.sh g4        # if PXE-wiped
ssh g3 'chmod 666 /dev/dax0.0'; ssh g4 'chmod 666 /dev/dax0.0'
scripts/bootstrap_slave.sh g3 && scripts/bootstrap_slave.sh g4
bash scripts/run_g34_scaling_sweep.sh                         # ~25 min
python3 docs/plot_scaling_sweep.py logs/g34_scaling_sweep_*/SUMMARY.log \
        docs/g34_scaling_ycsb --cache=on
python3 docs/plot_scaling_sweep.py logs/g34_scaling_sweep_*/SUMMARY.log \
        docs/g34_scaling_ycsb/cache_off --cache=off
python3 docs/plot_scaling_extra.py logs/g34_scaling_sweep_*/SUMMARY.log \
        docs/g34_scaling_ycsb/extra
```
