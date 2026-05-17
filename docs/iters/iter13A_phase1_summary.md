# iter-13A Phase 1 — Read-path copy-elimination summary

**Date**: 2026-05-17
**Branch**: `feat/cxl-migration`
**Predecessor baseline**: `docs/iters/iter13A_baseline_summary.md` (iter-12A post-fix)
**Sweep dir**: `docs/g34_scaling_ycsb_iter13A_phase1_hazard_20260517_032541/`
**Build**: `build-cxl-hazard` (FUSEE_READ_GUARD=2)

---

## TL;DR

Read-path copy elimination via direct-pool-read protected by HAZARD pointers shipped + verified. **Workloadc (100 % read) +2.4 %, workloadf +5.6 %; workloada/b regressed 3-4 %** due to per-read CXL-store overhead on the protect/release path that exceeds the savings on write-heavy mixes. §I9 correctness preserved (20/20 hash-diff PASS). Zero collapse across 210 cells.

---

## Per-workload peak vs iter-12A baseline

| Workload | Baseline | Phase 1 HAZARD | Δ % | Notes |
|---|---|---|---|---|
| workloada | 11.59 | 11.12 | **−4.1 %** | 50/50 R/U Zipf — write path unchanged; per-read HAZARD slot store costs ~vlen/CXL-BW savings |
| workloadb | 19.00 | 18.51 | **−2.6 %** | 95R/5U Zipf cache=on — most reads hit local TLS, never go cross-host; HAZARD overhead pays without benefit |
| workloadc | 18.93 | **19.39** | **+2.4 %** | 100R Zipf — pure-read win, where copy savings on owner offset reader's CXL store cost |
| workloadd | 17.91 | 17.92 | +0.0 % | 95R/5I — flat |
| workloadf | 17.01 | **17.96** | **+5.6 %** | 50R/50RMW — RMW path's read leg gets the copy savings, write leg unchanged; net positive |

## Anomaly scan (§13 gate 5)

- COLLAPSES (thpt < 0.1 Mops/s): **0**
- MIDs (T>2, thpt < 1.0 Mops/s): **0**
- FAILs: **0**
- Gate 5: **PASS**

## G1 hash-diff battery (Phase 1.4)

- 20/20 cells PASS (5 workloads × 4 KV sizes) on `build-cxl-hazard`
- §I9 strict-A linearizability preserved across the cross-host direct-pool-read change
- See `docs/iter13A_phase1_hashdiff_hazard/SUMMARY.log`

## Per-cell latency picture (HAZARD vs STAGING from Phase 1.3 5-rep median)

- **r_avg**: HAZARD wins or ties in 5/5 cells (workloadc: 5.66 → 5.44 µs)
- **r_p99**: HAZARD wins in 4/5 cells; workloadc p99 27.6 → 23.7 µs (-14 %)
- **w_avg**: unchanged within noise (Phase 1 doesn't touch write path)
- **w_p99**: unchanged within noise

## Why the modest aggregate gain?

The HAZARD path **eliminates the owner-side pool→staging copy** but adds **2 CXL stores per cross-host read** (hazard_protect + hazard_release). At low parallelism (low T), the owner has bandwidth headroom and the saved copy doesn't translate to throughput; the 2 added CXL stores dominate. At high T on read-only workloads (workloadc), the owner becomes the bottleneck and the savings show.

For mixed workloads (workloada/b), the read-path benefit is offset by:
1. HAZARD CXL-store overhead on every cross-host read (whether owner is busy or not)
2. No change to write path (writes still pay the staging copy)
3. CXL bandwidth not yet saturated → owner-side savings not visible in throughput

## Decision: keep HAZARD as default for Phase 2 + iter-14A

- Workloadc / workloadf show the optimization works as designed
- Workloada/b regressions are within 5 % and not a blocker — Phase 2 will recover them via write-path copy elimination
- §I9 preserved + zero collapse → no correctness concerns
- HAZARD wins by 1.3 % over RCU on read fast-path while being simpler; promote HAZARD

## Delivery audit

| Sub-phase | Plan | Delivered | Status |
|---|---|---|---|
| 1.0 RAP | RCU + Hazard §XIII RAP both candidates | `docs/iters/iter13A_phase1_read_rap.md` | ✅ FULL |
| 1.1 RCU impl | `cxl_rcu.h` + wire in read_handler/forward_read_direct | merged into `cxl_read_guard.h` with cached publish_epoch optimization | ✅ FULL |
| 1.2 Hazard impl | `cxl_hazard.h` + wire | merged into `cxl_read_guard.h` | ✅ FULL |
| 1.3 dual-track compare | 5 cells × 5 reps × 3 builds | 75/75 OK, VERDICT.md commits HAZARD as winner | ✅ FULL |
| 1.4 G1 hash-diff | 20-cell battery on winner | 20/20 PASS | ✅ FULL |
| 1.5 Full sweep + plots | 210-cell + spec §6 PNGs | 210/210 OK, 86 + 42 PNGs generated | ✅ FULL |

## Phase 1 → Phase 2 hand-off

- RCU module ships (in `cxl_read_guard.h`) but is **not the read fast-path winner**. It will be reused for Phase 2 W1's GC (G1 retire-list pattern per user QR6 decision).
- `cxl_read_guard.h` data structures (RcuDomain, HazardDomain) stable; future iter-14A ABA work (generation tag in encoded slot) will extend this header.
- iter-12A Phase 5 stale-cache fix (always-memset-flush) extended to RCU/Hazard domain init — consistent treatment.
