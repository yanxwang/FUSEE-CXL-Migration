# Gap to 20 Mops/s target (per `docs/design_goals.md`)

Peak Mops/s per (workload, KV size), cache=on.

| Workload | KV | Peak Mops/s | At T= | Gap to 20 Mops/s |
|---|---|---|---|---|
| workloada | 256 | 6.965 | 64 | +13.04 (34.8% of target) |
| workloada | 512 | 0.083 | 64 | +19.92 (0.4% of target) |
| workloada | 1024 | 1.952 | 16 | +18.05 (9.8% of target) |
| workloadb | 256 | 10.961 | 32 | +9.04 (54.8% of target) |
| workloadb | 512 | 18.622 | 64 | +1.38 (93.1% of target) |
| workloadb | 1024 | 15.080 | 64 | +4.92 (75.4% of target) |
| workloadc | 256 | 13.371 | 32 | +6.63 (66.9% of target) |
| workloadc | 512 | 18.182 | 64 | +1.82 (90.9% of target) |
| workloadc | 1024 | 18.237 | 64 | +1.76 (91.2% of target) |
| workloadd | 256 | 16.948 | 64 | +3.05 (84.7% of target) |
| workloadd | 512 | 9.736 | 16 | +10.26 (48.7% of target) |
| workloadd | 1024 | 9.276 | 16 | +10.72 (46.4% of target) |
| workloadf | 256 | 9.498 | 32 | +10.50 (47.5% of target) |
| workloadf | 512 | 3.577 | 16 | +16.42 (17.9% of target) |
| workloadf | 1024 | 12.468 | 64 | +7.53 (62.3% of target) |

## All cells (cache=on)

| Workload | KV | T | Mops/s | %% of 20 Mops/s |
|---|---|---|---|---|
| workloada | 256 | 1 | 0.0144 | 0.07% |
| workloada | 512 | 1 | 0.0145 | 0.07% |
| workloada | 1024 | 1 | 0.0142 | 0.07% |
| workloada | 256 | 2 | 0.0806 | 0.40% |
| workloada | 512 | 2 | 0.0024 | 0.01% |
| workloada | 1024 | 2 | 0.0799 | 0.40% |
| workloada | 256 | 4 | 0.2839 | 1.42% |
| workloada | 512 | 4 | 0.0049 | 0.02% |
| workloada | 1024 | 4 | 0.3080 | 1.54% |
| workloada | 256 | 8 | 0.9284 | 4.64% |
| workloada | 512 | 8 | 0.0099 | 0.05% |
| workloada | 1024 | 8 | 0.9135 | 4.57% |
| workloada | 256 | 16 | 1.9927 | 9.96% |
| workloada | 512 | 16 | 0.0201 | 0.10% |
| workloada | 1024 | 16 | 1.9515 | 9.76% |
| workloada | 256 | 32 | 6.2641 | 31.32% |
| workloada | 512 | 32 | 0.0410 | 0.21% |
| workloada | 1024 | 32 | 0.0407 | 0.20% |
| workloada | 256 | 64 | 6.9650 | 34.83% |
| workloada | 512 | 64 | 0.0832 | 0.42% |
| workloada | 1024 | 64 | 0.0831 | 0.42% |
| workloadb | 256 | 1 | 0.0424 | 0.21% |
| workloadb | 512 | 1 | 0.0402 | 0.20% |
| workloadb | 1024 | 1 | 0.0401 | 0.20% |
| workloadb | 256 | 2 | 0.3270 | 1.63% |
| workloadb | 512 | 2 | 0.3017 | 1.51% |
| workloadb | 1024 | 2 | 0.3284 | 1.64% |
| workloadb | 256 | 4 | 0.9194 | 4.60% |
| workloadb | 512 | 4 | 0.8216 | 4.11% |
| workloadb | 1024 | 4 | 1.0445 | 5.22% |
| workloadb | 256 | 8 | 3.0087 | 15.04% |
| workloadb | 512 | 8 | 3.7831 | 18.92% |
| workloadb | 1024 | 8 | 2.7554 | 13.78% |
| workloadb | 256 | 16 | 6.1931 | 30.97% |
| workloadb | 512 | 16 | 7.1475 | 35.74% |
| workloadb | 1024 | 16 | 5.0589 | 25.29% |
| workloadb | 256 | 32 | 10.9607 | 54.80% |
| workloadb | 512 | 32 | 9.5479 | 47.74% |
| workloadb | 1024 | 32 | 9.7537 | 48.77% |
| workloadb | 256 | 64 | 0.3166 | 1.58% |
| workloadb | 512 | 64 | 18.6220 | 93.11% |
| workloadb | 1024 | 64 | 15.0795 | 75.40% |
| workloadc | 256 | 1 | 1.6584 | 8.29% |
| workloadc | 512 | 1 | 1.6039 | 8.02% |
| workloadc | 1024 | 1 | 1.5308 | 7.65% |
| workloadc | 256 | 2 | 3.1648 | 15.82% |
| workloadc | 512 | 2 | 3.0434 | 15.22% |
| workloadc | 1024 | 2 | 0.0080 | 0.04% |
| workloadc | 256 | 4 | 5.0247 | 25.12% |
| workloadc | 512 | 4 | 0.0187 | 0.09% |
| workloadc | 1024 | 4 | 0.0187 | 0.09% |
| workloadc | 256 | 8 | 7.3180 | 36.59% |
| workloadc | 512 | 8 | 7.1544 | 35.77% |
| workloadc | 1024 | 8 | 7.0062 | 35.03% |
| workloadc | 256 | 16 | 0.0870 | 0.43% |
| workloadc | 512 | 16 | 10.4482 | 52.24% |
| workloadc | 1024 | 16 | 10.2659 | 51.33% |
| workloadc | 256 | 32 | 13.3708 | 66.85% |
| workloadc | 512 | 32 | 13.0847 | 65.42% |
| workloadc | 1024 | 32 | 13.0132 | 65.07% |
| workloadc | 256 | 64 | 0.4480 | 2.24% |
| workloadc | 512 | 64 | 18.1818 | 90.91% |
| workloadc | 1024 | 64 | 18.2365 | 91.18% |
| workloadd | 256 | 1 | 1.2516 | 6.26% |
| workloadd | 512 | 1 | 1.2173 | 6.09% |
| workloadd | 1024 | 1 | 1.1517 | 5.76% |
| workloadd | 256 | 2 | 2.4558 | 12.28% |
| workloadd | 512 | 2 | 0.0060 | 0.03% |
| workloadd | 1024 | 2 | 2.2730 | 11.37% |
| workloadd | 256 | 4 | 4.6183 | 23.09% |
| workloadd | 512 | 4 | 0.0135 | 0.07% |
| workloadd | 1024 | 4 | 4.2276 | 21.14% |
| workloadd | 256 | 8 | 6.5632 | 32.82% |
| workloadd | 512 | 8 | 6.5559 | 32.78% |
| workloadd | 1024 | 8 | 0.0282 | 0.14% |
| workloadd | 256 | 16 | 9.8391 | 49.20% |
| workloadd | 512 | 16 | 9.7357 | 48.68% |
| workloadd | 1024 | 16 | 9.2756 | 46.38% |
| workloadd | 256 | 32 | 12.8783 | 64.39% |
| workloadd | 512 | 32 | 0.1405 | 0.70% |
| workloadd | 1024 | 32 | 0.1236 | 0.62% |
| workloadd | 256 | 64 | 16.9477 | 84.74% |
| workloadd | 512 | 64 | 0.0885 | 0.44% |
| workloadd | 1024 | 64 | 0.1452 | 0.73% |
| workloadf | 256 | 1 | 0.0094 | 0.05% |
| workloadf | 512 | 1 | 0.0093 | 0.05% |
| workloadf | 256 | 2 | 0.1679 | 0.84% |
| workloadf | 512 | 2 | 0.1712 | 0.86% |
| workloadf | 1024 | 2 | 0.1229 | 0.61% |
| workloadf | 256 | 4 | 0.5460 | 2.73% |
| workloadf | 512 | 4 | 0.5529 | 2.76% |
| workloadf | 1024 | 4 | 0.4579 | 2.29% |
| workloadf | 256 | 8 | 1.6405 | 8.20% |
| workloadf | 512 | 8 | 1.4855 | 7.43% |
| workloadf | 1024 | 8 | 1.2227 | 6.11% |
| workloadf | 256 | 16 | 5.7130 | 28.56% |
| workloadf | 512 | 16 | 3.5768 | 17.88% |
| workloadf | 1024 | 16 | 3.1748 | 15.87% |
| workloadf | 256 | 32 | 9.4985 | 47.49% |
| workloadf | 512 | 32 | 0.0542 | 0.27% |
| workloadf | 1024 | 32 | 7.7465 | 38.73% |
| workloadf | 256 | 64 | 0.1065 | 0.53% |
| workloadf | 512 | 64 | 0.1064 | 0.53% |
| workloadf | 1024 | 64 | 12.4680 | 62.34% |


## §13 gate 5 anomaly scan (dual-condition threshold)

Threshold: cell flagged if Mops/s < 0.1 absolute OR < (same-(wl, kv) T-neighbor geomean) / 10.

**55 anomaly cells** flagged (HARD FAIL on §13 gate 5 unless explained):

| Workload | KV | T | Cache | Mops/s | Reason |
|---|---|---|---|---|---|
| workloada | 256 | 1 | off | 0.0145 | < 0.1 abs; < neighbor-geomean(1.071)/10 |
| workloada | 256 | 2 | off | 0.0801 | < 0.1 abs; < neighbor-geomean(0.806)/10 |
| workloada | 512 | 1 | off | 0.0145 | < 0.1 abs; < neighbor-geomean(0.546)/10 |
| workloada | 512 | 2 | off | 0.0804 | < 0.1 abs |
| workloada | 512 | 64 | off | 0.0832 | < 0.1 abs |
| workloada | 1024 | 1 | off | 0.0143 | < 0.1 abs; < neighbor-geomean(0.557)/10 |
| workloada | 1024 | 2 | off | 0.0024 | < 0.1 abs; < neighbor-geomean(0.752)/10 |
| workloada | 256 | 1 | on | 0.0144 | < 0.1 abs; < neighbor-geomean(1.108)/10 |
| workloada | 256 | 2 | on | 0.0806 | < 0.1 abs; < neighbor-geomean(0.831)/10 |
| workloada | 512 | 1 | on | 0.0145 | < 0.1 abs |
| workloada | 512 | 2 | on | 0.0024 | < 0.1 abs |
| workloada | 512 | 4 | on | 0.0049 | < 0.1 abs |
| workloada | 512 | 8 | on | 0.0099 | < 0.1 abs |
| workloada | 512 | 16 | on | 0.0201 | < 0.1 abs |
| workloada | 512 | 32 | on | 0.0410 | < 0.1 abs |
| workloada | 512 | 64 | on | 0.0832 | < 0.1 abs |
| workloada | 1024 | 1 | on | 0.0142 | < 0.1 abs; < neighbor-geomean(0.230)/10 |
| workloada | 1024 | 2 | on | 0.0799 | < 0.1 abs |
| workloada | 1024 | 32 | on | 0.0407 | < 0.1 abs |
| workloada | 1024 | 64 | on | 0.0831 | < 0.1 abs |
| workloadb | 256 | 1 | off | 0.0410 | < 0.1 abs; < neighbor-geomean(0.428)/10 |
| workloadb | 256 | 2 | off | 0.0063 | < 0.1 abs; < neighbor-geomean(0.585)/10 |
| workloadb | 256 | 4 | off | 0.0143 | < 0.1 abs; < neighbor-geomean(0.511)/10 |
| workloadb | 512 | 1 | off | 0.0403 | < 0.1 abs; < neighbor-geomean(2.701)/10 |
| workloadb | 1024 | 1 | off | 0.0414 | < 0.1 abs; < neighbor-geomean(1.531)/10 |
| workloadb | 1024 | 16 | off | 0.0629 | < 0.1 abs; < neighbor-geomean(1.428)/10 |
| workloadb | 256 | 1 | on | 0.0424 | < 0.1 abs; < neighbor-geomean(1.640)/10 |
| workloadb | 512 | 1 | on | 0.0402 | < 0.1 abs; < neighbor-geomean(3.256)/10 |
| workloadb | 1024 | 1 | on | 0.0401 | < 0.1 abs; < neighbor-geomean(2.982)/10 |
| workloadc | 256 | 2 | off | 0.0080 | < 0.1 abs; < neighbor-geomean(1.224)/10 |
| workloadc | 256 | 4 | off | 0.0187 | < 0.1 abs; < neighbor-geomean(1.062)/10 |
| workloadc | 256 | 8 | off | 0.0419 | < 0.1 abs; < neighbor-geomean(0.929)/10 |
| workloadc | 256 | 16 | on | 0.0870 | < 0.1 abs; < neighbor-geomean(3.240)/10 |
| workloadc | 512 | 4 | on | 0.0187 | < 0.1 abs; < neighbor-geomean(6.654)/10 |
| workloadc | 1024 | 2 | on | 0.0080 | < 0.1 abs; < neighbor-geomean(2.807)/10 |
| workloadc | 1024 | 4 | on | 0.0187 | < 0.1 abs; < neighbor-geomean(2.434)/10 |
| workloadd | 256 | 2 | off | 0.0060 | < 0.1 abs; < neighbor-geomean(0.787)/10 |
| workloadd | 256 | 4 | off | 0.0135 | < 0.1 abs; < neighbor-geomean(0.688)/10 |
| workloadd | 1024 | 4 | off | 0.0133 | < 0.1 abs; < neighbor-geomean(2.439)/10 |
| workloadd | 1024 | 16 | off | 0.0576 | < 0.1 abs; < neighbor-geomean(1.911)/10 |
| workloadd | 512 | 2 | on | 0.0060 | < 0.1 abs; < neighbor-geomean(0.485)/10 |
| workloadd | 512 | 4 | on | 0.0135 | < 0.1 abs; < neighbor-geomean(0.424)/10 |
| workloadd | 512 | 64 | on | 0.0885 | < 0.1 abs |
| workloadd | 1024 | 8 | on | 0.0282 | < 0.1 abs; < neighbor-geomean(1.107)/10 |
| workloadf | 256 | 1 | off | 0.0092 | < 0.1 abs; < neighbor-geomean(2.021)/10 |
| workloadf | 512 | 1 | off | 0.0093 | < 0.1 abs; < neighbor-geomean(0.169)/10 |
| workloadf | 512 | 4 | off | 0.0066 | < 0.1 abs; < neighbor-geomean(0.179)/10 |
| workloadf | 512 | 16 | off | 0.0269 | < 0.1 abs |
| workloadf | 512 | 32 | off | 0.0542 | < 0.1 abs |
| workloadf | 1024 | 1 | off | 0.0091 | < 0.1 abs; < neighbor-geomean(0.981)/10 |
| workloadf | 1024 | 4 | off | 0.0065 | < 0.1 abs; < neighbor-geomean(1.036)/10 |
| workloadf | 256 | 1 | on | 0.0094 | < 0.1 abs; < neighbor-geomean(0.977)/10 |
| workloadf | 512 | 1 | on | 0.0093 | < 0.1 abs; < neighbor-geomean(0.378)/10 |
| workloadf | 512 | 32 | on | 0.0542 | < 0.1 abs |
| workloadf | 1024 | 2 | on | 0.1229 | < neighbor-geomean(2.799)/10 |

**Per spec §13 gate 5 (added iter-7A 2026-05-03)**: each anomaly must be explained with **5-rep multi-rep evidence** of "genuine noise, not a bug", OR the iter cannot be marked COMPLETE. "Single-rep noise" tag without 5-rep evidence is the iter-6A failure pattern explicitly forbidden.
