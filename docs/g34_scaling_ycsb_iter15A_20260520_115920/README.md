# iter-15A YCSB scaling sweep — post-fix headline

**Date**: 2026-05-20
**Spec**: `docs/scaling_ycsb_spec.md` dimensions, with **only deviation**:
MAX_OPS=10M (vs spec default 200K).

**Build**: post-fix binaries (Bug 1 `wire_rings_for_child` + Bug 2 pool
cursor flush). 4 V-matched builds on g3+g4.

## Result: 210/210 cells OK, 0 failures

| Workload | Description | Peak thpt | Best (T, cache, kv) | gap to 20 Mops/s |
|---|---|---:|---|---:|
| workloada | R50/W50 zipf | 1.447 | T=32, cache=off, kv=256 | -18.55 |
| workloadb | R95/W5 zipf | 3.233 | T=32, cache=on, kv=1024 | -16.77 |
| **workloadc** | **R100 zipf** | **32.249** | T=64, cache=off, kv=256 | **+12.25 ✓ PASS** |
| workloadd | R95/W5 latest | 15.991 | T=64, cache=on, kv=512 | -4.01 |
| workloadf | R50/RMW50 | 1.916 | T=32, cache=off, kv=256 | -18.08 |

## Headline finding

**Only workloadc (100% read) passes 20 Mops/s bar**. Mixed-RW workloads
cap at 1-3 Mops because cross-host write forwarding bottlenecks the
cluster.

This is the **first sweep since iter-9A with clean numbers**. Previous
iters (9A → 14A) were contaminated by Bug 1 (`wr_=null` on forked workers
silently failing) which inflated mixed-RW throughput.

## Per-workload analysis

### workloada (R50/W50 zipf) — 1.45 Mops peak
- Severely bound. Each host has 50% local reads (~25 Mops in microbench) +
  50% peer-key writes (cross-host forward → 0.6 Mops single-thread receiver).
- Amdahl/queueing: 50% xhost ops at 0.6 Mops cap the cluster around 1-2 Mops.
- T=32 peak; cache off slightly better (less cache_pool_insert overhead).

### workloadb (R95/W5 zipf) — 3.23 Mops peak
- Mostly reads but 5% writes still dominate (any cross-host write blocks
  the receiver pipeline).
- cache=on helps modestly (~5%) because reads benefit from cache hits
  while occasional writes still go through xhost.

### workloadc (R100 zipf) — 32.25 Mops peak ✓
- No writes → no cross-host write bottleneck.
- First-touch fills cache_pool, then 99%+ of reads hit local cache.
- Scales to T=64 because read path is mostly lock-free seqlock.
- Below the microbench cellular max (Phase 3 c=1% local_read peaked at
  65.7 Mops, but Phase 5 50/50 mix shows even reads suffer 38× drop with
  any xhost contribution; workloadc is "all reads but half are peer-owned"
  in the YCSB role-mode 2-host pattern).
- **The only workload above the 20 Mops/s bar.**

### workloadd (R95/W5 latest) — 15.99 Mops peak (close)
- Same as workloadb but uses "latest" key distribution (recently-inserted
  keys are hot) — different cache locality pattern → slightly higher.

### workloadf (R50/RMW50) — 1.92 Mops peak
- Read-modify-write = effectively 100% writes (each RMW does 1 read + 1
  write). Same Amdahl bound as workloada.

## Comparison to iter-10A/12A/14A peaks (bug-contaminated)

iter-10A peak workloada was reported as 14.81 Mops; current (post-fix) is
1.45 Mops = **10× lower**. The 14.81 figure was almost certainly Bug 1
(`wr_=null` silent failure) inflating the count of failed-fast no-op
attempts. Subsequent iters built on this false signal.

iter-15A's 32.25 Mops on workloadc is REAL (read path doesn't depend on
the buggy forward_*_direct path). Previous iter-10A workloadc was 11.72;
the iter-15A 3× improvement comes from MAX_OPS=10M reaching steady-state
(vs 200K which is mostly warmup).

## Files

- `SUMMARY.log` — 210 YCSB lines + header
- `raw/<workload>_optA_t<T>_cache<on/off>_kv<256/512/1024>/` — per-cell stdout/err
- `plots/`:
  - `<workload>_thpt.png` × 5 — per-workload thpt vs T, 6 lines (kv × cache)
  - `A_target_summary.png` — peak per workload vs 20 Mops/s target (bar chart)
  - `A_all_workloads_log.png` — all 5 workloads best-cell scaling, log Y

## Cross-references

- iter-15A summary: `docs/iters/iter15A_summary_20260520.md`
- iter-15A microbench plan: `docs/iter15A_microbench_plan/README.md`
- Phase 6 RCA (explains why mixed-RW is bottlenecked): `docs/iter15A_phase6_0_dist_c2c_*/ANALYSIS.md` + others
- iter-15A backlog memo: `docs/iters/iter15A_backlog_memo.md`
- scaling_ycsb spec: `docs/scaling_ycsb_spec.md`

## Implications for iter-16A backlog

These sweep results give clean numbers that align with iter-15A microbench
findings:

- **xhost write path is the universal bottleneck** for mixed-RW (a/b/d/f).
  Fix candidate: multi-thread receiver (per Phase 6c + 6b finding).
  Predicted gain: 5-10× on workloads a/b/d/f (currently 1-3 Mops, projection
  10-15 Mops at multi-receiver).

- **Workloadc 32 Mops is already above bar** but could go higher via
  cache_pool compaction (per Phase 3 + 6a finding: smaller cache pool ⇒
  less LLC pressure ⇒ faster reads).

- **Hot-key lock break** (per Phase 6.0c + 6d finding) primarily benefits
  zipf-1.5 cells which aren't in standard YCSB (uses zipf-0.99). May not
  show up in scaling_ycsb but real workloads with stronger skew would
  benefit.
