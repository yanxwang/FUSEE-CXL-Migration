# iter-4 / kv-varlen — C-only sweep @ FUSEE_VALUE_SIZE=256

- Timestamp: 20260424_180808
- Config: `FUSEE_BATCH_K=4096 FUSEE_BATCH_T_US=100
  FUSEE_VALUE_SIZE=256`. Pool path active (`use_pool=true`): each
  UPDATE writes `(key_seed>>(8*(i&7))) ^ (i*31)` deterministic
  256-B payload through the per-host bump-alloc CXL pool; slot
  stores the pool byte-offset in lieu of the inline u64.
- Cache-on peaks (Mops/s @T):
  A 17.18 @T=64, B 28.89 @T=64, C 31.25 @T=64, D 34.37 @T=86,
  **F 21.24 @T=64**. vs 20 Mops/s bar: A miss, B PASS, **F PASS**.
- Measured B 28.89 = 42 % of predicted 68 Mops/s BW ceiling
  (320 B/UPDATE × 2-host ceiling) — still latency-bound.
- 80/80 runs OK, 0 FAIL.
- See `docs/iter4_variable_kv_summary_20260424.md` for the full
  4-size analysis.
