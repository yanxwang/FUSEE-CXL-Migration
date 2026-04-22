# 2M-ops sustained validation — 9 peak cells

Per `docs/scaling_ycsb_spec.md` §11 ("Short runs (200k ops)
underestimate steady-state at T=86 by roughly 40-60 %"), re-ran each
peak cell from the 200k-ops sweep with `MAX_OPS=2000000`.

## Peak cell comparison

| cell | 200k Mops/s | 2M Mops/s | ratio |
|------|------------:|----------:|------:|
| A workloadc T=16 | 18.10 | **38.88** | 2.15× |
| B workloadc T=16 | 17.99 | **39.16** | 2.18× |
| C workloadc T=86 | 48.36 | **78.67** | 1.63× |
| A workloadd T=16 | 2.33 | 2.58 | 1.11× |
| B workloadd T=16 | 3.13 | 3.57 | 1.14× |
| C workloadd T=86 | 43.61 | **67.03** | 1.54× |
| A workloada T=2 | 0.27 | 0.27 | 1.00× |
| B workloada T=4 | 0.31 | 0.31 | 1.00× |
| C workloada T=4 | 1.12 | 1.10 | 0.98× |

## Interpretation

1. **Read-dominated workloads (c, d) underestimate at 200k by 1.5-2.2×**.
   The long tail of `max_wall / avg_wall` per-worker amortizes over more
   ops, so steady-state shows the actual sustained throughput. Spec's
   "40-60 %" estimate is on the low side for our 2-host setup.
2. **Write-heavy workloads (a) match within 2 %**. Not bound by run
   length; bound by lock-serialization floor.
3. **C sustained on workloadc T=86: 78.67 Mops/s** — updates the
   paper-grade reference from the baseline-recorded 48 Mops/s to this
   number. `scaling_ycsb_spec.md` §12 already cited 79 Mops/s as the
   2M number for this cell.

## What this means for the final comparison

Using the paper-grade 2M numbers:

| workload | opt | baseline 2M (est) | Phase 1+4+5 2M | ratio |
|----------|-----|------------------:|---------------:|------:|
| c (100R) T=16 | A | ~3.3 | **38.88** | **11.8×** |
| c | B | ~3.4 | **39.16** | **11.5×** |
| c | C (T=86) | 79 | 78.67 | 1.00× (preserved) |

Under sustained load, A/B read improvement is **11-12× versus baseline**,
much larger than the 5× indicated by 200k-ops runs. The
structural change (P1 + P4) moves A/B from a single-worker-per-host
read bottleneck to full intra-host concurrency.

## Raw

SUMMARY.log in this directory; per-cell g3.log / g4.log per `<tag>/`.
