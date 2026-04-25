# iter-5 / multi-flusher V2 — C-only sweep @ kv=256, N=4

- Timestamp: 20260425_004507
- Config: `FUSEE_BATCH_K=4096 FUSEE_BATCH_T_US=100
  FUSEE_VALUE_SIZE=256 FUSEE_BATCH_NUM_FLUSHERS=4`. Pool path
  active for kv ≥ 256.
- Cache-on peaks (Mops/s @T): A 17.32@T64, B 26.36@T64,
  C 34.23@T86, D 33.11@T86, F 20.38@T64.
- 80/80 OK, 0 FAIL.
- See `docs/iter5_summary_20260425.md` for the full 9-cell
  (vsize × N) analysis. The summary's ATL;DR`: workload A peaks
  at 19.35 (kv256/N=2 cache=on) — below 25 Mops/s success bar.
  Multi-flusher V2 is correct (zero hangs across 720 runs) but
  not the right lever for A; iter-6 should target hot-bucket
  producer-side serialisation under Zipf.
