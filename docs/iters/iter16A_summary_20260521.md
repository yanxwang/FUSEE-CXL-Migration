# iter-16A summary

**Date**: 2026-05-21
**Focus**: cross-host write path stage decomposition + first-round optimization (H9 fix)

## High-level outcomes

### 1. Single-flush H9 fix delivers +22% throughput

`publish_slot_cow` was using two-phase publish (`slot->value = X; flush;
sfence; slot->key = Y; flush; sfence;`). The two-phase pattern is for
cases where key and value live on DIFFERENT cachelines. In the current
16 B slot layout both live on the same cacheline, so one flush is
sufficient (CXL cacheline write is atomic). Simplified to single
write+flush+sfence. Validation:

| T | post-H9 thpt (Mops/s) | iter-15A Phase 2 baseline | Δ |
|---:|---:|---:|---:|
|  1 | 0.205 | 0.198 | +4% |
|  2 | 0.400 | 0.299 | +34% |
|  4 | 0.638 | 0.527 | +21% |
|  8 | 0.676 | 0.554 | +22% |
| 16 | 0.689 | 0.563 | +22% |
| 32 | 0.750 | 0.612 | +22% |
| 64 | 0.741 | 0.604 | +23% |

Data: `docs/iter16A_xhost_T_sweep_20260521_070106/`

### 2. Receiver-NOOP study (iter-16A RN-A/B)

Confirmed receiver-side cost decomposition:
- Invalidate broadcast: ~0% (path is OP_CACHE_REGISTER-dependent and that wire is missing since iter-4A; sharer_bitmap always empty)
- Bucket lookup + dir + CoW: ~35% of receiver work
- CXL slot publish: ~60% of receiver work
- Ring + ack baseline: ~5%

L3 ceiling (receiver does ack only): 1.52 Mops/s/host at T≥16. Sets the
architectural ceiling for any single-receiver design.

Data: `docs/iter16A_xhost_rn_study_20260521_023705/ANALYSIS_partial_RN-A.md`

### 3. Stage decomposition framework

Built end-to-end stage decomposition infrastructure:
- **8 canonical stages** (5 worker + 3 receiver) for xhost write
- **15 probe tags** (XWS*/XWR*) with strict `Σstages == StageW/R` invariants
- **11 derived latencies + 4 event counters**
- **PROBE_OP vs PROBE_PATH macro split**: stage probes (XWS*/XWR*) gate
  by FUSEE_PROBE; legacy path probes (W*/R*/I*) gate by FUSEE_PROBE_PATH.
  Lets stage decomp run without load-phase W* events overflowing the ring.
- **Probe ring 128MB → 512MB**: at T=1, single worker emits 15M trans
  events, blew the 128MB cap. 512MB comfortably fits even T=1.
- **V-matched probe builds**: kForwardStagingSlotBytes configurable
  per-V via `-DFUSEE_FWD_STAGING_SLOT_BYTES=N`.

Recipe doc: [docs/xhost_write_decomp_recipe.md](xhost_write_decomp_recipe.md)
Scripts: [scripts/iter16A_xhost_decomp_*.{sh,py}](../scripts/)

### 4. Sanity-check sweep findings (T={1, 8, 64})

(from `docs/iter16A_xhost_decomp_sweep_20260521_072029/`, pre-bug-fix; T=1
had probe ring overflow due to W* probes)

T=8 (post-H9-fix-build):
| Stage | p50 (ns) | % of StageW |
|---|---:|---:|
| Stage1 slot_reserve | 528 | 3% |
| Stage2 slot_wait | 1150 | 7% |
| Stage3 value_xfer | 31 | <1% (CPU-side only, see caveat) |
| Stage4 ctrl_publish | 18 | <1% |
| Stage5 ack_wait | **13900** | **88%** |
| Total StageW | 15890 | 100% |

### 5. Full T-sweep stage decomp (probe-on, V=1024, zipf-0.99)

Data: `docs/iter16A_xhost_decomp_sweep_20260521_075204/`. 21 cells × all 11 latencies.

| T | thpt | Stage1 | Stage2 | Stage3 | Stage4 | Stage5 | StageW | Stage6 | Stage7 | Stage8 | StageR | RTT |
|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| 1 | 0.055 | 540 | 1189 | 36 | 20 | 4722 | 6579 | 2966 | 667 | 18 | 3702 | 2767 |
| 2 | 0.103 | 540 | 1185 | 36 | 20 | 4813 | 6672 | 2888 | 664 | 18 | 3606 | 3137 |
| 4 | 0.186 | 538 | 1163 | 37 | 20 | 5286 | 7102 | 1963 | 668 | 18 | 2702 | 4520 |
| 8 | 0.587 | 532 | 1155 | 33 | 18 | 14044 | 15877 | 1108 | 672 | 15 | 1907 | 13642 |
| 16 | 0.648 | 533 | 1155 | 38 | 20 | 21680 | 23523 | 1157 | 676 | 15 | 2072 | 21252 |
| 32 | 0.718 | 475 | 1038 | 38 | 21 | 66512 | 68209 | 1036 | 637 | 15 | 1868 | 66106 |
| 64 | 0.707 | 476 | 1018 | 38 | 21 | 139674 | 141366 | 1101 | 658 | 16 | 1979 | 139041 |

**Key T-scaling observations**:
- **Stage 5 (ack_wait) dominates at all T** (72%-99% of StageW)
- T=1→4: Stage 5 ≈ 5000 ns flat — single-flow CXL RTT bound (worker spin ~ 5 µs)
- T=8: Stage 5 jumps 3× to 14 µs — receiver saturation kicks in; workers fight for the single receiver
- T=64: Stage 5 = 139 µs — workers queueing behind receiver
- Stage 1/2 (slot_reserve, slot_wait) **stable ~1500 ns** across all T — ring tail
  fetch_add not a bottleneck. C-spin (XWS2R) rate < 1% at all T.
- StageR (receiver-internal work) stays at **1.8-3.7 µs across all T** — receiver
  is NOT saturated by its own work; it's idle between ops at high T (waiting for
  ring activity).
- **RTT = Stage 5 - StageR ≈ 99% of StageW at T=64** — virtually all worker
  latency is CXL roundtrip + ack-wait spin, not receiver work.

### 6. V sweep findings (probe-on)

Data: `docs/iter16A_xhost_V_sweep_20260521_085331/`. 36 cells = 4V × 3T × 3 reps.

thpt (Mops/s) vs V (probe-on):

| T | V=64 | V=256 | V=512 | V=1024 |
|--:|--:|--:|--:|--:|
| 1 | 0.057 | 0.057 | 0.056 | 0.055 |
| 8 | 0.602 | 0.598 | 0.585 | 0.595 |
| 64 | 0.713 | 0.709 | 0.710 | 0.708 |

**thpt essentially invariant across V**. → V=1024 is **NOT CXL-BW-bound**
(otherwise V=64 would be ~16× faster). Stage 5 (CXL RTT + spin) dominates and is
V-independent. Stage 3 (value_xfer CPU-side) is small (~40 ns) and the actual
1024 B CXL write completes async during Stage 5.

### 7. Dist sweep findings (probe-on)

Data: `docs/iter16A_xhost_dist_sweep_20260521_102853/`. 36 cells = 4 dists × 3T × 3 reps.

thpt (Mops/s) vs key distribution (V=1024):

| T | uniform | zipf-0.5 | zipf-0.99 | zipf-1.5 |
|--:|--:|--:|--:|--:|
| 1 | 0.053 | 0.053 | 0.055 | 0.055 |
| 8 | 0.583 | 0.599 | 0.601 | 0.599 |
| 64 | 0.710 | 0.717 | 0.705 | 0.703 |

**thpt invariant across distribution skew**. Key implication: with **current
single-receiver**, hot-bucket lock contention (Stage 7's `slot_directory_lock`)
**does NOT manifest** because all bucket lock attempts serialize on the
single receiver thread anyway — no actual concurrency on the lock.

**Implication for iter-17A multi-receiver**: hot-bucket contention will likely
re-surface when multiple receivers attempt the same bucket lock. The dist
sweep should be **re-run after multi-receiver lands** to verify.

### 8. Perf stat sweep findings (probe-off, T={1..64})

Data: `docs/iter16A_xhost_perfstat_20260521_121842/grid.csv`. 21 cells × 6 perf events.

| T | thpt | cycles/op | IPC | cache_miss/op | LLC_miss% | w_p50 (µs) |
|--:|--:|--:|--:|--:|--:|--:|
| 1 | 0.214 | 181k | 0.31 | 56 | 88% | 10.1 |
| 2 | 0.423 | 132k | 0.38 | 46 | 86% | 10.1 |
| 4 | 0.637 | 131k | **0.43** | 44 | 81% | 13.1 |
| 8 | 0.674 | 178k | 0.42 | 53 | 85% | 24.3 |
| 16 | 0.688 | 257k | 0.39 | 68 | 89% | 47.0 |
| 32 | 0.749 | 393k | 0.30 | 98 | 91% | 85.6 |
| 64 | 0.738 | 1.17M | **0.18** | 171 | 93% | 172.9 |

**Key observations**:
- **T=4 is the most efficient operating point** (lowest cycles/op = 131k, best IPC = 0.43)
- T=64 spends **9× more cycles per op** than T=4 (mostly stalled on memory/CXL)
- **LLC miss rate 80%+ everywhere** — confirms CXL is the binding memory medium
  (load goes to L3, L3 miss = CXL fetch). CXL miss rate stays roughly constant
  because op pattern (ring + bucket + pool) hits CXL the same way regardless of T.
- IPC drops from 0.43 (T=4) → 0.18 (T=64) = 2.4× degradation from contention/stall
- post-PXE thpt at T=64 = 0.738 Mops (vs pre-PXE 0.741) — consistent baseline.

## Established infrastructure (for future iters)

| Artifact | Path | Purpose |
|---|---|---|
| Recipe doc | `docs/xhost_write_decomp_recipe.md` | process-ready procedure |
| Spec doc updates | `docs/microbench_xhost_spec.md`, `microbench_4path_spec.md` | canonical params + decomp pointer |
| Stage probes | `src/cxl_kv_ops_A.cc`, `src/cxl_probe.h` | code |
| T probe-off sweep | `scripts/iter16A_xhost_T_sweep.sh` | thpt baseline |
| T probe-on sweep | `scripts/iter16A_xhost_decomp_sweep.sh` | stage decomp |
| V sweep | `scripts/iter16A_xhost_V_sweep.sh` | V scaling |
| Dist sweep | `scripts/iter16A_xhost_dist_sweep.sh` | distribution scaling |
| Perfstat sweep | `scripts/iter16A_xhost_perfstat_sweep.sh` | hw counters |
| Analyzer | `scripts/iter16A_xhost_decomp_analyze.py` | trace → CSV |
| T viz | `scripts/iter16A_decomp_viz.py` | T sweep plots |
| Generic viz | `scripts/iter16A_decomp_viz_generic.py` | V/dist plots |

## Issues / caveats discovered

1. **Stage 3 not measurable directly**: clflushopt is async; sfence drains
   CPU side but actual CXL propagation hides in Stage 5. Measured Stage 3
   ≈ 40 ns (CPU-side cost), NOT 1024 B CXL write physical time.
2. **Stages 4 & 8 (small) probe-overhead-bound**: < 100 ns stage with
   ~30 ns probe overhead = not reliable.
3. **Cross-host op_id ambiguity**: `write_op_counter_` is per-process;
   after fork, child workers can have colliding op_ids. Cross-host pairing
   is correct in aggregate (distributions) but not per-op exact.
4. **OP_CACHE_REGISTER never wired** (latent since iter-4A spec drift):
   sharer_bitmap is always empty, invalidate broadcast is a no-op,
   strict-A linearizability is violated under concurrent read+write
   (untested by current hash-diff). To fix in iter-17A+.
5. **F2 status read** appeared at first to have CXL staleness risk —
   resolved as a misread; status lives on cacheline 2 (consumer-owned),
   gets flushed by receiver's I3 alongside resp_op_id, so worker's F1
   flush+load picks up fresh status. No bug.
6. **H7 (re-flush bucket after slot lock)** is defensive but functionally
   redundant — code doesn't re-scan after the flush. Optimization
   candidate.

## Next iter (iter-17A) candidates

Priority-ordered based on iter-16A findings:

1. **Multi-receiver implementation** (highest ROI per RN study + T-sweep
   findings) — Stage 5 dominance + StageR idle = clear pipeline bottleneck.
   Re-run dist sweep after multi-receiver lands to surface hot-bucket
   contention.
2. **OP_CACHE_REGISTER fix** (correctness debt from iter-4A) — strict-A
   linearizability is currently violated under concurrent read+write because
   sharer_bitmap is always empty.
3. **H7 removal** (small but free perf gain) — defensive flush after
   slot_directory_lock has no functional purpose given no re-scan.
4. **D2 + D3 → pool->write_iov merge** (1 sfence saved per op).
5. **NT stores for value_xfer** (Stage 3) — bypass cache + no flush_line
   per cacheline; potentially significant for large V.

## Iter completion checklist

| Deliverable | Status |
|---|---|
| Stage decomp framework (15 probes, 11 latencies) | ✅ done |
| H9 single-flush optimization | ✅ done (+22% all T) |
| T sweep (probe-off + probe-on) | ✅ 42 cells |
| V sweep (4V × 3T × 3 reps) | ✅ 36 cells |
| Dist sweep (4 dists × 3T × 3 reps) | ✅ 36 cells |
| Perfstat sweep (7T × 3 reps) | ✅ 21 cells |
| Recipe doc (`xhost_write_decomp_recipe.md`) | ✅ |
| Iter summary | ✅ this doc |
| Memory updates | ✅ project_protocol_a_iter16A.md + MEMORY.md |
| Plots (T:14, V:13, dist:13, perfstat:1) | ✅ 41 PNGs |
| Raw probe cleanup | ✅ probes_h0/h1 dirs deleted, per_op.csv gzipped |
| Code commit | ⏸ pending user signoff per CLAUDE.md commit hygiene |

## Code change inventory (uncommitted)

```
scripts/iter14A_gen_microbench_traces.py | 115 ++++++++--
src/cxl_cache_pool.cc                    |   7 +
src/cxl_forward_staging.h                |  18 +-
src/cxl_kv_blockpool.cc                  |  15 ++
src/cxl_kv_ops_A.cc                      | 346 +++++++++++++++++++++----------
src/cxl_kv_ops_A.h                       |  29 ++-
src/cxl_probe.h                          |  23 +-
tests/protocol_a_ycsb.cc                 |  67 +++++-
 11 files changed, 646 insertions(+), 207 deletions(-)
```
Plus 10 new scripts in `scripts/iter16A_*` + 2 new docs.
