# iter-5 / multi-flusher V2 — C-only sweep @ kv=1024, N=4

- Timestamp: 20260425_041619
- Config: `FUSEE_BATCH_K=4096 FUSEE_BATCH_T_US=100
  FUSEE_VALUE_SIZE=1024 FUSEE_BATCH_NUM_FLUSHERS=4`. Pool path
  active for kv ≥ 256.
- Cache-on peaks (Mops/s @T): A 13.38@T64, B 15.75@T64,
  C 16.95@T64, D 17.30@T64, F 15.09@T64.
- 80/80 OK, 0 FAIL.
- See `docs/iter5_summary_20260425.md` for the full 9-cell
  (vsize × N) analysis. The summary's ATL;DR`: workload A peaks
  at 19.35 (kv256/N=2 cache=on) — below 25 Mops/s success bar.
  Multi-flusher V2 is correct (zero hangs across 720 runs) but
  not the right lever for A; iter-6 should target hot-bucket
  producer-side serialisation under Zipf.
