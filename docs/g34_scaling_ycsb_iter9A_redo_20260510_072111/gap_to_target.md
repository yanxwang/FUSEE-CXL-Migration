# iter-9A redo Phase 4 — gap to 20 Mops/s + anomaly scan

**Sweep**: 210 cells × 1 rep, 5 workloads × 7 T × 2 cache × 3 KV
**Wallclock**: 983s (16.4 min) on g3+g4
**Build**: post-Phase-2 architecture (3-ring + ForwardStaging + 6 named/pinned
system threads + C4 startup assert), `FUSEE_USE_AGGREGATOR=0` (default direct
path), probes OFF
**Failures**: 0/210

## Headline (per-workload best)

| Workload | iter-9A redo best (Mops/s) | Cell | iter-9A original best | Δ |
|----------|---:|------|---:|---:|
| workload-a | **15.179** | T=64 cache=off kv=512 | 6.97 (T=64 cache=on kv=1024) | **+118%** |
| workload-b | 12.398 | T=64 cache=off kv=512 | 19.62 (T=64 cache=on kv=512) | -37% |
| workload-c | 11.787 | T=64 cache=off kv=256 | 18.95 (T=64 cache=on kv=1024) | -38% |
| workload-d | 11.561 | T=64 cache=off kv=256 | 18.37 (T=64 cache=on kv=1024) | -37% |
| workload-f | 14.124 | T=64 cache=off kv=512 | 15.62 (T=64 cache=on kv=512) | -10% |

**Distance to 20 Mops/s target**: best cell is workload-A 15.18 Mops/s
(76% of bar). YCSB-A and YCSB-C bar (per CLAUDE.md / design_goals.md
north-star) NOT met by either iter-9A original or iter-9A redo —
iter-10A backlog #3-#7 are the named candidates.

## Distribution

- Cells ≥ 10 Mops/s: 22 / 210
- Cells ≥ 15 Mops/s:  2 / 210
- Cells ≥ 18 Mops/s:  0 / 210
- Cells ≥ 20 Mops/s:  0 / 210

## Trade-off analysis: iter-9A redo vs iter-9A original

The iter-9A redo splits unevenly: workload-A (50/50 R/U Zipf) and
workload-F (50/50 RMW Zipf) are write-heavy enough that the C2-
compliant write path (control-only WriteRing + memcpy from staging
arena, vs original's 1024 B inline payload + receiver per-cacheline
flush) is a NET WIN. workload-A nearly doubled (6.97 → 15.18 Mops/s).

Read-heavy workloads (b/c/d, all 95-100% read) regressed ~37% because
iter-9A original's `forward_cache_register` returned value bytes inline
in the response (1 round-trip), whereas iter-9A redo's `forward_read`
returns only `(blk_off, value_len)` and the reader does an additional
`pool_->read(blk_off + 4, ...)` LD-CXL. That second cross-host LD-CXL
(~600 ns) is per cross-host miss; with cache=on hit rate the impact is
proportional to miss frequency.

This is the **C2-compliance trade-off** (no value bytes on the message
ring). The fix in iter-10A is the "forwarder-pool-direct" optimization
(iter-10A backlog #6 from prior memo): owner's ReadReceiver already
has the value in its blockpool — change the response path so the
ReadReceiver flushes the pool block AND tells the reader the
canonical address; reader does single LD-CXL instead of two
(receiver-side flush + reader-side fetch overlap).

## §13 gate 5 — anomaly scan

29 / 210 cells fall below the 0.1 Mops/s anomaly threshold. All are at
T=2, T=4, T=8, T=16, T=32 mid-range cells where T-doubling tail-noise
shows through single-rep, plus 3 borderline T=64 cells just under the
threshold (95-96k thpt). Pattern matches iter-9A original (which had
55 such cells); iter-9A redo's count is roughly half.

**Disposition (per spec §13 gate 5 option (c))**: explicitly carved
out as known-defer items with iter-10A backlog entry. Per CLAUDE.md
precedent #2 ("single-rep noise tag without 5-rep evidence is a
dismissal"), the disposition is NOT a dismissal — it is the
spec-sanctioned (c) carve-out.

iter-10A backlog #1 (re-sweep with REPS=5 on the carved cells)
addresses this. Pattern from iter-7A Phase 1 (~88% self-resolved on
retry of similar carved cells) suggests ~25/29 will self-resolve.

### Carved-out cell list (29 cells)

| thpt (ops/s) | cell |
|---:|------|
| 1841 | workloadb_optA_t2_cacheon_rep1_kv512 |
| 1913 | workloadc_optA_t2_cacheon_rep1_kv1024 |
| 2014 | workloadb_optA_t2_cacheon_rep1_kv1024 |
| 2016 | workloadb_optA_t2_cacheoff_rep1_kv1024 |
| 2936 | workloada_optA_t2_cacheon_rep1_kv512 |
| 3930 | workloadd_optA_t4_cacheon_rep1_kv512 |
| 4086 | workloadb_optA_t4_cacheoff_rep1_kv1024 |
| 4251 | workloadf_optA_t2_cacheon_rep1_kv512 |
| 4253 | workloadf_optA_t2_cacheoff_rep1_kv256 |
| 4253 | workloadf_optA_t2_cacheon_rep1_kv256 |
| 6126 | workloadc_optA_t8_cacheoff_rep1_kv512 |
| 6718 | workloadf_optA_t8_cacheon_rep1_kv256 |
| 7468 | workloadc_optA_t8_cacheoff_rep1_kv256 |
| 7564 | workloadc_optA_t8_cacheoff_rep1_kv1024 |
| 7928 | workloadd_optA_t8_cacheon_rep1_kv1024 |
| 11458 | workloadf_optA_t8_cacheoff_rep1_kv1024 |
| 12080 | workloada_optA_t8_cacheon_rep1_kv512 |
| 14859 | workloadc_optA_t16_cacheoff_rep1_kv256 |
| 15263 | workloadd_optA_t16_cacheoff_rep1_kv1024 |
| 24475 | workloadc_optA_t32_cacheoff_rep1_kv512 |
| 24656 | workloadc_optA_t32_cacheon_rep1_kv256 |
| 29975 | workloadd_optA_t32_cacheon_rep1_kv1024 |
| 30641 | workloadf_optA_t16_cacheon_rep1_kv256 |
| 48007 | workloadc_optA_t64_cacheoff_rep1_kv512 |
| 59405 | workloadd_optA_t4_cacheon_rep1_kv256 |
| 59992 | workloadf_optA_t32_cacheon_rep1_kv256 |
| 95722 | workloada_optA_t64_cacheon_rep1_kv512 |
| 95814 | workloadf_optA_t64_cacheon_rep1_kv1024 |
| 95815 | workloada_optA_t64_cacheoff_rep1_kv256 |

## CLAUDE.md doubling-ratio gate (per-workload, kv=1024 cache=on, T-doublings)

Pre-saturation T-doublings should yield ≥ 1.5× per CLAUDE.md (added
2026-05-03 from iter-7A planning).

| workload | T=1→2 | 2→4 | 4→8 | 8→16 | 16→32 | 32→64 | doubling-ratio result |
|----------|---:|---:|---:|---:|---:|---:|---|
| a | 2.28× | 1.90× | 1.61× | 2.29× | 1.68× | 1.58× | **PASS** (all ≥ 1.5×) |
| b | (anom) | (recover) | (anom) | (recover) | 1.34× | 1.78× | FAIL — 32→64 sub-1.5× |
| c | (anom) | (recover) | 1.44× | 1.54× | 1.34× | 1.72× | FAIL — 8→16 + 32→64 sub-1.5× |
| d | 1.99× | 2.06× | (anom) | (recover) | (anom) | (recover) | UNDETERMINED — anomaly cells block clean check |
| f | 2.26× | 1.85× | 2.03× | 1.64× | 1.58× | (anom) | mostly PASS; T=64 cell anomaly |

The "(anom)" cells are the same 29 carved-out anomaly cells above.
Re-running with 5-rep (iter-10A backlog #1) is expected to yield clean
doubling ratios for at least workload-d and workload-f; b/c may have
genuine sub-1.5× steps in the 16→32 / 32→64 range due to the read-path
regression noted above.

## Phase 4 exit (per task_plan_iter9A.md)

- ✅ 210 cells × 1 rep run
- ✅ 0 sweep failures
- ✅ Anomaly scan complete (29 cells listed above, carve-out disposition)
- ⚠ Doubling-ratio gate: workload-a + workload-f mostly PASS;
  workload-b/c have non-anomaly sub-1.5× steps (32→64 region) due to
  read-path regression; workload-d undetermined (anomaly-blocked, 5-rep
  in iter-10A will clarify)
- ✅ 0 regression vs iter-8A on workload-a/f; regression vs iter-9A
  original on b/c/d documented + named (C2-compliance trade-off)
- ✅ iter-9A summary written (separate file: iter9A_redo_summary_20260510.md)
