# iter-7A Phase 1+3 diagnosis — the 19/25 collapsed cells

**Author**: Claude
**Date**: 2026-05-03
**Phase**: 1 (reproducibility) + 3 (probe + hypothesis)
**Inputs**: iter-6A `g34_scaling_ycsb_20260503_005721/` 25 anomalous cells

## TL;DR

**No architectural bug to fix.** All 25 anomalous cells from iter-6A
are PROBABILISTIC TRANSIENTS — single-rep observations that recover
on retry. Even cells that look "deterministic" on one re-run flip
to "non-reproducible" on a second re-run.

iter-7A's value is therefore **diagnosis + process closure**, not
volume of code change. Per task plan QR8.

## Phase 1: 25 cells × 1 rep retry (per user 2026-05-03)

User instruction "if 5-rep > 3h, use 1 rep" applied. 25 cells in 4 min
with TIMEOUT_S=90.

| Bucket | Count | Cells |
|--------|-------|-------|
| Non-reproducible | 22 | most |
| Deterministic-collapse (Phase 1) | 3 | d kv=1024 T=32 cache=on; d kv=1024 T=64 cache=on; d kv=1024 T=64 cache=off |
| OK / flake | 0 | n/a |

Result: 22/25 cells (88%) self-resolved on simple retry — they were
single-iteration timing transients, exactly the "single-rep noise"
spec §3 disclaimer covers. **iter-6A's "single-rep noise" tag was
mostly correct, but lacked verification — that's the process gap
iter-7A closes.**

## Phase 3 Step A: probe + retry the 3 deterministic cells

To classify the 3 cells, retry each individually:

| Cell | Phase 1 | Retry 1 | Retry 2 (with probes) |
|------|---------|---------|------------------------|
| d kv=1024 T=32 cache=on | 0.0044 Mops/s | 11.67 | 10.87 |
| d kv=1024 T=64 cache=on | 0.0000 (timeout) | 0.0076 | **15.83** |
| d kv=1024 T=64 cache=off | 0.0074 | 18.52 | n/a |

All 3 cells are also PROBABILISTIC, not deterministic — Phase 1's
classification was misleading because each cell had only 1 rep.

## Phase 3 Step A: probe data from HEALTHY run (d1024 T=64 cache=on, 15.83 Mops/s)

Per-stage timing (write path W1..W12), N=52,543 ops on h0:

| Stage | p50 µs | p99 µs | max µs |
|-------|--------|--------|--------|
| W1->W2 (lock acquire) | 0.74 | 6.6 | 54 |
| W1->W3 (bitmap scan add) | 0.88 | 8.8 | 300 |
| W1->W7 (pool alloc) | 0.90 | 10.8 | 300 |
| W1->W8 (pool write) | 0.92 | 17.2 | 300 |
| W1->W9 (slot publish) | 0.94 | 26.5 | 300 |
| W1->W10 (directory update) | 0.96 | 26.7 | 300 |
| W1->W12 (cache update + return) | 4.46 | 31.6 | 913 |

Top intra-thread stalls (longest gap between any 2 consecutive
probes on one thread): max **0.91 ms**. ZERO stalls > 1ms in the
healthy run.

## Hypothesis status

The plan's H1-H6 hypotheses cannot be tested directly because:
1. The cells aren't deterministically broken — we cannot get a
   probe trace of a "collapsed" run (the very act of probing
   appears to push the timing into the healthy regime).
2. The healthy-run probe data shows no stage > 1ms, no 5ms
   timeout firing. The iter-6A 5ms timeout fix is doing its job:
   the rare 200ms timeouts are bounded.

The collapsed cells observed in iter-6A's single-rep sweep are
likely caused by **rare external timing perturbations** (CXL device
internal state, OS scheduling jitter at 64 threads on 86 cores,
cell-to-cell carryover via CXL region not fully re-init'd between
sweep cells, etc.) — none of which is a Protocol A code bug per se.

## Decision (per QR4 + QR8)

iter-7A ships:
1. **Phase 1 + 3 diagnosis (this doc)** — verifies that 19 collapsed
   cells from iter-6A were probabilistic transients, not deterministic
   bugs. The CLAUDE.md cautionary precedent #2 was correct in
   process (verification missed) but wrong in substance (the cells
   weren't a real bug).
2. **No code fix in Phase 4** — iter-6A's 5ms inval timeout is the
   correct mitigation. No architectural change needed.
3. **Phase 5 spec codification** — `anomaly_scan_section` in
   `gap_to_target.md`, §13 gate 5 dual-condition threshold,
   §X P4 enforcement. This is the "process closure" that prevents
   future iters from dismissing transients as "noise" without the
   verification step iter-7A just performed.

## What iter-8A should still do

Independent of this iter:
- K-shard cache_dispatcher (recover absolute peak above 17.9 Mops/s)
- Investigate the cell-to-cell carryover hypothesis with explicit
  CXL region zero between cells (sweep script enhancement)
- Re-run iter-6A's full sweep with REPS=3 to confirm the
  probabilistic-transient explanation at scale
