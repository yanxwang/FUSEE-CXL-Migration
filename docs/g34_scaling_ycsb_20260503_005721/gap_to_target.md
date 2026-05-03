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
