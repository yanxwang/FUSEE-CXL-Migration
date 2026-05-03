# Gap to 20 Mops/s target (per `docs/design_goals.md`)

Peak Mops/s per (workload, KV size), cache=on.

| Workload | KV | Peak Mops/s | At T= | Gap to 20 Mops/s |
|---|---|---|---|---|
| workloada | 256 | 7.596 | 64 | +12.40 (38.0% of target) |
| workloada | 512 | 7.196 | 64 | +12.80 (36.0% of target) |
| workloada | 1024 | 4.825 | 64 | +15.17 (24.1% of target) |
| workloadb | 256 | 11.066 | 64 | +8.93 (55.3% of target) |
| workloadb | 512 | 11.801 | 32 | +8.20 (59.0% of target) |
| workloadb | 1024 | 12.120 | 32 | +7.88 (60.6% of target) |
| workloadc | 256 | 18.208 | 64 | +1.79 (91.0% of target) |
| workloadc | 512 | 11.923 | 32 | +8.08 (59.6% of target) |
| workloadc | 1024 | 18.950 | 64 | +1.05 (94.8% of target) |
| workloadd | 256 | 8.180 | 16 | +11.82 (40.9% of target) |
| workloadd | 512 | 18.000 | 64 | +2.00 (90.0% of target) |
| workloadd | 1024 | 8.322 | 16 | +11.68 (41.6% of target) |
| workloadf | 256 | 9.229 | 64 | +10.77 (46.1% of target) |
| workloadf | 512 | 10.138 | 64 | +9.86 (50.7% of target) |
| workloadf | 1024 | 4.888 | 64 | +15.11 (24.4% of target) |

## All cells (cache=on)

| Workload | KV | T | Mops/s | %% of 20 Mops/s |
|---|---|---|---|---|
| workloada | 256 | 1 | 0.0139 | 0.07% |
| workloada | 512 | 1 | 0.0142 | 0.07% |
| workloada | 1024 | 1 | 0.0144 | 0.07% |
| workloada | 512 | 2 | 0.0764 | 0.38% |
| workloada | 1024 | 2 | 0.0775 | 0.39% |
| workloada | 256 | 4 | 0.3281 | 1.64% |
| workloada | 512 | 4 | 0.3270 | 1.63% |
| workloada | 1024 | 4 | 0.2880 | 1.44% |
| workloada | 512 | 8 | 0.8326 | 4.16% |
| workloada | 1024 | 8 | 1.0136 | 5.07% |
| workloada | 256 | 16 | 0.0005 | 0.00% |
| workloada | 512 | 16 | 2.2002 | 11.00% |
| workloada | 1024 | 16 | 0.0005 | 0.00% |
| workloada | 256 | 32 | 3.6005 | 18.00% |
| workloada | 512 | 32 | 2.4191 | 12.10% |
| workloada | 1024 | 32 | 2.8331 | 14.17% |
| workloada | 256 | 64 | 7.5959 | 37.98% |
| workloada | 512 | 64 | 7.1958 | 35.98% |
| workloada | 1024 | 64 | 4.8252 | 24.13% |
| workloadb | 256 | 1 | 0.0412 | 0.21% |
| workloadb | 512 | 1 | 0.0410 | 0.20% |
| workloadb | 1024 | 1 | 0.0404 | 0.20% |
| workloadb | 256 | 2 | 0.3397 | 1.70% |
| workloadb | 512 | 2 | 0.3182 | 1.59% |
| workloadb | 1024 | 2 | 0.3443 | 1.72% |
| workloadb | 256 | 4 | 0.0004 | 0.00% |
| workloadb | 512 | 4 | 0.9100 | 4.55% |
| workloadb | 1024 | 4 | 0.9703 | 4.85% |
| workloadb | 256 | 8 | 2.1767 | 10.88% |
| workloadb | 512 | 8 | 3.6276 | 18.14% |
| workloadb | 1024 | 8 | 4.1776 | 20.89% |
| workloadb | 256 | 16 | 7.3621 | 36.81% |
| workloadb | 512 | 16 | 6.4727 | 32.36% |
| workloadb | 256 | 32 | 10.9981 | 54.99% |
| workloadb | 512 | 32 | 11.8008 | 59.00% |
| workloadb | 1024 | 32 | 12.1205 | 60.60% |
| workloadb | 256 | 64 | 11.0662 | 55.33% |
| workloadb | 512 | 64 | 8.8250 | 44.12% |
| workloadb | 1024 | 64 | 0.0096 | 0.05% |
| workloadc | 256 | 1 | 2.0297 | 10.15% |
| workloadc | 512 | 1 | 2.0464 | 10.23% |
| workloadc | 1024 | 1 | 2.0403 | 10.20% |
| workloadc | 256 | 2 | 3.8147 | 19.07% |
| workloadc | 512 | 2 | 3.7975 | 18.99% |
| workloadc | 256 | 4 | 5.0707 | 25.35% |
| workloadc | 512 | 4 | 5.5414 | 27.71% |
| workloadc | 1024 | 4 | 0.0005 | 0.00% |
| workloadc | 256 | 8 | 6.4853 | 32.43% |
| workloadc | 512 | 8 | 6.0485 | 30.24% |
| workloadc | 1024 | 8 | 6.2016 | 31.01% |
| workloadc | 256 | 16 | 0.0014 | 0.01% |
| workloadc | 512 | 16 | 9.1228 | 45.61% |
| workloadc | 1024 | 16 | 0.0587 | 0.29% |
| workloadc | 256 | 32 | 12.2205 | 61.10% |
| workloadc | 512 | 32 | 11.9225 | 59.61% |
| workloadc | 1024 | 32 | 12.2234 | 61.12% |
| workloadc | 256 | 64 | 18.2083 | 91.04% |
| workloadc | 512 | 64 | 0.0106 | 0.05% |
| workloadc | 1024 | 64 | 18.9502 | 94.75% |
| workloadd | 256 | 1 | 1.4832 | 7.42% |
| workloadd | 512 | 1 | 1.4712 | 7.36% |
| workloadd | 1024 | 1 | 1.4698 | 7.35% |
| workloadd | 256 | 4 | 4.9865 | 24.93% |
| workloadd | 512 | 4 | 4.4643 | 22.32% |
| workloadd | 1024 | 4 | 0.0003 | 0.00% |
| workloadd | 256 | 8 | 5.5151 | 27.58% |
| workloadd | 512 | 8 | 5.7483 | 28.74% |
| workloadd | 1024 | 8 | 5.5932 | 27.97% |
| workloadd | 256 | 16 | 8.1796 | 40.90% |
| workloadd | 512 | 16 | 8.4062 | 42.03% |
| workloadd | 1024 | 16 | 8.3215 | 41.61% |
| workloadd | 256 | 32 | 0.0010 | 0.00% |
| workloadd | 512 | 32 | 0.0009 | 0.00% |
| workloadd | 1024 | 32 | 0.0045 | 0.02% |
| workloadd | 256 | 64 | 0.0055 | 0.03% |
| workloadd | 512 | 64 | 18.0002 | 90.00% |
| workloadd | 1024 | 64 | 0.0055 | 0.03% |
| workloadf | 256 | 1 | 0.0093 | 0.05% |
| workloadf | 512 | 1 | 0.0090 | 0.04% |
| workloadf | 1024 | 1 | 0.0094 | 0.05% |
| workloadf | 256 | 2 | 0.1709 | 0.85% |
| workloadf | 512 | 2 | 0.1682 | 0.84% |
| workloadf | 256 | 4 | 0.5538 | 2.77% |
| workloadf | 512 | 4 | 0.5528 | 2.76% |
| workloadf | 1024 | 4 | 0.5852 | 2.93% |
| workloadf | 256 | 8 | 0.0003 | 0.00% |
| workloadf | 512 | 8 | 1.4811 | 7.41% |
| workloadf | 1024 | 8 | 1.4414 | 7.21% |
| workloadf | 256 | 16 | 4.1273 | 20.64% |
| workloadf | 512 | 16 | 0.0007 | 0.00% |
| workloadf | 1024 | 16 | 3.1761 | 15.88% |
| workloadf | 256 | 32 | 9.1174 | 45.59% |
| workloadf | 512 | 32 | 0.0011 | 0.01% |
| workloadf | 1024 | 32 | 3.0370 | 15.18% |
| workloadf | 256 | 64 | 9.2289 | 46.14% |
| workloadf | 512 | 64 | 10.1384 | 50.69% |
| workloadf | 1024 | 64 | 4.8878 | 24.44% |
