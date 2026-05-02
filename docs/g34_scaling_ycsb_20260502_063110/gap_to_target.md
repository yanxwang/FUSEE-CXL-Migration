# Gap to 20 Mops/s target (per `docs/design_goals.md`)

Peak Mops/s per (workload, KV size), cache=on.

| Workload | KV | Peak Mops/s | At T= | Gap to 20 Mops/s |
|---|---|---|---|---|
| workloada | 256 | 17.895 | 64 | +2.10 (89.5% of target) |
| workloada | 512 | 14.443 | 64 | +5.56 (72.2% of target) |
| workloada | 1024 | 0.248 | 64 | +19.75 (1.2% of target) |
| workloadb | 256 | 12.234 | 86 | +7.77 (61.2% of target) |
| workloadb | 512 | 11.647 | 64 | +8.35 (58.2% of target) |
| workloadb | 1024 | 5.270 | 16 | +14.73 (26.4% of target) |
| workloadc | 256 | 12.309 | 86 | +7.69 (61.5% of target) |
| workloadc | 512 | 13.263 | 64 | +6.74 (66.3% of target) |
| workloadc | 1024 | 11.990 | 64 | +8.01 (60.0% of target) |
| workloadd | 256 | 12.243 | 86 | +7.76 (61.2% of target) |
| workloadd | 512 | 6.694 | 32 | +13.31 (33.5% of target) |
| workloadd | 1024 | 11.745 | 86 | +8.25 (58.7% of target) |
| workloadf | 256 | 0.246 | 86 | +19.75 (1.2% of target) |
| workloadf | 512 | 0.245 | 86 | +19.76 (1.2% of target) |
| workloadf | 1024 | 15.615 | 86 | +4.38 (78.1% of target) |

## All cells (cache=on)

| Workload | KV | T | Mops/s | %% of 20 Mops/s |
|---|---|---|---|---|
| workloada | 256 | 1 | 0.0007 | 0.00% |
| workloada | 512 | 1 | 0.0010 | 0.01% |
| workloada | 1024 | 1 | 0.0007 | 0.00% |
| workloada | 256 | 2 | 0.0043 | 0.02% |
| workloada | 512 | 2 | 0.0038 | 0.02% |
| workloada | 1024 | 2 | 0.0038 | 0.02% |
| workloada | 256 | 4 | 0.0080 | 0.04% |
| workloada | 512 | 4 | 0.0108 | 0.05% |
| workloada | 1024 | 4 | 0.0100 | 0.05% |
| workloada | 256 | 8 | 0.0138 | 0.07% |
| workloada | 1024 | 8 | 0.0615 | 0.31% |
| workloada | 256 | 16 | 0.0825 | 0.41% |
| workloada | 1024 | 16 | 0.1209 | 0.60% |
| workloada | 256 | 32 | 0.2451 | 1.23% |
| workloada | 512 | 32 | 0.0006 | 0.00% |
| workloada | 1024 | 32 | 0.0622 | 0.31% |
| workloada | 256 | 64 | 17.8955 | 89.48% |
| workloada | 512 | 64 | 14.4425 | 72.21% |
| workloada | 1024 | 64 | 0.2480 | 1.24% |
| workloada | 256 | 86 | 0.2460 | 1.23% |
| workloada | 512 | 86 | 0.2472 | 1.24% |
| workloada | 1024 | 86 | 0.1246 | 0.62% |
| workloadb | 256 | 1 | 0.0021 | 0.01% |
| workloadb | 512 | 1 | 0.0032 | 0.02% |
| workloadb | 1024 | 1 | 0.0035 | 0.02% |
| workloadb | 256 | 2 | 0.0165 | 0.08% |
| workloadb | 512 | 2 | 0.0223 | 0.11% |
| workloadb | 256 | 4 | 0.0487 | 0.24% |
| workloadb | 512 | 4 | 0.0608 | 0.30% |
| workloadb | 256 | 8 | 0.1222 | 0.61% |
| workloadb | 512 | 8 | 0.0617 | 0.31% |
| workloadb | 256 | 16 | 5.2334 | 26.17% |
| workloadb | 512 | 16 | 0.1238 | 0.62% |
| workloadb | 1024 | 16 | 5.2704 | 26.35% |
| workloadb | 256 | 32 | 0.1243 | 0.62% |
| workloadb | 512 | 32 | 0.2446 | 1.22% |
| workloadb | 1024 | 32 | 0.0008 | 0.00% |
| workloadb | 256 | 64 | 10.9818 | 54.91% |
| workloadb | 512 | 64 | 11.6469 | 58.23% |
| workloadb | 1024 | 64 | 0.0015 | 0.01% |
| workloadb | 256 | 86 | 12.2339 | 61.17% |
| workloadb | 512 | 86 | 0.2466 | 1.23% |
| workloadb | 1024 | 86 | 0.0024 | 0.01% |
| workloadc | 256 | 1 | 0.6700 | 3.35% |
| workloadc | 512 | 1 | 0.6051 | 3.03% |
| workloadc | 1024 | 1 | 0.6863 | 3.43% |
| workloadc | 256 | 2 | 1.3469 | 6.73% |
| workloadc | 512 | 2 | 1.2042 | 6.02% |
| workloadc | 256 | 4 | 2.3950 | 11.97% |
| workloadc | 512 | 4 | 2.1884 | 10.94% |
| workloadc | 1024 | 4 | 2.3336 | 11.67% |
| workloadc | 256 | 8 | 3.7161 | 18.58% |
| workloadc | 512 | 8 | 3.9139 | 19.57% |
| workloadc | 1024 | 8 | 3.8215 | 19.11% |
| workloadc | 256 | 16 | 5.4987 | 27.49% |
| workloadc | 512 | 16 | 5.3220 | 26.61% |
| workloadc | 1024 | 16 | 5.3775 | 26.89% |
| workloadc | 256 | 32 | 7.4738 | 37.37% |
| workloadc | 512 | 32 | 6.7105 | 33.55% |
| workloadc | 1024 | 32 | 6.8795 | 34.40% |
| workloadc | 256 | 64 | 11.6632 | 58.32% |
| workloadc | 512 | 64 | 13.2626 | 66.31% |
| workloadc | 1024 | 64 | 11.9904 | 59.95% |
| workloadc | 256 | 86 | 12.3092 | 61.55% |
| workloadc | 512 | 86 | 12.4813 | 62.41% |
| workloadc | 1024 | 86 | 0.0024 | 0.01% |
| workloadd | 256 | 1 | 0.5140 | 2.57% |
| workloadd | 512 | 1 | 0.4920 | 2.46% |
| workloadd | 1024 | 1 | 0.5044 | 2.52% |
| workloadd | 256 | 2 | 0.9993 | 5.00% |
| workloadd | 512 | 2 | 1.0172 | 5.09% |
| workloadd | 256 | 4 | 1.6816 | 8.41% |
| workloadd | 512 | 4 | 1.9505 | 9.75% |
| workloadd | 1024 | 4 | 1.7984 | 8.99% |
| workloadd | 256 | 8 | 3.6789 | 18.39% |
| workloadd | 512 | 8 | 3.4165 | 17.08% |
| workloadd | 1024 | 8 | 3.5333 | 17.67% |
| workloadd | 256 | 16 | 4.8063 | 24.03% |
| workloadd | 512 | 16 | 5.0761 | 25.38% |
| workloadd | 1024 | 16 | 5.1921 | 25.96% |
| workloadd | 256 | 32 | 6.6208 | 33.10% |
| workloadd | 512 | 32 | 6.6943 | 33.47% |
| workloadd | 1024 | 32 | 6.6997 | 33.50% |
| workloadd | 256 | 64 | 11.8427 | 59.21% |
| workloadd | 512 | 64 | 0.0014 | 0.01% |
| workloadd | 1024 | 64 | 0.0014 | 0.01% |
| workloadd | 256 | 86 | 12.2429 | 61.21% |
| workloadd | 512 | 86 | 0.0018 | 0.01% |
| workloadd | 1024 | 86 | 11.7454 | 58.73% |
| workloadf | 512 | 1 | 0.0005 | 0.00% |
| workloadf | 256 | 2 | 0.0049 | 0.02% |
| workloadf | 512 | 2 | 0.0054 | 0.03% |
| workloadf | 1024 | 2 | 0.0058 | 0.03% |
| workloadf | 256 | 4 | 0.0191 | 0.10% |
| workloadf | 512 | 4 | 0.0124 | 0.06% |
| workloadf | 1024 | 4 | 0.0274 | 0.14% |
| workloadf | 256 | 8 | 0.0178 | 0.09% |
| workloadf | 512 | 8 | 0.0276 | 0.14% |
| workloadf | 1024 | 8 | 0.0616 | 0.31% |
| workloadf | 256 | 16 | 0.0620 | 0.31% |
| workloadf | 512 | 16 | 0.0620 | 0.31% |
| workloadf | 1024 | 16 | 0.0826 | 0.41% |
| workloadf | 256 | 32 | 0.1239 | 0.62% |
| workloadf | 512 | 32 | 0.1239 | 0.62% |
| workloadf | 1024 | 32 | 9.0959 | 45.48% |
| workloadf | 256 | 64 | 0.1240 | 0.62% |
| workloadf | 512 | 64 | 0.1241 | 0.62% |
| workloadf | 1024 | 64 | 13.1510 | 65.75% |
| workloadf | 256 | 86 | 0.2463 | 1.23% |
| workloadf | 512 | 86 | 0.2446 | 1.22% |
| workloadf | 1024 | 86 | 15.6152 | 78.08% |
