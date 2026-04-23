# Scaling-YCSB run index

Per `docs/scaling_ycsb_spec.md` §10, append one line here after every run.

| timestamp | phase | git SHA | notes |
|-----------|-------|---------|-------|
| 20260422_034858 | baseline (pre-improvement) | `a0595ef` | 240-run full matrix; A/B clamped to 1 per host (PendingRing per-host only). Baseline peaks: A/B workloadc T=86 3.3 Mops/s, C workloadc T=86 48 Mops/s. |
| 20260422_153439 | Phase 1 + Phase 4 | `67790c6` | A/B unclamped via read-only attach (P1) + per-client PendingRing matrix (P4). A/B capped at T=16 due to O(N²) ring traffic at T≥32. A/B workloadc T=16 jumps to 17.8 / 18.1 Mops/s (5.4×). A/B write-lite workloads +40 % to +100 %. C workloadc T=86 regressed 48 → 5.7 Mops/s (region grew 2.5 GB → 6.4 GB, cache fit issue). See `docs/g34_scaling_ycsb_20260422_153439/analysis.md`. |
| 20260422_162908 | Phase 1 + 4 + 5 combined (K=4 for A) | `7196f2e` | 180-run full sweep. A/B read workloadc T=16: 18.1 / 18.0 Mops/s (5.4× vs baseline). A write workloads +44-92 %. B writes +25-210 %. C preserved (no regression this run). A/B still capped at T=16 per host. See `docs/g34_scaling_ycsb_20260422_162908/analysis.md`. |
| 20260423_051200 | iter1 C write-path: per-slot LFM (FUSEE_PER_SLOT_LOCK=ON) | working-tree on `feat/cxl-migration` (uncommitted) | 80-run C-only sweep for task `docs/task_plan_20260423_c_writepath.md`. Per-slot LFM shrinks lock p99 at T=64 on workload A by 29× (12.8 ms → 440 µs). Workload A peak 1.08 → 3.41 Mops/s (3.16×), B peak 6.55 → 9.98 (1.52×), F peak 1.44 → 3.53 (2.45×). Still 5.9× / 2.0× / 5.7× below the 20 Mops/s north-star bar; C/D slightly regressed (17 GiB SlotLockTable init cost). See `docs/g34_scaling_ycsb_C_only_20260423_051200/iteration_note.md`. |
