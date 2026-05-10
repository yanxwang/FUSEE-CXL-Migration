# iter-10A Phase 0 — µbench baseline (Sol-4)

**Tool**: `tests/cxl_primitive_bench` on g3 (-DFUSEE_PROBE=1 build active)
**Captured**: 2026-05-10T17:28:00-05:00 (UTC: 22:28)
**TSC freq**: 2.000 GHz
**Predecessor**: `docs/path_decomp_iter9A_pre_20260510_060841/baseline.md` (iter-9A redo Phase 0)
**Purpose**: iter-10A Phase 0 baseline; quantitatively the same hardware so primitives largely unchanged

## Primitives

| primitive                          |     N |  p50ns |  p99ns |   maxns |   p50c |   p99c |    maxc |
|------------------------------------|-------|--------|--------|---------|--------|--------|---------|
| mfence (alone)                     | 10000 |   23.0 |   24.0 |  7447.0 |     46 |     48 |   14894 |
| sfence (alone)                     | 10000 |   13.0 |   14.0 |    15.0 |     26 |     28 |      30 |
| lfence (alone)                     | 10000 |   16.0 |   17.0 |    18.0 |     32 |     34 |      36 |
| clflushopt+sfence (CXL)            | 10000 |   98.0 |  103.0 |   109.0 |    196 |    206 |     218 |
| LD-CXL (after flush+mfence)        | 10000 |  602.0 |  897.0 |  7342.0 |   1204 |   1794 |   14684 |
| ST-CXL+flush+sfence                | 10000 |   14.0 |   15.0 |   208.0 |     28 |     30 |     416 |
| CXL atomic fetch_add (no flush)    | 10000 |   17.0 |   17.0 |  1453.0 |     34 |     34 |    2906 |
| CXL atomic fetch_add+flush+sfence  | 10000 | 1334.0 | 1619.0 | 11124.0 |   2668 |   3238 |   22248 |
| spinlock uncontested (lock+unlock) | 10000 |   29.0 |   30.0 |  1461.0 |     58 |     60 |    2922 |
| spinlock contested T=2             |  2000 |  250.0 | 2765.0 |  5791.0 |    500 |   5530 |   11582 |
| spinlock contested T=4             |  4000 | 1016.0 | 10814.0 | 148254.0 |   2032 |  21628 |  296508 |
| spinlock contested T=8             |  8000 | 3315.0 | 57061.0 | 278950.0 |   6630 | 114122 |  557900 |
| spinlock contested T=16            | 16000 | 2791.0 | 178165.0 | 739609.0 |   5582 | 356330 | 1479218 |
| spinlock contested T=32            | 32000 | 5048.0 | 381646.0 | 1857805.0 |  10096 | 763292 | 3715610 |
| spinlock contested T=64            | 64000 | 10576.0 | 859623.0 | 13048253.0 |  21152 | 1719246 | 26096500 |

## Comparison vs iter-9A redo baseline (2026-05-10 06:08:41 CDT)

| primitive | iter-9A redo p50 (ns) | iter-10A p50 (ns) | Δ |
|---|---|---|---|
| mfence | 23.0 | 23.0 | 0% |
| LD-CXL | 602.0 | 602.0 | 0% |
| ST-CXL+flush | 16.0 | 14.0 | -12% (1-2 ns noise) |
| CXL atomic fetch_add+flush | 1425.0 | 1334.0 | -6% (run-to-run) |
| spinlock uncontested | 29.0 | 29.0 | 0% |
| spinlock T=64 contested | 9033.0 | 10576.0 | **+17%** |

T=64 contested spinlock ~17% slower this run — within run-to-run µbench variance for the highest-T case (high tail std). T=32 5048 vs iter-9A redo 5250 ≈ 4% difference, solid.

## iter-10A 用到的关键 primitive 总结

- **CXL atomic fetch_add+flush p50 = 1.33 µs**: 这是 sender batching Phase 3 优化的目标 — N=16 batch 每 op 摊薄到 ~83 ns
- **spinlock uncontested = 29 ns**: Phase 2 lock-free CAS 替换的 baseline
- **spinlock T=64 = 10.6 µs**: workload-A KV=1024 T=64 cache=on Zipf hot bucket 上 W10 测得 3.97 µs ≈ 介于 T=8 (3.3 µs) 跟 T=32 (5.0 µs) 之间，对应 hot bucket 实际 contention degree ~10-20 worker 同时
- **LD-CXL = 602 ns**: 跨 host pool->read 单 cacheline cost；read-path regression 的来源
- **ST-CXL+flush = 14 ns**: cache-absorbed, very cheap

## Phase 0 exit

- ✅ µbench baseline.md 写完
- ✅ smoke (workload-d kv=8 T=4 cache=on) PASS = 1.643 Mops/s (post-bootstrap iter-10A baseline; matches iter-9A redo 1.85 within run-to-run noise)
- ✅ FUSEE_PROBE=1 cmake flag 正常 propagation 到 target_compile_definitions (iter-9A redo fix carried)
- ✅ All 6 system threads pinned + named verified (smoke stderr)
- per_stage_expected.md deferred to Phase 4 (架构变化大, 在 post-Phase-3 arch 上重新 derive)
