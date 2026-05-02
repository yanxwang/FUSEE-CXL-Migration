# Gap to 20 Mops/s target (per `docs/design_goals.md`)

Headline: peak Mops/s per workload (cache=on, all T values).

| Workload | Peak Mops/s | At T= | Gap to 20 Mops/s |
|---|---|---|---|
| workloada | 16.62 | 86 | +3.38 (83.1% of target) |
| workloadb | 12.34 | 86 | +7.66 (61.7% of target) |
| workloadc | 11.66 | 64 | +8.34 (58.3% of target) |
| workloadd | 12.33 | 86 | +7.67 (61.7% of target) |
| workloadf | 14.74 | 86 | +5.26 (73.7% of target) |

## All cells (cache=on)

| Workload | T | Mops/s | %% of 20 Mops/s |
|---|---|---|---|
| workloada | 1 | 0.513 | 2.6% |
| workloada | 2 | 1.035 | 5.2% |
| workloada | 4 | 1.784 | 8.9% |
| workloada | 8 | 3.545 | 17.7% |
| workloada | 16 | 6.217 | 31.1% |
| workloada | 32 | 9.393 | 47.0% |
| workloada | 64 | 15.509 | 77.5% |
| workloada | 86 | 16.622 | 83.1% |
| workloadb | 1 | 0.666 | 3.3% |
| workloadb | 2 | 1.336 | 6.7% |
| workloadb | 4 | 2.341 | 11.7% |
| workloadb | 8 | 3.840 | 19.2% |
| workloadb | 16 | 5.364 | 26.8% |
| workloadb | 32 | 6.744 | 33.7% |
| workloadb | 64 | 12.025 | 60.1% |
| workloadb | 86 | 12.340 | 61.7% |
| workloadc | 1 | 0.692 | 3.5% |
| workloadc | 2 | 1.404 | 7.0% |
| workloadc | 4 | 2.420 | 12.1% |
| workloadc | 8 | 3.834 | 19.2% |
| workloadc | 16 | 5.334 | 26.7% |
| workloadc | 32 | 7.831 | 39.2% |
| workloadc | 64 | 11.658 | 58.3% |
| workloadc | 86 | 0.006 | 0.0% |
| workloadd | 1 | 0.511 | 2.6% |
| workloadd | 2 | 1.009 | 5.0% |
| workloadd | 4 | 1.892 | 9.5% |
| workloadd | 8 | 3.414 | 17.1% |
| workloadd | 16 | 4.889 | 24.4% |
| workloadd | 32 | 6.621 | 33.1% |
| workloadd | 64 | 11.361 | 56.8% |
| workloadd | 86 | 12.330 | 61.7% |
| workloadf | 1 | 0.552 | 2.8% |
| workloadf | 2 | 1.095 | 5.5% |
| workloadf | 4 | 2.119 | 10.6% |
| workloadf | 8 | 3.744 | 18.7% |
| workloadf | 16 | 5.858 | 29.3% |
| workloadf | 32 | 8.116 | 40.6% |
| workloadf | 64 | 13.924 | 69.6% |
| workloadf | 86 | 14.736 | 73.7% |
