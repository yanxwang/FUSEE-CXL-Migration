# iter-11A Phase 6 — gap_to_target.md

Sweep headlines per workload (best cell) and the gap to the 20 Mops/s success bar.

| workload | best Mops/s | gap to 20 (Mops/s) | best cell |
|---|---:|---:|---|
| workloada | 13.691 | 6.309 | T=64 cache=off kv=1024 |
| workloadb | 12.042 | 7.958 | T=64 cache=off kv=512 |
| workloadc | 11.188 | 8.812 | T=64 cache=on kv=512 |
| workloadd | 11.513 | 8.487 | T=64 cache=off kv=256 |
| workloadf | 13.158 | 6.842 | T=64 cache=off kv=256 |

## §13 gate 5 anomaly scan

Total cells: 195; anomalous cells flagged: 38.

| workload | T | cache | kv | thpt Mops/s | reason |
|---|---:|---|---:|---:|---|
| workloada | 2 | on | 256 | 0.0030 | abs<0.1 (0.0030); nbr/10 (0.0030 < 0.0043) |
| workloada | 4 | on | 256 | 0.0059 | abs<0.1 (0.0059) |
| workloada | 8 | on | 256 | 0.0121 | abs<0.1 (0.0121) |
| workloada | 16 | on | 256 | 0.0241 | abs<0.1 (0.0241) |
| workloada | 32 | on | 256 | 0.0013 | abs<0.1 (0.0013); nbr/10 (0.0013 < 0.0048) |
| workloada | 64 | on | 256 | 0.0958 | abs<0.1 (0.0958) |
| workloada | 4 | on | 512 | 0.0059 | abs<0.1 (0.0059); nbr/10 (0.0059 < 0.1395) |
| workloada | 64 | on | 1024 | 0.0958 | abs<0.1 (0.0958); nbr/10 (0.0958 < 0.6990) |
| workloada | 16 | off | 256 | 0.0242 | abs<0.1 (0.0242) |
| workloada | 32 | off | 256 | 0.0014 | abs<0.1 (0.0014) |
| workloada | 64 | off | 256 | 0.0030 | abs<0.1 (0.0030) |
| workloada | 32 | off | 512 | 0.0505 | abs<0.1 (0.0505); nbr/10 (0.0505 < 0.0758) |
| workloada | 64 | off | 512 | 0.0957 | abs<0.1 (0.0957) |
| workloadb | 32 | on | 256 | 0.0008 | abs<0.1 (0.0008); nbr/10 (0.0008 < 0.0536) |
| workloadb | 2 | on | 512 | 0.0279 | abs<0.1 (0.0279); nbr/10 (0.0279 < 0.0810) |
| workloadb | 8 | on | 512 | 0.1296 | nbr/10 (0.1296 < 0.2734) |
| workloadb | 2 | off | 256 | 0.0280 | abs<0.1 (0.0280); nbr/10 (0.0280 < 0.0837) |
| workloadb | 8 | off | 256 | 0.1302 | nbr/10 (0.1302 < 0.2892) |
| workloadb | 16 | off | 1024 | 0.0004 | abs<0.1 (0.0004); nbr/10 (0.0004 < 0.4111) |
| workloadc | 32 | on | 256 | 0.0006 | abs<0.1 (0.0006); nbr/10 (0.0006 < 0.0097) |
| workloadc | 64 | on | 256 | 0.0016 | abs<0.1 (0.0016) |
| workloadd | 2 | on | 256 | 0.0299 | abs<0.1 (0.0299); nbr/10 (0.0299 < 0.0371) |
| workloadd | 2 | on | 512 | 0.0298 | abs<0.1 (0.0298); nbr/10 (0.0298 < 0.0703) |
| workloadd | 8 | on | 512 | 0.1223 | nbr/10 (0.1223 < 0.2654) |
| workloadd | 32 | on | 512 | 0.3882 | nbr/10 (0.3882 < 0.7272) |
| workloadd | 64 | off | 512 | 0.5432 | nbr/10 (0.5432 < 0.6530) |
| workloadd | 64 | off | 1024 | 0.0015 | abs<0.1 (0.0015); nbr/10 (0.0015 < 0.5856) |
| workloadf | 4 | on | 256 | 0.0087 | abs<0.1 (0.0087); nbr/10 (0.0087 < 0.1480) |
| workloadf | 32 | on | 256 | 0.0012 | abs<0.1 (0.0012); nbr/10 (0.0012 < 0.0783) |
| workloadf | 4 | on | 512 | 0.0087 | abs<0.1 (0.0087); nbr/10 (0.0087 < 0.1522) |
| workloadf | 16 | on | 512 | 0.0301 | abs<0.1 (0.0301); nbr/10 (0.0301 < 0.4750) |
| workloadf | 64 | on | 512 | 0.1243 | nbr/10 (0.1243 < 0.7443) |
| workloadf | 32 | on | 1024 | 0.0599 | abs<0.1 (0.0599); nbr/10 (0.0599 < 0.7615) |
| workloadf | 2 | off | 256 | 0.0042 | abs<0.1 (0.0042); nbr/10 (0.0042 < 0.0699) |
| workloadf | 8 | off | 256 | 0.0168 | abs<0.1 (0.0168); nbr/10 (0.0168 < 0.2923) |
| workloadf | 32 | off | 256 | 0.0010 | abs<0.1 (0.0010); nbr/10 (0.0010 < 0.8933) |
| workloadf | 8 | off | 512 | 0.0168 | abs<0.1 (0.0168); nbr/10 (0.0168 < 0.2783) |
| workloadf | 16 | off | 1024 | 0.0006 | abs<0.1 (0.0006); nbr/10 (0.0006 < 0.4197) |
