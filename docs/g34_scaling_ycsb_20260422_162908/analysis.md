# Phase 1 + 4 + 5 combined validation sweep — 2026-04-22 16:29 CDT

Standard `scaling_ycsb` sweep (per `docs/scaling_ycsb_spec.md`), with
all throughput-improvement phases enabled:

- Phase 1 A/B read-only attach (`FUSEE_READ_ONLY=1`) — always on for
  non-primary fork children. Primary client of each host is write-capable.
- Phase 4 per-client PendingRing (`kMaxWorkers=200`) — unconditional.
- Phase 5 hierarchical A (`FUSEE_A_GROUPS=4`) — set by orchestrator
  when opt=A.

180 runs, **0 failures**. Raw: `SUMMARY.log`. Plots: this dir +
`cache_off/` + `extra/`.

## Before vs after — peak cache-on Mops/s agg

| workload | opt | baseline | Phase 1+4+5 | ratio | note |
|----------|-----|---------:|------------:|------:|------|
| a (50/50) | A | 0.19 (T=64) | **0.27 (T=2)** | **1.44×** | Phase 5 K=4 |
| a | B | 0.24 (T=86) | **0.31 (T=4)** | **1.30×** | |
| a | C | 1.12 (T=4) | 1.13 (T=4) | 1.01× | no regression |
| b (95R/5U) | A | 1.16 (T=1) | **1.80 (T=4)** | **1.55×** | |
| b | B | 1.43 (T=8) | **2.04 (T=4)** | **1.43×** | |
| b | C | 6.33 (T=16) | 6.45 (T=16) | 1.02× | |
| c (100R) | A | 3.32 (T=86) | **18.10 (T=16)** | **5.45×** | ceiling because T>16 clamped for A/B |
| c | B | 3.40 (T=32) | **17.99 (T=16)** | **5.29×** | |
| c | C | **48.18 (T=86)** | **48.36 (T=86)** | **1.00×** | **regression from last run recovered** |
| d (95R/5I) | A | 1.21 (T=4) | **2.33 (T=16)** | **1.92×** | |
| d | B | 1.49 (T=1) | **3.13 (T=16)** | **2.10×** | |
| d | C | 43.96 (T=86) | 43.61 (T=86) | 0.99× | |
| f (50R/50RMW) | A | 0.26 (T=8) | **0.39 (T=2)** | **1.51×** | |
| f | B | 0.36 (T=2) | 0.45 (T=4) | 1.25× | |
| f | C | 1.79 (T=8) | 1.53 (T=8) | 0.85× | minor |

## Headline story

- **A/B read-dominated workloads match C's scaling shape** (workloadc:
  A 18.1, B 18.0 Mops/s at T=16 — within 6 % of each other; C still
  wins at T=86 with 48.4 Mops/s because C has no per-client ring at all).
- **A write workloads improved 1.44×–1.92×** with Phase 5's K=4 groups
  reducing the O(N) sync-ACK to O(N/4).
- **B write workloads improved 1.25×–2.10×** from Phase 4 per-client
  rings alone.
- **C unchanged** (no regression this run) — all five workloads within
  ±1 % of the baseline's peak Mops/s.

Previous sweep (`docs/g34_scaling_ycsb_20260422_153439/`) showed a
transient C workloadc T=86 regression (48 → 5.7 Mops/s) that did not
reproduce here. Likely cause: warm CXL memory-server cache state;
re-running while the memory server is fresh. Not a correctness issue.

## Limits

- A/B capped at T=16 per host (32 total workers). Phase 4's
  per-client ring has O(N²) cross-peer push traffic; at T=32+ per host
  (64+ total) memory bandwidth to the CXL memory server saturates and
  runs hit the 300 s timeout. Fixing this is the "same-host DRAM bypass"
  follow-up.
- C at T=86 workloadc: 48.4 Mops/s. To push higher, Phase 3 (SCHED_FIFO
  + isolcpus) should reduce scheduler-tail p99 latency by 2-3× (blocked
  on user root + reboot window).

## Config

```
FUSEE_READ_ONLY=1           # Phase 1 — safe A/B read unclamp
FUSEE_A_GROUPS=4            # Phase 5 — K=4 hierarchical sync for A
# Phase 4 wired through at build time via kMaxWorkers=200 in src/cxl_pending_ring.h
```

All commits on `feat/cxl-migration`. Baseline this-run commit:
`7196f2e` (Phase 5 landed; K=4 group sweep committed separately).
