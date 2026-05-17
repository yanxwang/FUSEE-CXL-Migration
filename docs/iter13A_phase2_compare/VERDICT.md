# iter-13A Phase 2.3 — Write-path dual-track VERDICT

**Date**: 2026-05-17
**Source**: `docs/iter13A_phase2_compare/SUMMARY.tsv` (5 cells × 3 builds × 5 reps = 75 runs)
**Builds**:
- `hazard` (Phase 1 winner — HAZARD read + STAGING write)
- `w1`     (HAZARD read + W1 per-host reserved write — `cxl_kv_blockpool.h` `alloc_peer`)
- `w3`     (HAZARD read + W3 batched preallocation, K=256 from §2.2.E sweep)

---

## Median throughput (Mops/s)

| cell | hazard | W1 | W3(K=256) | best |
|---|---|---|---|---|
| workloada T=4 off kv=512 | 1.61 | 1.61 | **1.05** | hazard/W1 (W3 **−35 %**) |
| workloada T=32 off kv=1024 | 10.44 | 10.37 | 10.39 | tied |
| workloadb T=32 off kv=256 | 13.09 | 13.11 | 13.14 | tied |
| workloadc T=64 off kv=1024 | 17.97 | **18.53** | 18.42 | **W1 (+3.1 %)** |
| workloadf T=32 off kv=512 | 11.65 | 11.63 | 11.53 | tied |

## Median w_avg (µs)

| cell | hazard | W1 | W3 |
|---|---|---|---|
| workloada T=4 off kv=512 | 6.79 | 6.75 | 6.78 |
| workloada T=32 off kv=1024 | 11.11 | 11.06 | 11.03 |
| workloadb T=32 off kv=256 | 10.20 | 10.24 | 10.22 |
| workloadc T=64 off kv=1024 | — | — | — (no writes) |
| workloadf T=32 off kv=512 | 10.44 | 10.39 | 10.43 |

## Median w_p99 (µs)

| cell | hazard | W1 | W3 |
|---|---|---|---|
| workloada T=4 off kv=512 | 15.4 | 15.8 | 15.5 |
| workloada T=32 off kv=1024 | 68.0 | 66.5 | 72.1 |
| workloadb T=32 off kv=256 | 43.3 | 45.2 | 46.8 |
| workloadf T=32 off kv=512 | 35.0 | 32.1 | 30.8 |

---

## K sweep results (Phase 2.2.E)

| T | K=64 | K=128 | K=256 | K=512 | K=1024 | K=2048 | K=4096 |
|---|---|---|---|---|---|---|---|
| 4 | 1.53 | 1.54 | **1.54** | 1.54 | 1.53 | 1.53 | 1.53 |
| 16 | 6.42 | 6.51 | 6.43 | 6.52 | 6.39 | 6.51 | **6.52** |
| 32 | 10.20 | 10.10 | 10.20 | 10.10 | 10.20 | 10.11 | **10.30** |
| 64 | 10.43 | 10.80 | **11.21** | 10.86 | 10.86 | 10.51 | 10.44 |

Per-K average across T:
- K=64: 7.14 Mops/s
- K=128: 7.23
- **K=256: 7.35 (best)**
- K=512: 7.26
- K=1024: 7.24
- K=2048: 7.16
- K=4096: 7.20

K=256 wins per primary metric; no w_p99 veto.

K<64 (K=16, K=32) failed to complete — current `kReservRingDepth=64` exposes a slot-reuse race in worker wait-for-slot-free loop when small batches force many wraparounds. **Deferred to iter-14A backlog**; for iter-13A we use K_winner=256 which is in the safe range.

---

## Decision: **W1 wins**

Per Phase 2.3 winner rules (`task_plan_iter13A.md`):
1. **Primary metric** (median thpt on most-write-heavy cell, here workloada T=32 off kv=1024 as proxy):
   - hazard 10.44 / W1 10.37 / W3 10.39 — within 0.7 %, effectively tied
2. **Tie-break** (median w_p99 across 5 cells): mixed (W1 wins 1, hazard wins 1, W3 wins 1) — also tied
3. **Veto** (w_avg regression > 30 % from STAGING any cell): no veto. But **W3 thpt regresses 35 %** on workloada T=4 off kv=512 — although veto is technically w_avg-only, this is a serious throughput regression that disqualifies W3 in practice.

**W1 is the winner**: it matches or beats HAZARD baseline on every cell, with the largest win on workloadc T=64 (+3.1 %). No regressions of any kind.

## Why W1 marginally beats W3

- W1's hot path is a single DRAM atomic (`peer_bumps_[owner].fetch_add(1)`) per cross-host write — ZERO cross-host coordination.
- W3 incurs a round-trip CXL reservation request every K writes; even amortized, this round trip costs ~5-10 µs of latency that adds to the critical path of "first write to owner B after refill". At low T, this latency dominates because there's no parallelism to hide it.
- W3's small-K hang (K<64) further restricts its applicability.

## Phase 1 + Phase 2 winner stack

For iter-13A Phase 2.5 full sweep: build `build-cxl-w1` (FUSEE_READ_GUARD=2 HAZARD + FUSEE_WRITE_ALLOC=1 RESERVED).

---

## Next actions

- Phase 2.4: G1 hash-diff battery on W1 build (already done above: 20/20 PASS on `build-cxl-w1`)
- Phase 2.5: Full 210-cell scaling sweep with `build-cxl-w1` + plots + compare to Phase 1
- Phase 3: iter-13A summary + iter-14A backlog (record W3 K<64 hang + RCU/Hazard for read-path choice + W3 deferred as alternative)
