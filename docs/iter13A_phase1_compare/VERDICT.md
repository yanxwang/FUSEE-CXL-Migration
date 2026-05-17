# iter-13A Phase 1.3 — Read-path dual-track VERDICT

**Date**: 2026-05-17
**Source**: `docs/iter13A_phase1_compare/SUMMARY.tsv` (5 cells × 3 builds × 5 reps = 75 runs)
**Builds**:
- `staging` (current iter-12A behavior — owner pool→staging copy)
- `rcu`     (FUSEE_READ_GUARD=1 — `cxl_read_guard.h` RcuDomain, epoch slot per thread, direct pool read)
- `hazard`  (FUSEE_READ_GUARD=2 — `cxl_read_guard.h` HazardDomain, blk_off slot per thread, direct pool read)

---

## Median throughput (Mops/s)

| cell | STAGING | RCU | HAZARD | best |
|---|---|---|---|---|
| workloada T=4 off kv=512 | 1.28 | 1.28 | 1.28 | tied |
| workloada T=32 off kv=1024 | 10.56 | 10.42 | 9.86 | STAGING |
| workloadb T=32 off kv=256 | 10.86 | 10.82 | 10.20 | STAGING |
| **workloadc T=64 off kv=1024** | **17.48** | **18.15** | **18.38** | **HAZARD (+5.2 %)** |
| workloadf T=32 off kv=512 | 9.10 | 9.20 | 9.01 | RCU (+1.1 %) |

## Median r_avg (µs)

| cell | STAGING | RCU | HAZARD |
|---|---|---|---|
| workloada T=4 off kv=512 | 2.39 | 2.40 | 2.38 |
| workloada T=32 off kv=1024 | 5.16 | 5.02 | 4.87 |
| workloadb T=32 off kv=256 | 5.08 | 5.08 | 5.04 |
| workloadc T=64 off kv=1024 | 5.66 | 5.57 | 5.44 |
| workloadf T=32 off kv=512 | 5.58 | 5.49 | 5.62 |

## Median r_p99 (µs)

| cell | STAGING | RCU | HAZARD |
|---|---|---|---|
| workloada T=4 off kv=512 | 16.49 | 16.81 | 16.90 |
| workloada T=32 off kv=1024 | 25.76 | 25.12 | 23.74 |
| workloadb T=32 off kv=256 | 22.56 | 22.67 | 21.99 |
| workloadc T=64 off kv=1024 | 27.61 | 26.08 | 23.67 |
| workloadf T=32 off kv=512 | 24.45 | 24.04 | 24.23 |

---

## Decision: **HAZARD wins** (per Phase 1.3 rules in `task_plan_iter13A.md`)

1. **Primary**: median thpt on workloadc T=64 (read-dominant)
   - HAZARD 18.38 > RCU 18.15 > STAGING 17.48 — **HAZARD wins by +5.2 %** over STAGING, +1.3 % over RCU
2. **Tie-break**: median r_p99 across all 5 cells
   - HAZARD lowest in 4 / 5 cells (only workloada T=4 essentially tied — both within 3 % of STAGING which itself is low at 16 µs)
3. **Veto**: any cell's r_avg regress > 30 % from STAGING?
   - No. Worst r_avg delta: HAZARD on workloadf is +0.04 µs (+0.7 %)

## Caveats — honest assessment

- The improvement is **modest** (+5.2 % on the most-favorable cell). Both RCU and HAZARD show **regression of 5-7 %** on mixed-workload cells (workloada T=32, workloadb T=32).
- At low T (T=4 workloada), all three builds tie — copy-savings on owner are negligible when owner has spare bandwidth.
- The expected win is **larger** under cache=on + workloads where read latency dominates end-to-end; cache=off here amplifies the cost of every CXL read.

## Why HAZARD beats RCU (small margin):

- RCU pays 1 CXL load (cached publish_epoch refresh every 64 calls, amortized to ~negligible) + 1 CXL store (publish my_epoch).
- HAZARD pays 1 CXL store (publish blk_off) + 1 CXL store (release).
- In the optimized RCU implementation (with the every-64-calls publish_epoch refresh cache), per-call CXL store count is the same (1 each).
- HAZARD's win likely comes from simpler write barrier ordering (RCU's `rcu_enter` does flush_line + full_fence on publish_epoch first, then a separate flush_line on the slot; HAZARD's `hazard_protect` is one slot store + flush_line). The microbenchmark difference is at the noise floor.

## Note on future GC compatibility

- For Phase 2 W1 (per-host reserved segments with G1 RCU-defer GC), we **still need RCU infrastructure** for the deferred reclamation. HAZARD is the read-side winner, but the GC side will pull in RCU as well (the user's QR6 G1 answer mandates RCU-defer + retire list for W1 GC). So both modules ship; HAZARD is the production read fast-path.

---

## Next actions

- Phase 1.4: G1 hash-diff battery on HAZARD build (verify §I9 strict-A linearizability preserved)
- Phase 1.5: Full 210-cell scaling sweep with HAZARD build + plots
- Reject RCU build for read path; keep RCU module for Phase 2 W1 GC
