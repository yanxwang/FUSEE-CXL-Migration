# iter-4 / kv-varlen — C-only sweep @ FUSEE_VALUE_SIZE=8

- Timestamp: 20260424_173316
- Config: `FUSEE_BATCH_K=4096 FUSEE_BATCH_T_US=100` (iter-3
  phase-3 micro-batching), `FUSEE_VALUE_SIZE=8`. Because
  `kValueSize <= sizeof(uint64_t)` the runner skips pool attach
  and C stays on the iter-3 inline u64 fast path (byte-for-byte
  identical code path).
- Purpose: apples-to-apples kv-8 baseline for the cross-size
  comparison in `docs/iter4_variable_kv_summary_20260424.md`.
- Cache-on peaks (Mops/s @T):
  A 13.94 @T=86, B 32.15 @T=86, C 57.72 @T=86, D 48.01 @T=86,
  F 16.18 @T=86. vs 20 Mops/s bar: **A miss, B PASS, F miss**.
  Within run-to-run envelope of iter-3 phase-3 (A 17.05 / B 33.37
  / F 20.48); the slight A/F softening vs iter-3 is attributed to
  the new per-client value buffer plumbing and NUMA/thermal state.
- 80/80 runs OK, 0 FAIL.
- See `docs/iter4_variable_kv_summary_20260424.md` for the full
  4-size analysis.
