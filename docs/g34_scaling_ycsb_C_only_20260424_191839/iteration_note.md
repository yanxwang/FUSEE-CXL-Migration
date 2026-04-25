# iter-4 / kv-varlen — C-only sweep @ FUSEE_VALUE_SIZE=1024

- Timestamp: 20260424_191839
- Config: `FUSEE_BATCH_K=4096 FUSEE_BATCH_T_US=100
  FUSEE_VALUE_SIZE=1024`. Pool path active; 1024-B deterministic
  payload per UPDATE.
- Cache-on peaks (Mops/s @T):
  A 13.64 @T=64, B 16.37 @T=64, C 16.78 @T=64, D 16.78 @T=64,
  F 14.80 @T=64. vs 20 Mops/s bar: **all workloads below bar**.
- Measured B 16.37 = 82 % of predicted 20 Mops/s BW ceiling
  (1088 B/UPDATE × 2-host ceiling) — **BW-saturated**. All five
  workloads cluster in the 14-17 Mops/s band, reflecting a
  uniform per-op CXL-write cost that dominates any
  workload-specific lock/contention differences.
- 80/80 runs OK, 0 FAIL.
- Confirms iter-4 hypothesis: at vsize ≥ 1024 B, the 22 GB/s CXL
  seq_write bandwidth is the binding constraint on throughput,
  not write-path latency.
- See `docs/iter4_variable_kv_summary_20260424.md` for the full
  4-size analysis.
