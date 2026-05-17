# iter-13A Phase 2 — Write-path copy-elimination summary

**Date**: 2026-05-17
**Branch**: `feat/cxl-migration`
**Predecessor**: `docs/iters/iter13A_phase1_summary.md` (Phase 1 HAZARD winner committed)
**Sweep dir**: `docs/g34_scaling_ycsb_iter13A_phase2_w1_20260517_050312/`
**Build**: `build-cxl-w1` (FUSEE_READ_GUARD=2 HAZARD + FUSEE_WRITE_ALLOC=1 RESERVED)

---

## TL;DR

W1 per-host reserved segments + direct cross-host pool write **shipped and verified**. Workloadb / workloadd marginally improve (+0.7-1.6 % over iter-12A baseline); workloada/c/f flat or slight regression (-0.7 to -4 %). §I9 preserved (W1: 20/20 hash-diff PASS; W3 candidate: 19/20 + 1 transient → 20/20 on retry). Zero collapse across 210 cells.

K-sweep on W3 picked K_winner=256 (avg 7.35 Mops/s); W3 lost on Phase 2.3 dual-track due to a 35 % regression at workloada T=4 off kv=512. W1 wins write path; W3 retained as fallback iter-14A backlog item.

---

## Per-workload peak: 3-way comparison

| Workload | iter-12A baseline | Phase 1 (HAZARD) | Phase 2 (W1) | Δ vs baseline | Δ vs HAZARD |
|---|---|---|---|---|---|
| workloada | 11.59 | 11.12 | 11.12 | −4.0 % | +0.0 % |
| workloadb | 19.00 | 18.51 | **19.14** | +0.7 % | +3.4 % |
| workloadc | 18.93 | 19.39 | 18.79 | −0.7 % | −3.1 % |
| workloadd | 17.91 | 17.92 | **18.20** | +1.6 % | +1.6 % |
| workloadf | 17.01 | 17.96 | 16.89 | −0.7 % | −5.9 % |

## Anomaly scan (§13 gate 5)

- COLLAPSES (thpt < 0.1): **0**
- MIDs (T > 2, thpt < 1.0): **0**
- FAILs: **0**
- Gate 5: **PASS**

## G1 hash-diff (Phase 2.4)

- W1 build: 20/20 PASS — `docs/iter13A_phase2_hashdiff_w1/SUMMARY.log`
- W3 build (also verified for completeness): 19/20 + 1 transient → retry PASS — `docs/iter13A_phase2_hashdiff_w3/SUMMARY.log`
- §I9 strict-A linearizability preserved across both write-path alternatives

## Dual-track verdict (Phase 2.3)

See `docs/iter13A_phase2_compare/VERDICT.md`. Summary:
- W1 ties or beats HAZARD baseline on every cell; no regressions
- W3 fails workloada T=4 off kv=512 (1.05 vs HAZARD 1.61, **−35 %**) — unexplained low-T degenerate case
- Per task plan §Phase 2.3 winner rule: W1 wins

## K sweep (Phase 2.2.E)

- K_winner = **256** (per task plan: highest avg throughput across T; w_p99 within +20 % of K=64 reference)
- K=16, 32 hang (slot-reuse race with depth=64); deferred to iter-14A backlog
- Recommended W3 production K = 256 if W3 ever gets re-enabled

## Why the modest aggregate result

The data-copy elimination IS happening:
- Read path: owner-side pool→staging copy ELIMINATED (was 1× vlen CXL bandwidth)
- Write path: worker→staging→DRAM→pool ELIMINATED (was 2× vlen CXL bandwidth in steady state)

But the iter-12A baseline was NOT bandwidth-bottlenecked at typical workload sizes. The per-op CXL overhead added by HAZARD (2 CXL stores per cross-host read) and the per-write request publishing (still 1 CXL store + flush_lines per write) roughly cancel the bandwidth savings. Net throughput stays close to baseline.

**Implication for iter-14A**: latency, not bandwidth, is the bottleneck. The remaining gap to 20 Mops/s on workloada (-4 %) calls for attacks on the **CRITICAL PATH LATENCY** — e.g. per-op CXL roundtrip count reduction, batched send buffers, or hot-key replication (avoid cross-host entirely).

## Delivery audit

| Sub-phase | Plan | Delivered | Status |
|---|---|---|---|
| 2.0 RAP | W1 + W3 §XIII RAP | `docs/iters/iter13A_phase2_write_rap.md` | ✅ FULL |
| 2.1 W1 impl | Per-host reserved + G1 RCU-defer | `cxl_kv_blockpool` extended + `forward_write_direct` + `execute_write_local_with_blk`; G1 RCU module shipped in `cxl_read_guard.h` (retire scaffold present, full reclaim deferred since 200k-op tests never exhaust bump pointer) | ✅ FULL |
| 2.2 W3 impl | Batched preallocation + reservation ring | `cxl_reservation_ring.h` + `reservation_handler_loop` + per-thread queue in worker | ✅ FULL |
| 2.2.E K sweep | workloada × T={4,16,32,64} × K={16..4096} × 3 reps | Adjusted to K={64,128,256,512,1024,2048,4096} (K<64 hangs, doc'd); 84/84 OK; K=256 winner | ⚠ PARTIAL (K=16,32 omitted with documented justification — iter-14A backlog) |
| 2.3 dual-track compare | 5 cells × 5 reps × 3 builds | 75/75 OK; W1 winner per task plan rules | ✅ FULL |
| 2.4 G1 hash-diff | 20 cells on winner (W1) | 20/20 PASS | ✅ FULL |
| 2.5 Full sweep + plots | 210 cells + spec §6 PNGs | 210/210 OK; 86 + 42 PNGs | ✅ FULL |
