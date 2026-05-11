# workloadc_best per-stage decomposition

**Cell**: workload=workloadc kv=1024 T=64 cache=on
**Sweep headline**: 11.72 Mops/s
**Build**: TLS=1024 + lock-free CAS cache_pool + B0 (worker direct multi-MPSC)

## Healthy capture (try_1)

| Stage | N | p50 µs | p90 µs | p99 µs | max µs | mean µs |
|---|---|---|---|---|---|---|
| W1 | 100000 | 0.766 | 4.305 | 10.974 | 62.763 | 1.602 |
| W2 | 99992 | 0.148 | 0.444 | 2.458 | 131.308 | 0.282 |
| W3 | 99992 | 0.025 | 0.028 | 5.001 | 138.181 | 0.152 |
| W4 | — | — | — | — | — | ✱ no data (workload-specific) |
| W6 | — | — | — | — | — | ✱ no data (workload-specific) |
| W7 | 99992 | 0.046 | 0.049 | 0.067 | 109.988 | 0.100 |
| W8 | 99992 | 0.030 | 0.030 | 5.686 | 857.122 | 0.164 |
| W9 | 99992 | 0.021 | 0.024 | 0.029 | 82.172 | 0.041 |
| W10 | 99992 | 3.436 | 16.189 | 27.986 | 865.051 | 6.144 |
| W12 | 99992 | 0.555 | 5.850 | 14.446 | 15720.234 | 1.885 |
| R1 | 200000 | 6.934 | 10.734 | 17.326 | 275.633 | 5.053 |
| R2hit | 96446 | 0.103 | 0.231 | 0.447 | 166.342 | 0.166 |
| R2miss | 23632 | 2.854 | 5.265 | 8.968 | 132.589 | 3.285 |
| R3 | 414 | 9.328 | 10.502 | 14.630 | 19.771 | 9.463 |
| R4 | 414 | 0.343 | 0.520 | 0.918 | 6.153 | 0.383 |
| R6 | 199460 | 0.468 | 0.748 | 1.053 | 204.808 | 0.584 |
| R0_tls_h | 79922 | 0.030 | 0.038 | 0.050 | 240.916 | 0.074 |
| I1 | — | — | — | — | — | ✱ no data (workload-specific) |
| I2 | — | — | — | — | — | ✱ no data (workload-specific) |
| I3 | — | — | — | — | — | ✱ no data |
| I4 | — | — | — | — | — | ✱ no data (workload-specific) |
| I5 | — | — | — | — | — | ✱ no data (workload-specific) |
| I6 | — | — | — | — | — | ✱ no data (workload-specific) |
| I7 | — | — | — | — | — | ✱ no data (workload-specific) |
| I8 | — | — | — | — | — | ✱ no data (workload-specific) |

## Top-3 mean-cost stages (healthy)

- **R3**: 9.463 µs/op (p99=14.630 µs)
- **W10**: 6.144 µs/op (p99=27.986 µs)
- **R1**: 5.053 µs/op (p99=17.326 µs)