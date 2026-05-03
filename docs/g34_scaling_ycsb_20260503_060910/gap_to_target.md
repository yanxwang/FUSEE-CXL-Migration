# Gap to 20 Mops/s target (per `docs/design_goals.md`)

Peak Mops/s per (workload, KV size), cache=on.

| Workload | KV | Peak Mops/s | At T= | Gap to 20 Mops/s |
|---|---|---|---|---|
| workloada | 256 | 1.468 | 16 | +18.53 (7.3% of target) |
| workloada | 512 | 6.941 | 64 | +13.06 (34.7% of target) |
| workloada | 1024 | 8.889 | 64 | +11.11 (44.4% of target) |
| workloadb | 256 | 19.619 | 64 | +0.38 (98.1% of target) |
| workloadb | 512 | 10.355 | 32 | +9.65 (51.8% of target) |
| workloadb | 1024 | 10.208 | 32 | +9.79 (51.0% of target) |
| workloadc | 256 | 11.583 | 32 | +8.42 (57.9% of target) |
| workloadc | 512 | 18.742 | 64 | +1.26 (93.7% of target) |
| workloadc | 1024 | 8.583 | 16 | +11.42 (42.9% of target) |
| workloadd | 256 | 17.884 | 64 | +2.12 (89.4% of target) |
| workloadd | 512 | 18.374 | 64 | +1.63 (91.9% of target) |
| workloadd | 1024 | 17.974 | 64 | +2.03 (89.9% of target) |
| workloadf | 256 | 4.617 | 32 | +15.38 (23.1% of target) |
| workloadf | 512 | 13.544 | 64 | +6.46 (67.7% of target) |
| workloadf | 1024 | 7.557 | 64 | +12.44 (37.8% of target) |

## All cells (cache=on)

| Workload | KV | T | Mops/s | %% of 20 Mops/s |
|---|---|---|---|---|
| workloada | 256 | 1 | 0.0140 | 0.07% |
| workloada | 512 | 1 | 0.0143 | 0.07% |
| workloada | 1024 | 1 | 0.0143 | 0.07% |
| workloada | 256 | 2 | 0.0790 | 0.39% |
| workloada | 256 | 4 | 0.3100 | 1.55% |
| workloada | 512 | 4 | 0.2892 | 1.45% |
| workloada | 1024 | 4 | 0.2669 | 1.33% |
| workloada | 256 | 8 | 1.1432 | 5.72% |
| workloada | 512 | 8 | 0.8576 | 4.29% |
| workloada | 256 | 16 | 1.4675 | 7.34% |
| workloada | 512 | 16 | 2.4276 | 12.14% |
| workloada | 1024 | 16 | 0.0005 | 0.00% |
| workloada | 256 | 32 | 0.0010 | 0.01% |
| workloada | 512 | 32 | 4.9334 | 24.67% |
| workloada | 1024 | 32 | 0.0010 | 0.01% |
| workloada | 256 | 64 | 0.0021 | 0.01% |
| workloada | 512 | 64 | 6.9408 | 34.70% |
| workloada | 1024 | 64 | 8.8893 | 44.45% |
| workloadb | 256 | 1 | 0.0414 | 0.21% |
| workloadb | 512 | 1 | 0.0406 | 0.20% |
| workloadb | 1024 | 1 | 0.0424 | 0.21% |
| workloadb | 256 | 2 | 0.3260 | 1.63% |
| workloadb | 512 | 2 | 0.3093 | 1.55% |
| workloadb | 1024 | 2 | 0.2991 | 1.50% |
| workloadb | 256 | 4 | 0.0004 | 0.00% |
| workloadb | 512 | 4 | 0.9398 | 4.70% |
| workloadb | 1024 | 4 | 0.0004 | 0.00% |
| workloadb | 256 | 8 | 0.0008 | 0.00% |
| workloadb | 512 | 8 | 3.2186 | 16.09% |
| workloadb | 1024 | 8 | 2.7160 | 13.58% |
| workloadb | 256 | 16 | 0.0016 | 0.01% |
| workloadb | 512 | 16 | 5.3087 | 26.54% |
| workloadb | 256 | 32 | 0.0026 | 0.01% |
| workloadb | 512 | 32 | 10.3546 | 51.77% |
| workloadb | 1024 | 32 | 10.2082 | 51.04% |
| workloadb | 256 | 64 | 19.6194 | 98.10% |
| workloadb | 512 | 64 | 6.2543 | 31.27% |
| workloadb | 1024 | 64 | 7.3341 | 36.67% |
| workloadc | 256 | 1 | 2.0150 | 10.08% |
| workloadc | 512 | 1 | 2.0617 | 10.31% |
| workloadc | 1024 | 1 | 2.0452 | 10.23% |
| workloadc | 256 | 2 | 3.7941 | 18.97% |
| workloadc | 512 | 2 | 3.7500 | 18.75% |
| workloadc | 256 | 4 | 5.0555 | 25.28% |
| workloadc | 512 | 4 | 5.3570 | 26.79% |
| workloadc | 1024 | 4 | 0.0005 | 0.00% |
| workloadc | 256 | 8 | 6.5327 | 32.66% |
| workloadc | 512 | 8 | 6.3175 | 31.59% |
| workloadc | 1024 | 8 | 6.1273 | 30.64% |
| workloadc | 256 | 16 | 8.8040 | 44.02% |
| workloadc | 512 | 16 | 8.9726 | 44.86% |
| workloadc | 1024 | 16 | 8.5833 | 42.92% |
| workloadc | 256 | 32 | 11.5828 | 57.91% |
| workloadc | 512 | 32 | 11.3225 | 56.61% |
| workloadc | 1024 | 32 | 0.0294 | 0.15% |
| workloadc | 256 | 64 | 0.0112 | 0.06% |
| workloadc | 512 | 64 | 18.7424 | 93.71% |
| workloadc | 1024 | 64 | 0.0112 | 0.06% |
| workloadd | 256 | 1 | 1.4689 | 7.34% |
| workloadd | 512 | 1 | 1.4759 | 7.38% |
| workloadd | 1024 | 1 | 1.4875 | 7.44% |
| workloadd | 256 | 2 | 2.8750 | 14.38% |
| workloadd | 512 | 2 | 2.8746 | 14.37% |
| workloadd | 1024 | 2 | 2.9021 | 14.51% |
| workloadd | 256 | 4 | 4.5066 | 22.53% |
| workloadd | 512 | 4 | 4.6388 | 23.19% |
| workloadd | 1024 | 4 | 4.6339 | 23.17% |
| workloadd | 256 | 8 | 5.9605 | 29.80% |
| workloadd | 512 | 8 | 5.2221 | 26.11% |
| workloadd | 256 | 16 | 8.1443 | 40.72% |
| workloadd | 512 | 16 | 8.6211 | 43.11% |
| workloadd | 1024 | 16 | 8.1917 | 40.96% |
| workloadd | 256 | 32 | 10.9409 | 54.70% |
| workloadd | 512 | 32 | 11.5942 | 57.97% |
| workloadd | 1024 | 32 | 0.0036 | 0.02% |
| workloadd | 256 | 64 | 17.8843 | 89.42% |
| workloadd | 512 | 64 | 18.3739 | 91.87% |
| workloadd | 1024 | 64 | 17.9743 | 89.87% |
| workloadf | 256 | 1 | 0.0093 | 0.05% |
| workloadf | 512 | 1 | 0.0090 | 0.04% |
| workloadf | 1024 | 1 | 0.0090 | 0.05% |
| workloadf | 256 | 2 | 0.1710 | 0.85% |
| workloadf | 1024 | 2 | 0.1663 | 0.83% |
| workloadf | 256 | 4 | 0.5348 | 2.67% |
| workloadf | 1024 | 4 | 0.5905 | 2.95% |
| workloadf | 256 | 8 | 1.5122 | 7.56% |
| workloadf | 512 | 8 | 0.0003 | 0.00% |
| workloadf | 1024 | 8 | 1.4834 | 7.42% |
| workloadf | 256 | 16 | 4.0216 | 20.11% |
| workloadf | 512 | 16 | 3.3012 | 16.51% |
| workloadf | 1024 | 16 | 0.0007 | 0.00% |
| workloadf | 256 | 32 | 4.6172 | 23.09% |
| workloadf | 512 | 32 | 0.0014 | 0.01% |
| workloadf | 1024 | 32 | 2.8202 | 14.10% |
| workloadf | 256 | 64 | 4.2285 | 21.14% |
| workloadf | 512 | 64 | 13.5437 | 67.72% |
| workloadf | 1024 | 64 | 7.5569 | 37.78% |


## §13 gate 5 anomaly scan (dual-condition threshold)

Threshold: cell flagged if Mops/s < 0.1 absolute OR < (same-(wl, kv) T-neighbor geomean) / 10.

**55 anomaly cells** flagged (HARD FAIL on §13 gate 5 unless explained):

| Workload | KV | T | Cache | Mops/s | Reason |
|---|---|---|---|---|---|
| workloada | 256 | 1 | off | 0.0144 | < 0.1 abs; < neighbor-geomean(0.299)/10 |
| workloada | 256 | 2 | off | 0.0797 | < 0.1 abs |
| workloada | 256 | 64 | off | 0.0021 | < 0.1 abs; < neighbor-geomean(0.413)/10 |
| workloada | 512 | 1 | off | 0.0144 | < 0.1 abs; < neighbor-geomean(1.076)/10 |
| workloada | 512 | 2 | off | 0.0788 | < 0.1 abs; < neighbor-geomean(0.810)/10 |
| workloada | 1024 | 1 | off | 0.0144 | < 0.1 abs |
| workloada | 1024 | 32 | off | 0.0010 | < 0.1 abs; < neighbor-geomean(0.120)/10 |
| workloada | 1024 | 64 | off | 0.0021 | < 0.1 abs; < neighbor-geomean(0.103)/10 |
| workloada | 256 | 1 | on | 0.0140 | < 0.1 abs |
| workloada | 256 | 2 | on | 0.0790 | < 0.1 abs |
| workloada | 256 | 32 | on | 0.0010 | < 0.1 abs; < neighbor-geomean(0.103)/10 |
| workloada | 256 | 64 | on | 0.0021 | < 0.1 abs; < neighbor-geomean(0.092)/10 |
| workloada | 512 | 1 | on | 0.0143 | < 0.1 abs; < neighbor-geomean(1.832)/10 |
| workloada | 1024 | 1 | on | 0.0143 | < 0.1 abs |
| workloada | 1024 | 16 | on | 0.0005 | < 0.1 abs; < neighbor-geomean(0.077)/10 |
| workloada | 1024 | 32 | on | 0.0010 | < 0.1 abs; < neighbor-geomean(0.064)/10 |
| workloadb | 256 | 1 | off | 0.0408 | < 0.1 abs; < neighbor-geomean(0.817)/10 |
| workloadb | 256 | 64 | off | 0.0066 | < 0.1 abs; < neighbor-geomean(1.108)/10 |
| workloadb | 512 | 1 | off | 0.0408 | < 0.1 abs; < neighbor-geomean(0.900)/10 |
| workloadb | 512 | 4 | off | 0.0004 | < 0.1 abs; < neighbor-geomean(2.319)/10 |
| workloadb | 1024 | 1 | off | 0.0390 | < 0.1 abs |
| workloadb | 1024 | 8 | off | 0.0008 | < 0.1 abs; < neighbor-geomean(0.431)/10 |
| workloadb | 1024 | 64 | off | 0.0079 | < 0.1 abs; < neighbor-geomean(0.293)/10 |
| workloadb | 256 | 1 | on | 0.0414 | < 0.1 abs |
| workloadb | 256 | 4 | on | 0.0004 | < 0.1 abs; < neighbor-geomean(0.031)/10 |
| workloadb | 256 | 8 | on | 0.0008 | < 0.1 abs; < neighbor-geomean(0.027)/10 |
| workloadb | 256 | 16 | on | 0.0016 | < 0.1 abs; < neighbor-geomean(0.024)/10 |
| workloadb | 256 | 32 | on | 0.0026 | < 0.1 abs |
| workloadb | 512 | 1 | on | 0.0406 | < 0.1 abs; < neighbor-geomean(2.618)/10 |
| workloadb | 1024 | 1 | on | 0.0424 | < 0.1 abs; < neighbor-geomean(0.465)/10 |
| workloadb | 1024 | 4 | on | 0.0004 | < 0.1 abs; < neighbor-geomean(1.208)/10 |
| workloadc | 256 | 4 | off | 0.0005 | < 0.1 abs; < neighbor-geomean(0.058)/10 |
| workloadc | 256 | 8 | off | 0.0010 | < 0.1 abs; < neighbor-geomean(0.049)/10 |
| workloadc | 256 | 16 | off | 0.0022 | < 0.1 abs; < neighbor-geomean(0.043)/10 |
| workloadc | 256 | 64 | off | 0.0112 | < 0.1 abs |
| workloadc | 512 | 4 | off | 0.0005 | < 0.1 abs; < neighbor-geomean(1.377)/10 |
| workloadc | 512 | 32 | off | 0.0024 | < 0.1 abs; < neighbor-geomean(0.993)/10 |
| workloadc | 256 | 64 | on | 0.0112 | < 0.1 abs; < neighbor-geomean(5.434)/10 |
| workloadc | 1024 | 4 | on | 0.0005 | < 0.1 abs; < neighbor-geomean(0.513)/10 |
| workloadc | 1024 | 32 | on | 0.0294 | < 0.1 abs |
| workloadc | 1024 | 64 | on | 0.0112 | < 0.1 abs; < neighbor-geomean(0.272)/10 |
| workloadd | 512 | 8 | off | 0.0007 | < 0.1 abs; < neighbor-geomean(1.226)/10 |
| workloadd | 512 | 32 | off | 0.0012 | < 0.1 abs; < neighbor-geomean(1.113)/10 |
| workloadd | 1024 | 32 | off | 0.0041 | < 0.1 abs; < neighbor-geomean(4.985)/10 |
| workloadd | 1024 | 32 | on | 0.0036 | < 0.1 abs; < neighbor-geomean(4.941)/10 |
| workloadf | 256 | 1 | off | 0.0091 | < 0.1 abs; < neighbor-geomean(1.954)/10 |
| workloadf | 512 | 1 | off | 0.0091 | < 0.1 abs; < neighbor-geomean(0.476)/10 |
| workloadf | 512 | 64 | off | 0.0027 | < 0.1 abs; < neighbor-geomean(0.608)/10 |
| workloadf | 1024 | 1 | off | 0.0094 | < 0.1 abs; < neighbor-geomean(4.273)/10 |
| workloadf | 256 | 1 | on | 0.0093 | < 0.1 abs; < neighbor-geomean(1.488)/10 |
| workloadf | 512 | 1 | on | 0.0090 | < 0.1 abs |
| workloadf | 512 | 8 | on | 0.0003 | < 0.1 abs; < neighbor-geomean(0.153)/10 |
| workloadf | 512 | 32 | on | 0.0014 | < 0.1 abs; < neighbor-geomean(0.108)/10 |
| workloadf | 1024 | 1 | on | 0.0090 | < 0.1 abs; < neighbor-geomean(0.358)/10 |
| workloadf | 1024 | 16 | on | 0.0007 | < 0.1 abs; < neighbor-geomean(0.551)/10 |

**Per spec §13 gate 5 (added iter-7A 2026-05-03)**: each anomaly must be explained with **5-rep multi-rep evidence** of "genuine noise, not a bug", OR the iter cannot be marked COMPLETE. "Single-rep noise" tag without 5-rep evidence is the iter-6A failure pattern explicitly forbidden.
