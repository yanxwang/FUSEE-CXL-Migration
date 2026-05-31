# iter-19A Phase 0 baseline reproduction

Date: Sun May 31 01:53:33 AM CDT 2026
Platform: g1+g2 (CXL x8 era, post-2026-05-27)
Reference: iter-15A Phase 3+4 on g3+g4 (CXL x16 era)

## Anomaly A (cache_pct sweep)

| cache_pct | g1/g2 thpt (Mops) | iter-15A thpt (Mops) |
|---:|---:|---:|
| 1 | 65.61 | 65.73 |
| 2 | 62.44 | 62.44 |
| 5 | 55.89 | 55.88 |
| 10 | 47.51 | 47.55 |
| 20 | 38.70 | 38.52 |
| 50 | 32.11 | 32.06 |
| 100 | 25.38 | 25.98 |

Ratio cache_pct=100/cache_pct=1: g1/g2 = **0.387** vs iter-15A 0.395

## Anomaly B (dist sweep, cache_pct=10)

| dist | g1/g2 thpt (Mops) | iter-15A thpt (Mops) |
|---|---:|---:|
| uniform | 29.98 | 29.52 |
| zipf-0.5 | 40.65 | 40.52 |
| zipf-0.99 | 47.51 | 47.67 |
| zipf-1.5 | 16.42 | 14.81 |

Ratio zipf-1.5/zipf-0.99: g1/g2 = **0.346** vs iter-15A 0.311

## Verdict

- Anomaly A: PARTIAL reproduction
- Anomaly B: PARTIAL reproduction

**→ CONTINUE to Phase 1 (cache size anomaly) + Phase 2 (zipf-1.5 anomaly)**
