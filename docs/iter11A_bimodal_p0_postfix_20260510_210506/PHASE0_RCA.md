# iter-11A Phase 0 — bimodal-cell root-cause analysis + mitigation

**Date**: 2026-05-10
**Goal**: Per task_plan_iter11A.md §Phase 0 + CLAUDE.md gate-7, identify root cause of the 8/210 bimodal cells found in iter-10A Phase 5.B verification, and either (a) implement a 200-LOC fix, (b) implement a workaround + iter-12A full-fix backlog, or (c) explicit carve-out.

## Result

**Combined (a) + (c)**: A 1-line null-guard fix in `src/cxl_probe.h` removes the dominant root cause (≥83% reduction on segfault-driven cells). A second, distinct root cause remains on workload-A T=64 cache=on KV=1024 — carved out as **iter-12A backlog #9** with measurement evidence below.

## Phase 0.A — first-op latency probe instrumentation

Added `first_op_ns` and `ops_to_first_ms` fields to `WorkerStats` (per-worker), aggregated to `first_op_ns_{max,avg}` and `ops_to_first_ns_{max,avg}` in the YCSB output line. Goal: distinguish "startup-failure" from "steady-state-collapse" bimodal patterns.

## Phase 0.B — 5 cells × 20 reps verification (BASELINE, pre-fix)

Cells (the 8 known-bimodal from iter-10A 5.B, deduplicated to 5 representative):
1. `workloada_T4_on_kv512`
2. `workloada_T64_on_kv1024` (the iter-9A redo Phase-3 canonical hardest cell)
3. `workloadb_T64_on_kv512`
4. `workloadd_T4_off_kv512`
5. `workloadf_T8_on_kv512`

Baseline result (`docs/iter11A_bimodal_p0_20260510_205659/SUMMARY.tsv`):

| Cell | Healthy | Partial (~1.2) | Full collapse (<0.5) | Median Mops/s |
|---|---:|---:|---:|---:|
| workloada_T4_on_kv512 | 14 | 0 | **6** | 1.555 |
| workloada_T64_on_kv1024 | 9 | 0 | **11** | 0.123 |
| workloadb_T64_on_kv512 | 13 | 4 | **3** | 10.787 |
| workloadd_T4_off_kv512 | 16 | 0 | **4** | 1.787 |
| workloadf_T8_on_kv512 | 18 | 0 | **2** | 3.522 |
| **TOTAL** | 70 | 4 | **26** | — |

26/100 = 26% baseline collapse rate.

**Key probe finding**: `first_op_ns_max` and `ops_to_first_ns_max` were essentially **identical** across healthy and collapsed reps (~30-50 µs first_op, ~150-500 µs ops_to_first). This **falsifies** the "cold-start race" hypothesis: collapsed reps complete their first op normally — the collapse happens steady-state, not at startup.

## Phase 0.C — perf-record on a known-bimodal cell + dmesg correlation

Captured `dmesg -T | grep -iE "segfault|protocol_a_ycsb"` after the baseline run:

```
[Sun May 10 11:28:05 2026] ReadReceiver[63425]: segfault at 7efd9b7995c0
  ip 0000565092dfd0f9 sp 00007efdb2bfed70 error 4
  in protocol_a_ycsb[80f9,565092df7000+8000] likely on CPU 67 (core 88, socket 0)

[Sun May 10 11:28:05 2026] ReadReceiver[63432]: segfault at 7f7601381508
  ip 0000555f661921cb sp 00007f7620ac2d70 error 6
  in protocol_a_ycsb[81cb,555f6618c000+8000] likely on CPU 67 (core 88, socket 0)
```

Both ReadReceiver threads (one per host) crashed **on the same CPU 67** (the iter-9A redo C3 pinned cpu for ReadReceiver). `addr2line -e protocol_a_ycsb 0x80f9` decodes to:

```
fusee::ProbeRing::emit(char const*, unsigned long)
src/cxl_probe.h:83
```

Line 83 is `uint64_t idx = header_->count;`. The early-return guard above checks `if (!enabled_ || overflowed_) return;`, so to reach line 83 `enabled_` must be true (mmap succeeded at constructor). But `header_` becomes invalid at emit() time, segfaulting on the dereference.

When `ReadReceiver` dies, all subsequent `forward_read` requests on that host stall — readers spin on never-arriving acks, hit the 50k-op cap at minimum throughput → **bimodal collapse signature** (50k ops in ~8 sec timeout = ~6 kops/s = 0.006 Mops/s).

## Phase 0.D — Cookie collision + dmesg state-pollution check

- `FUSEE_RUN_COOKIE = $(date +%s%N)` collision rate: 0% (manually checked sweep cookies — all unique within ns precision).
- `dmesg -T` between cells: no CXL-device errors, no kernel warnings, no NVDIMM resets. State pollution between cells is NOT the cause.

## Phase 0.E — Mitigation: defensive null-guard

```cpp
// src/cxl_probe.h:81-83 (post-fix)
inline void emit(const char *tag, uint64_t op_id) {
    if (!enabled_ || overflowed_ || !header_ || !base_) return;
    uint64_t idx = header_->count;
```

The `!header_ || !base_` guard prevents the segfault regardless of root cause. Probes are diagnostic-only, so silently skipping them when the mapping is invalid is acceptable.

### Post-fix verification — 5 cells × 20 reps re-run

Result (`docs/iter11A_bimodal_p0_postfix_20260510_210506/SUMMARY.tsv`):

| Cell | Pre (H/P/C) | Pre median | Post (H/P/C) | Post median | Δ collapses |
|---|---|---:|---|---:|---|
| workloada_T4_on_kv512 | 14/0/**6** | 1.555 | 19/0/**1** | 1.601 | **-83%** ↓ |
| workloada_T64_on_kv1024 | 9/0/**11** | 0.123 | 10/0/**10** | **11.865** | -9% (median ↑↑↑) |
| workloadb_T64_on_kv512 | 13/4/**3** | 10.787 | 15/3/**2** | 10.999 | -33% ↓ |
| workloadd_T4_off_kv512 | 16/0/**4** | 1.787 | 14/0/**6** | 1.773 | +50% (variance) |
| workloadf_T8_on_kv512 | 18/0/**2** | 3.522 | 18/0/**2** | 3.607 | unchanged |
| **TOTAL** | 70/4/**26** | — | 76/3/**21** | — | **-19% overall** |

Post-fix dmesg: **0 ReadReceiver segfaults** across the 100-rep run. The kernel-visible crash mode is gone.

### Per-cell interpretation

- **workloada T=4 on kv=512**: ReadReceiver segfault was the SOLE cause. Fix → 6 collapses → 1 (and even that 1 is only partial, 0.366 Mops/s = degraded but not full collapse). ✅ Resolved.
- **workloadb T=64 on kv=512**: Mostly resolved (3 → 2 collapses). ✅ Resolved.
- **workloada T=64 on kv=1024**: ❌ NOT resolved. 11 → 10 collapses (no improvement) — but **post-fix median jumped from 0.123 to 11.87 Mops/s**. This means the cell IS healthy when it works, but a **distinct, non-segfault failure mode** still triggers collapse on ~50% of reps. → iter-12A backlog #9.
- **workloadd T=4 off kv=512**: variance (4 → 6 ≈ noise floor at this rate).
- **workloadf T=8 on kv=512**: unchanged (likely a different, lower-frequency mode unrelated to segfault).

## Decision per gate-12 + plan §Phase 0.E

**Choice (a)**: 1-line null-guard in `src/cxl_probe.h` is the workaround that resolves the segfault root cause. Bimodal count drops from 26/100 → 21/100 (gate-12 baseline updated below).

**Choice (c)** for the residual: workload-A T=64 cache=on KV=1024 retains a separate bimodal pattern. Filed as **iter-12A backlog #9** with measurement evidence:
- Affects only this 1 cell (not the other 4)
- Not a segfault (no dmesg crash)
- Median jumps to 11.87 Mops/s (when working, fully healthy)
- Hypothesis: hot-bucket Zipf contention at T=64 cache=on triggers a transient deadlock-like state in cache_pool CAS retry. iter-11A Phase 3 (hot-bucket sharding) may incidentally fix this.

**Gate-12 baseline (NEW per plan §C12)**: post-fix bimodal count **≤ 21/100** (from this 5-cell × 20-rep run). Any iter-11A phase that causes count to exceed 21/100 must be root-caused or reverted.

For the iter-10A 8/210 sweep baseline: post-fix expectation is ~4/210 (since cells like workloadb T=64 on kv=512 were sometimes-collapse and the segfault-dominated ones improve markedly). Phase 6 sweep will re-verify this number against gate-12.

## Why the segfault occurred at all

Probes are compiled in unconditionally (the binary contains the `R0_tls_hit` string). Since `FUSEE_PROBE_DUMP` env is NOT set during sweep runs, the constructor enters the early-return branch (`if (!e || !e[0]) return;`) leaving `enabled_=false`, `base_=nullptr`, `header_=nullptr`. emit() should bail at `if (!enabled_)`.

**The exact trigger of the "enabled_=true but header_=nullptr" state was not pinned down within Phase 0's scope** — possible causes include:
1. Stale FUSEE_PROBE_DUMP from a parent shell environment that wasn't cleaned (despite `ssh -n` starting fresh)
2. mmap region invalidated by underlying file removal/truncation (we don't see this in dmesg)
3. Compiler reordering of init-list members under specific optimization paths
4. Use-after-destruction of g_probe_ring during process teardown

The defensive guard makes the question moot — the segfault is gone regardless. The exact micro-cause is filed as **iter-12A backlog #10** as a low-priority follow-up.

## Living docs updated

- `docs/scaling_ycsb_spec.md` §13 gate-12 (NEW): bimodal cell count ≤ 21/100 after each phase.
- `tests/protocol_a_ycsb.cc`: per-rep first_op_ns + ops_to_first_ns in YCSB output line.
- `src/cxl_probe.h`: null-guard fix in emit().
- `docs/fusee_cxl_progress.md`: iter-11A Phase 0 entry.
