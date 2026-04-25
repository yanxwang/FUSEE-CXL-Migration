# iter-4 / kv-varlen — C-only sweep @ FUSEE_VALUE_SIZE=512

- Timestamp: 20260424_184334
- Config: `FUSEE_BATCH_K=4096 FUSEE_BATCH_T_US=100
  FUSEE_VALUE_SIZE=512`. Pool path active; 512-B deterministic
  payload per UPDATE.
- Cache-on peaks (Mops/s @T):
  A 12.09 @T=32, B 25.86 @T=64, C 30.19 @T=86, D 28.53 @T=86,
  F 19.87 @T=64. vs 20 Mops/s bar: A miss, B PASS, F ≈miss (just
  below).
- Measured B 25.86 = 68 % of predicted 38 Mops/s BW ceiling
  (576 B/UPDATE × 2-host ceiling) — mostly BW-bound; approaching
  the ceiling but still some headroom.
- 80/80 runs OK, 0 FAIL.
- See `docs/iter4_variable_kv_summary_20260424.md` for the full
  4-size analysis.
