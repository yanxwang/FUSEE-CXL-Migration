# Scaling-YCSB run index

Per `docs/scaling_ycsb_spec.md` §10, append one line here after every run.

| timestamp | phase | git SHA | notes |
|-----------|-------|---------|-------|
| 20260422_034858 | baseline (pre-improvement) | `a0595ef` | 240-run full matrix; A/B clamped to 1 per host (PendingRing per-host only). Baseline peaks: A/B workloadc T=86 3.3 Mops/s, C workloadc T=86 48 Mops/s. |
| 20260422_153439 | Phase 1 + Phase 4 | `67790c6` | A/B unclamped via read-only attach (P1) + per-client PendingRing matrix (P4). A/B capped at T=16 due to O(N²) ring traffic at T≥32. A/B workloadc T=16 jumps to 17.8 / 18.1 Mops/s (5.4×). A/B write-lite workloads +40 % to +100 %. C workloadc T=86 regressed 48 → 5.7 Mops/s (region grew 2.5 GB → 6.4 GB, cache fit issue). See `docs/g34_scaling_ycsb_20260422_153439/analysis.md`. |
