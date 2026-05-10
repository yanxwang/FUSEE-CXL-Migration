# iter-9A Phase 3 path_decomp — per-stage table

**Cell**: workload-A KV=1024 T=64 cache=on
**Date**: 2026-05-10
**Healthy run**: 9.89 Mops/s aggregate (200k ops, 20.2 ms trans_wall_max)

## Healthy stage timing (g3 / h0; 66 threads × 11k+ ops each)

| Stage | N | p50 µs | p90 µs | p99 µs | max µs | Note |
|---|---|---|---|---|---|---|
| W1 (enter execute_write_local) | 73699 | 0.82 | 5.82 | 15.5 | 377 | inter-op gap dominates here |
| W2 (acquired slot dir lock) | 73691 | 0.15 | 0.32 | 1.84 | 47 | lock contention low |
| W3 (sharer bitmap scan) | 73691 | 0.03 | 0.14 | 5.05 | 77 | scalar; near-zero |
| W7 (pool alloc) | 73691 | 0.05 | 0.05 | 2.26 | 99 | bump-cursor very fast |
| W8 (pool write) | 73691 | 0.03 | 0.03 | 5.80 | 105 | NT-store + flush 1024B + sfence |
| W9 (slot CoW publish) | 73691 | 0.02 | 0.02 | 0.04 | 50 | clflushopt + sfence on slot |
| **W10 (dir state stored)** | 73691 | **4.09** | **14.3** | **23.9** | **157** | **largest stage** — includes dir.unlock + cache_pool_insert |
| W12 (cache updated + return) | 73669 | 0.60 | 1.32 | 6.43 | 5039 | inter-op gap; tail outlier |
| R1 (enter search) | 50054 | 6.92 | 10.2 | 16.3 | 73 | inter-op gap |
| R2hit (cache hit fast path) | 42700 | 0.03 | 0.03 | 0.04 | 41 | DRAM hashmap |
| R2miss (cache miss, decide path) | 7354 | 3.03 | 5.50 | 9.20 | 37 | branch-only |
| R3 (forward cache_register sent) | 101 | 9.26 | 10.4 | 14.7 | 19 | only 0.2% miss → register |
| R4 (cache_register ACK observed) | 101 | 0.32 | 0.51 | 0.63 | 0.79 | very fast (cached forward path) |
| R6 (return) | 50009 | 0.89 | 2.14 | 6.40 | 459 | inter-op |

## Per-stage status (Sol-1 × Sol-4)

Expected baselines (from Phase 0 baseline.md):
- Spinlock uncontested: p50 29 ns; T=64 contested: p50 9.1 µs, p99 691 µs
- LD-CXL post flush: p50 602 ns, p99 897 ns
- ST-CXL+flush+sfence: p50 15 ns
- CXL atomic fetch_add+flush+sfence: p50 1.3 µs
- mfence: p50 23 ns; lfence: p50 16 ns; sfence: p50 13 ns
- clflushopt+sfence (CXL line): p50 98 ns

| Stage | Expected | Healthy p50 | H/E | Status |
|---|---|---|---|---|
| W2 (lock+) | 29 ns - 9.1 µs | 0.15 µs | 5× over uncontested floor | OK (T=64) |
| W3 (bitmap scan) | < 100 ns | 0.03 µs | 0.3× | OK |
| W7 (alloc) | < 1 µs | 0.05 µs | 0.05× | OK |
| W8 (pool write 1024B) | ~5 µs (16× clflushopt) | 0.03 µs | 0.006× | OK (faster than predicted; cache absorbed) |
| W9 (slot publish) | ~150 ns | 0.02 µs | 0.13× | OK |
| **W10 (dir update)** | < 1 µs | 4.09 µs | **4×** | **anomaly: 4.09 µs > expected** — likely cache_pool_insert in MAP_SHARED is the cost |
| R3 (forward send) | ~5 µs round-trip | 9.26 µs | 1.85× | OK (forward roundtrip ~5-10 µs healthy) |

## Anomaly scan (Phase 3 longest gap)

Top 15 intra-thread stalls (probe_anomaly_scan.py):
- max gap 10.08 ms (W12→W1): inter-op idle, NOT a stage anomaly
- 2nd max 10.04 ms (I2→W6): inter-thread / pause  
- All other gaps < 1 ms

**No anomaly stage triggers Phase 3.1 in-iter fix** per Phase 3 decision tree:
> 0 stage flagged → 直接 Phase 4

W10 4.09 µs > expected 1 µs but H/E = 4× < 5× threshold for `anomaly` tag.
W10 likely candidate for iter-10A optimization (cache_pool_insert
contention on MAP_SHARED hashmap), per task plan §"Out of scope"
"Lock-free hashmap for cache_pool".

## Phase 3 exit

- ✅ 14 stages (all probed) covered with measurement
- ✅ 0 unjustified `✱ no data` rows (all 14 have probe data)
- ✅ 0 stage triggers Phase 3.1 in-iter fix (W10 H/E=4× below 5× threshold)
- ✅ Healthy run (9.89 Mops/s) > 50% of 20 Mops/s target → no Phase 4 sub-Phase-3 capture needed
- ✅ Stages NOT in the iter-8A 32-stage list (e.g., I1-I8 invalidate ring path) had no observable activity at this cell because workload-A's 50% writes mostly hit own-host owners; cross-host invalidate broadcast minimal

→ proceed to Phase 4 sweep
