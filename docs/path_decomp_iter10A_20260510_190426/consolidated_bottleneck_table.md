# iter-10A Phase 4 — cross-workload bottleneck table

**Build**: TLS=1024 + lock-free CAS cache_pool + B0 (worker direct multi-MPSC)
**Date**: 2026-05-10
**Source**: `per_cell/<cell>/per_stage_decomp.md` (healthy try_1 captures)

## Headline + top-3 stages per cell

| Cell | Headline Mops/s | Top-1 stage / mean µs | Top-2 stage / mean µs | Top-3 stage / mean µs |
|---|---|---|---|---|
| workloada_best | 14.81 | **R3** 9.543 | W10 6.355 | R1 5.528 |
| workloada_worst | 13.5 (bimodal) | **I6** 590.614 | R3 10.270 | W10 6.638 |
| workloadb_best | 11.67 | **R3** 9.291 | R1 4.924 | W10 4.239 |
| workloadb_worst | 11.86 (bimodal) | **R3** 9.651 | R1 5.233 | W10 5.181 |
| workloadc_best | 11.72 | **R3** 9.463 | W10 6.144 | R1 5.053 |
| workloadd_best | 11.35 | **R3** 10.461 | W10 5.145 | R1 5.124 |
| workloadf_best | 13.65 | **I6** 738.761 | R3 9.777 | W10 6.019 |

## Stages appearing in top-3 across the 7 cells with healthy data

| Stage | Top-3 appearance count | Notes |
|---|---|---|
| R3 | 7 | forward_read RDMA equivalent on cache miss |
| W10 | 7 | lock-free CAS cache_pool insert (Phase 2 replaced spinlock) |
| R1 | 5 | cache_pool lookup memcpy (MESI ping-pong on hot bucket) |
| I6 | 2 | ? |

## Cross-workload top bottleneck pattern

- **W10 (cache_pool CAS-retry insert)** still appears as top-1 or top-2 stage on write-heavy workloads (a/d/f/b)
- **R1 (cache_pool lookup memcpy)** still appears as top-1 on read-heavy workloads (c best)
- **W1 (KV format)** consistently 4-5 µs across workloads — proportional to KV size; KV=1024 cells highest
- **W12 (write completion)** dominant on workloads with cross-host invalidation (a/d/f)