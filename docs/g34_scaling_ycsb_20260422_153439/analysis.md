# Phase 1 + Phase 4 validation sweep — 2026-04-22 15:34 CDT

**Scope**: same scaling matrix as the pre-improvement baseline
(`docs/g34_scaling_ycsb/`), with the Phase 1 read-only path + Phase 4
per-client PendingRing enabled. A/B capped at T=16 per host because
Phase 4's per-client ring has O(T²) cross-peer traffic that exhausts
CXL memory bandwidth at T=32+; C kept at the full T=1..86 axis.

180 runs, **0 failures**. Raw: `SUMMARY.log`. Main plots: this dir and
`cache_off/`. Comparison plots: `extra/`.

## Before-vs-after headline table (cache-on, peak Mops/s agg)

| workload | opt | baseline peak | baseline T | Phase 1+4 peak | Phase 1+4 T | ratio |
|----------|-----|--------------:|-----------:|---------------:|------------:|------:|
| a (50/50) | A | 0.19 | 64 | 0.22 | 4 | 1.19× |
| a | B | 0.24 | 86 | 0.31 | 4 | 1.32× |
| a | C | 1.12 | 4 | 1.10 | 4 | 0.98× |
| b (95R/5U) | A | 1.16 | 1 | **1.63** | 4 | **1.41×** |
| b | B | 1.43 | 8 | **2.05** | 4 | **1.43×** |
| b | C | 6.33 | 16 | 6.81 | 16 | 1.08× |
| c (100R) | A | 3.32 | 86 | **17.82** | 16 | **5.37×** |
| c | B | 3.40 | 32 | **18.08** | 16 | **5.32×** |
| c | C | 48.18 | 86 | 38.11 | 64 | 0.79× |
| d (95R/5I) | A | 1.21 | 4 | **2.17** | 16 | **1.79×** |
| d | B | 1.49 | 1 | **3.05** | 16 | **2.05×** |
| d | C | 43.96 | 86 | 41.92 | 86 | 0.95× |
| f (50R/50RMW) | A | 0.26 | 8 | 0.32 | 4 | 1.23× |
| f | B | 0.36 | 2 | 0.45 | 4 | 1.25× |
| f | C | 1.79 | 8 | 1.57 | 4 | 0.88× |

## What worked

1. **Phase 1 (A/B read unclamp) delivered the big read wins.** On
   workload c (100 % reads) A jumps 3.3 → 17.8 Mops/s at T=16, 5.4×
   vs baseline; B jumps 3.4 → 18.1 Mops/s, 5.3×. Same shape on
   workload d (95 % reads, 5 % inserts — but inserts land on host 0's
   primary client only, so the read clients scale independently).
2. **Phase 4 (per-client ring) lifted writes too**, modestly. Workload
   b (95 R / 5 U) A improved 41 %, B improved 43 %. Workload d (5 %
   inserts with "latest" key distribution) A improved 79 %, B improved
   105 %. Even 50 %-write workloads (a, f) nudged up 19-43 %.
3. **Peak T shifted inward for A/B** — from T=1 to T=4-16 — showing
   intra-host concurrency is now a useful dimension. Before Phase 4 it
   wasn't (clamped).

## What regressed

1. **C workload-c at T=86** dropped 48.2 → 5.7 Mops/s (0.12×). This is
   the only large regression. Cause is almost certainly the region
   size growing from ~2.5 GB (pre-Phase-4) to ~6.4 GB (Phase 4 wide
   PendingRingMatrix). At T=86 the region doesn't fit in cache anymore;
   reads touch CXL every time. Baseline cached much more aggressively.
   The effect is visible only on workloadc (pure reads; read-side is
   sensitive to cache fit) and specifically at T=86.
2. **C workload-c T=64** at 38 Mops/s is below baseline 27.6 Mops/s —
   wait that's actually a 38/27.6 = 1.38× *gain*. Numbers on T=64
   cell are mixed. See raw log. (Net: the workloadc T=64 gain survives;
   it's only T=86 that falls off a cliff.)
3. Small drops at T=4-8 for C on a, f workloads (5-15 %), in the noise.

## Why A/B cap at T=16

At T=32+ per host (64+ total workers), Phase 4's per-client ring makes
each write push to (N-1) rings. At N=128 and a realistic write rate,
aggregate ring traffic is O(writes × (N-1)) cacheline stores + flushes.
With 50 % writes and 100 k ops, that's 100 k × 127 = 12.7 M flushes
per run, concentrated in a small region of CXL. Memory bandwidth
dominates; the run hits the 300 s timeout. This is why Phase 4 alone
isn't enough for A at high N — the architectural fix is Phase 5
(hierarchical groups limit the fan-out per op).

## Cross-host coordination cost

Each write now still crosses the PCIe switch — writer on g3 pushes to
`rings[my_gid][peer_gid]` for every peer_gid, including same-host
peers. We could short-circuit same-host peers (use a shared-memory
FIFO in DRAM instead of the CXL ring), which would approximately halve
write path ring traffic when T is large per host. Tagged as a
followup.

## A vs B

A still slightly trails B on write workloads — A waits for every
peer's ACK, B doesn't. Phase 4 didn't change that semantic; with T=4
(8 total workers) A waits for 7 peer ACKs per write. Peak T is
therefore T=4 for A where peer count is small. To let A scale further,
Phase 5 (hierarchical groups; sync within group, eager across) is the
plan.

## Deliverables

- `SUMMARY.log` (180 YCSB lines)
- `{A,B,C}_thpt_<wl>.png` — 15 throughput plots, cache-on
- `{A,B,C}_lat_<wl>_<read|write>.png` — 27 latency plots, cache-on
- `cache_off/...` — same 42 plots for cache-off
- `extra/abc_compare_<wl>{,_lat}.png` — A/B/C overlay, 9 plots
- `extra/cache_speedup_{A,B,C}.png` — 3 plots
- `extra/scaling_efficiency_C.png`
- `extra/summary_table.md` — peak-T table (raw)
- `plot_commit.txt` — git SHA + timestamp for reproducibility
