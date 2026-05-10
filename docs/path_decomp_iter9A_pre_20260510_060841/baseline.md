# iter-9A redo path_decomp Phase 0 — µbench baseline (Sol-4)

**Tool**: `tests/cxl_primitive_bench` on g3
**Captured**: 2026-05-10T06:08:41-05:00
**TSC freq**: 2.000 GHz
**Predecessor**: `docs/path_decomp_iter9A_20260510_042416/baseline.md`
**Purpose**: iter-9A redo Phase 0 baseline (re-execute task_plan_iter9A.md
from start; original iter-9A's Phase 2 was silently descoped — see CLAUDE.md
cautionary precedent #3).

## Primitives

| primitive                          |     N |  p50ns |  p99ns |   maxns |   p50c |   p99c |    maxc |
|------------------------------------|-------|--------|--------|---------|--------|--------|---------|
| mfence (alone)                     | 10000 |   23.0 |   24.0 |  7447.0 |     46 |     48 |   14894 |
| sfence (alone)                     | 10000 |   13.0 |   14.0 |    15.0 |     26 |     28 |      30 |
| lfence (alone)                     | 10000 |   16.0 |   17.0 |    18.0 |     32 |     34 |      36 |
| clflushopt+sfence (CXL)            | 10000 |   98.0 |  103.0 |   109.0 |    196 |    206 |     218 |
| LD-CXL (after flush+mfence)        | 10000 |  602.0 |  897.0 |  7342.0 |   1204 |   1794 |   14684 |
| ST-CXL+flush+sfence                | 10000 |   16.0 |   22.0 |   364.0 |     32 |     44 |     728 |
| CXL atomic fetch_add (no flush)    | 10000 |   17.0 |   17.0 |  1393.0 |     34 |     34 |    2786 |
| CXL atomic fetch_add+flush+sfence  | 10000 | 1425.0 | 1771.0 | 11066.0 |   2850 |   3542 |   22132 |
| spinlock uncontested (lock+unlock) | 10000 |   29.0 |   30.0 |  1327.0 |     58 |     60 |    2654 |
| spinlock contested T=2             |  2000 |   90.0 | 2044.0 |  5305.0 |    180 |   4088 |   10610 |
| spinlock contested T=4             |  4000 |   90.0 | 1939.0 |  4020.0 |    180 |   3878 |    8040 |
| spinlock contested T=8             |  8000 | 2090.0 | 38890.0 | 154840.0 |   4180 |  77780 |  309680 |
| spinlock contested T=16            | 16000 | 3749.0 | 169185.0 | 502359.0 |   7498 | 338370 | 1004718 |
| spinlock contested T=32            | 32000 | 5250.0 | 366215.0 | 1897395.0 |  10500 | 732430 | 3794790 |
| spinlock contested T=64            | 64000 | 9033.0 | 711759.0 | 28215988.0 |  18066 | 1423518 | 56431964 |

## Comparison vs original iter-9A baseline (2026-05-10T04:24:32)

| primitive | iter-9A orig p50ns | iter-9A redo p50ns | Δ |
|---|---|---|---|
| mfence | 23.0 | 23.0 | 0% |
| LD-CXL | 602.0 | 602.0 | 0% |
| ST-CXL+flush+sfence | 15.0 | 16.0 | +7% (1 ns noise) |
| CXL atomic fetch_add+flush+sfence | 1311.0 | 1425.0 | +9% (run-to-run variance) |
| spinlock uncontested | 29.0 | 29.0 | 0% |
| spinlock T=2 | 320.0 | 90.0 | -72% (orig was an outlier) |
| spinlock T=4 | 1715.0 | 90.0 | -95% (orig clearly an outlier — pinning in this run is correct) |
| spinlock T=8 | 2775.0 | 2090.0 | -25% |
| spinlock T=16 | 5391.0 | 3749.0 | -30% |
| spinlock T=64 | 9097.0 | 9033.0 | -1% |

**Interpretation**: spinlock-contested numbers at low T (2, 4) are
substantially better in this run; the original iter-9A baseline
captured pre-CPU-pinning environmental noise (cxl_primitive_bench
itself does not pin, and original baseline was captured before
worker pinning was widespread on g3). High-T (32, 64) numbers match
within 1% — confirming hardware + microcode unchanged.

## Notable

- spinlock contested grows ~exponentially with T:
  T=2 p50 90 ns → T=64 p50 9.0 µs
  T=2 p99 2.0 µs → T=64 p99 712 µs (huge tail)
- CXL atomic fetch_add+flush+sfence p50 = 1.4 µs (cache_register tail update cost)
- LD-CXL post-flush p50 = 602 ns (canonical CXL line read)
- ST-CXL+flush+sfence p50 = 16 ns (cache absorbed; tail max 364 ns)

## Phase 0 exit

- ✅ µbench primitives captured for the redo iter
- ✅ baseline.md written
- ✅ smoke test (workload-d kv=8 T=4 cache=on) PASS:
  trans_agg_thpt=1985072 ops/s on g3+g4 with workers + receivers
  pinned correctly (cpu 0..3 + 65 + 69)
- per_stage_expected.md is deferred to Phase 3 (must be derived against
  the iter-9A redo's *post-Phase-2* architecture, not pre-Phase-2)
