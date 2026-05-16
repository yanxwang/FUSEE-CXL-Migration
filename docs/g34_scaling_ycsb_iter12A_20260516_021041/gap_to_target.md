# iter-11A Phase 6 — gap_to_target.md

Sweep headlines per workload (best cell) and the gap to the 20 Mops/s success bar.

| workload | best Mops/s | gap to 20 (Mops/s) | best cell |
|---|---:|---:|---|
| workloada | 14.628 | 5.372 | T=64 cache=on kv=256 |
| workloadb | 12.240 | 7.760 | T=64 cache=off kv=256 |
| workloadc | 11.762 | 8.238 | T=64 cache=on kv=1024 |
| workloadd | 12.887 | 7.113 | T=64 cache=off kv=256 |
| workloadf | 15.056 | 4.944 | T=64 cache=off kv=512 |

## §13 gate 5 anomaly scan

Total cells: 203; anomalous cells flagged: 15.

| workload | T | cache | kv | thpt Mops/s | reason |
|---|---:|---|---:|---:|---|
| workloada | 4 | on | 1024 | 0.0059 | abs<0.1 (0.0059); nbr/10 (0.0059 < 0.0094) |
| workloada | 8 | on | 1024 | 0.0121 | abs<0.1 (0.0121); nbr/10 (0.0121 < 0.0172) |
| workloada | 32 | off | 1024 | 0.0508 | abs<0.1 (0.0508) |
| workloada | 64 | off | 1024 | 0.0957 | abs<0.1 (0.0957) |
| workloadb | 4 | on | 256 | 0.0536 | abs<0.1 (0.0536); nbr/10 (0.0536 < 0.1746) |
| workloadb | 4 | on | 1024 | 0.0536 | abs<0.1 (0.0536); nbr/10 (0.0536 < 0.1150) |
| workloadb | 2 | off | 256 | 0.0280 | abs<0.1 (0.0280) |
| workloadb | 4 | off | 256 | 0.0520 | abs<0.1 (0.0520) |
| workloadb | 64 | off | 1024 | 0.0016 | abs<0.1 (0.0016); nbr/10 (0.0016 < 0.6450) |
| workloadc | 32 | on | 256 | 0.0008 | abs<0.1 (0.0008); nbr/10 (0.0008 < 0.8169) |
| workloadd | 4 | on | 256 | 0.0590 | abs<0.1 (0.0590); nbr/10 (0.0590 < 0.1731) |
| workloadd | 4 | off | 256 | 0.0590 | abs<0.1 (0.0590) |
| workloadd | 4 | off | 512 | 0.0590 | abs<0.1 (0.0590); nbr/10 (0.0590 < 0.1424) |
| workloadd | 4 | off | 1024 | 0.0589 | abs<0.1 (0.0589); nbr/10 (0.0589 < 0.1303) |
| workloadf | 64 | off | 256 | 0.1243 | nbr/10 (0.1243 < 0.8547) |
