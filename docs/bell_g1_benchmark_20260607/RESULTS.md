# G1 (2CN+2MN, data=2, idx=1) — Standard Results

Pristine RDMA FUSEE on bell. value=1024B. 3-rep mean. Both YCSB methods (paper / lw).

## Part 1 — Micro latency (1 client, 1 coroutine, sequential sync, µs)

| op | mean | p50 | p99 |
|---|---:|---:|---:|
| search | 5 | 5 | 7 |
| insert | 8 | 8 | 11 |
| update | 10 | 9 | 11 |
| delete | 26 | 12 | 30 |

## Part 2 — Micro throughput (Mops/s)

| clients | insert | search | update | delete |
|---:|---:|---:|---:|---:|
| 2 | 0.402 | 0.928 | 0.308 | 0.295 |
| 4 | 0.629 | 1.695 | 0.469 | 0.523 |
| 8 | 1.141 | 2.745 | 0.896 | 0.948 |
| 16 | 2.330 | 4.656 | 1.736 | 1.802 |
| 28 | 3.461 | 5.476 | 1.943 | 2.591 |

## Part 3 — YCSB throughput (Mops/s, paper / lw)

| workload | 2cl | 4cl | 8cl | 16cl | 28cl |
|---|---:|---:|---:|---:|---:|
| a (50r/50u) | 0.36/0.33 | 0.68/0.67 | 1.32/1.33 | 2.54/2.58 | CRASH/4.36 |
| b (95r/5u) | 0.61/0.61 | 1.05/1.03 | 2.00/2.00 | 3.78/3.88 | 5.77/6.59 |
| c (100r) | 0.75/0.73 | 1.45/1.38 | 2.82/2.68 | 5.50/4.91 | 5.01/5.33 |
| d (95r/5i) | 0.67/0.67 | 1.28/1.25 | 2.54/2.41 | 4.90/4.72 | 5.94/7.66 |
