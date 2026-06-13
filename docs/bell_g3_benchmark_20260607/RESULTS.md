# G3 (1CN+3MN, data=3, idx=3) — Standard Results

Pristine RDMA FUSEE on bell. value=1024B. 3-rep mean. Both YCSB methods (paper / lw).

## Part 1 — Micro latency (1 client, 1 coroutine, sequential sync, µs)

| op | mean | p50 | p99 |
|---|---:|---:|---:|
| search | 10 | 5 | 24 |
| insert | 29 | 14 | 19 |
| update | 32 | 16 | 31 |
| delete | 40 | 20 | 38 |

## Part 2 — Micro throughput (Mops/s)

| clients | insert | search | update | delete |
|---:|---:|---:|---:|---:|
| 2 | 0.103 | 0.493 | 0.091 | 0.080 |
| 4 | 0.257 | 1.110 | 0.226 | 0.108 |
| 8 | 0.566 | 2.013 | 0.461 | 0.241 |
| 14 | 0.940 | 2.716 | 0.684 | 0.303 |

## Part 3 — YCSB throughput (Mops/s, paper / lw)

| workload | 2cl | 4cl | 8cl | 14cl |
|---|---:|---:|---:|---:|
| a (50r/50u) | 0.11/- | 0.26/0.24 | 0.56/- | 0.96/- |
| b (95r/5u) | 0.32/0.04 | 0.72/0.53 | 1.50/- | 2.67/2.58 |
| c (100r) | 0.41/0.04 | 1.03/0.66 | 2.27/1.84 | 2.69/- |
| d (95r/5i) | 0.33/- | 0.86/- | 1.87/- | 3.30/- |

## Notes (G3, 1CN+3MN, idx=3)
- **Headline = ypaper**: 16/16 YCSB cells clean (0 anomalies, rep CV<15%). Single CN = 1
  NIC → read ceiling ~2.7 Mops (c-14cl), ~half of the 2-CN groups' ~5.5M (2 NICs). Scaling
  near-linear to the single-node 14-client cap.
- **lw NON-FUNCTIONAL at idx=3** (38/48 runs FAIL): identical-trace lockstep + 3-replica
  index CAS + single-node co-residence → FAIL_REDO storm + worker crashes; even read-only c
  is erratic. Confirms lw is invalid for idx≥2 (progressively worse: idx=2 broke writes,
  idx=3 breaks ~everything). ypaper is the method of record for replicated-index groups.
- idx=3 vs idx=2/1: write latency & throughput degrade further (3-way index replication,
  log commit), as expected (paper Fig 18). search p50 5µs unchanged (reads don't replicate);
  search/delete show longer tails at idx=3 (p99 24/38µs).
- Diagnostic: at memory_num=3 the client logs IBV_WC_WR_FLUSH_ERR ("polled state 5") =
  downstream of app-level FAIL_REDO (failed ops abandon multi-replica RDMA). Printf
  suppressed (nm.cc:819); confirmed not to skew tpt. tpt=(total-failed)/10 = successful ops/s.
