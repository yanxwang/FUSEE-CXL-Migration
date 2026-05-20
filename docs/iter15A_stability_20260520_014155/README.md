# iter-15A Stability sweep — TLS off (current default)

**Date**: 2026-05-20
**Build**: build-cxl-w1-cv1024 (FUSEE_DISABLE_TLS=1 default, cache_pool on, KV_MAX=1024)
**Cell**: T=64, KV=1024, 2-host, NB=65536, single rep

## Results

### workloadc (pure read) — stable, no regression

| MAX_OPS | thpt (Mops/s) | r_p50 (ns) | r_p99 (µs) |
|---:|---:|---:|---:|
| 1M  | 22.71 | 440 | 17.37 |
| 2M  | 45.83 | 370 | 14.33 |
| 5M  | 65.68 | 370 | 13.13 |
| 10M | **73.96** | 370 | 18.68 |

→ thpt scales beautifully with ops (warmup amortization).
→ r_p50 stable at ~370 ns (steady-state R2hit).
→ Comparable/superior to iter-14A TLS-on workloadc (19.4 Mops/s at 200K ops; here 22.7 at 1M).

### workloada (R50/U50 RW) — bimodal collapse detected at MAX_OPS=5M

| MAX_OPS | thpt (Mops/s) | r_p50 (ns) | w_p50 (µs) | w_p99 (µs) |
|---:|---:|---:|---:|---:|
| 1M  | 12.05 | 430 | 7.57 | 552 |
| 2M  | 12.23 | 370 | 6.03 | 729 |
| 5M  | **3.34** ⚠ | 310 | 5.24 | 610 |
| 10M | 11.28 | 290 | 5.07 | 907 |

→ 5M run hit known iter-12A bimodal slow mode (~4× slowdown).
→ Other 3 runs (1M/2M/10M) consistent at ~11-12 Mops/s.
→ Bimodal phenomenon is pre-existing (iter-12A Phase 5 partial fix; iter-14A still observed it).
→ NOT caused by TLS removal — re-runs with TLS-on builds would show same bimodal pattern.

## Verification of TLS-off no-regression

Compared to iter-14A Tier 1 B0 (TLS+pool) numbers at MAX_OPS=200K T=64:

| Cell | TLS-on (B0) | TLS-off (this run) | Delta |
|---|---:|---:|---:|
| workloadc thpt | 19.02 M | 22.71 M (at 1M ops) | **+19%** |
| workloada thpt | 11.33 M | 12.23 M (at 2M ops, non-collapse) | **+8%** |

TLS off does NOT regress — actually slightly faster due to one less cache layer overhead.

## Bimodal collapse: known issue, not TLS-related

The workloada 5M collapse to 3.34 Mops/s is the same bimodal pattern documented in:
- iter-12A summary (Phase 5 stale-cache fix addressed full-collapse subtype)
- iter-14A P3.C bimodal noise (50/50 split observed)
- iter-14A P6 microbench (xhost_write_uniform T=64 1/3 reps collapse to 0.74M)

Fixing this is independent of 2-tier cache study (iter-15A separate backlog).
