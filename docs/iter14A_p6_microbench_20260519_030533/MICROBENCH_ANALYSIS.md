# P6 microbench analysis

**Date**: 2026-05-19
**Phase**: P6
**Data**: 144 cells, 0 FAIL
**Build**: build-cxl-w1 (HAZARD direct-pool read + W1 RESERVED write, defaults OFF for P2 F1 + P4 F2)
**Cell**: 4 scenarios × 2 keyDists × 4 T values × {cold, warm if read; cold if write} × 3 reps
**Workload**: 2M load ops, 1M trans ops per host, KV=1024, NB=1M

## Headline scaling table (trans_agg_thpt, median across 3 reps, cold state)

| Scenario | T=1 | T=4 | T=32 | T=64 |
|---|---:|---:|---:|---:|
| local_read uniform | 0.38M | 1.54M | 7.65M | **12.19M** |
| local_read zipf | 0.79M | 4.02M | 13.49M | **21.47M** |
| xhost_read uniform | 0.38M | 1.53M | 7.49M | **12.11M** |
| xhost_read zipf | 1.10M | 4.00M | 15.26M | **21.37M** |
| local_write uniform | 0.25M | 0.98M | 8.06M | **13.01M** |
| local_write zipf | 0.24M | 0.95M | 6.38M | **6.53M** |
| xhost_write uniform | 0.28M | 0.98M | 8.07M | **12.71M** |
| xhost_write zipf | 0.25M | 0.97M | 6.20M | **6.86M** |

## Key findings

### 1. local_read ≈ xhost_read (within ±1%)

| Cell T=64 cold | local | xhost | diff |
|---|---:|---:|---:|
| uniform | 12.19M | 12.11M | -0.7% |
| zipf | 21.47M | 21.37M | -0.5% |

**Why**: cache_pool is per-host DRAM (mmap MAP_SHARED|MAP_ANONYMOUS = per-host
shared between workers, NOT across CXL). After load phase, each host's local
cache_pool gets populated via forward_read RTT for keys it doesn't own. After
~1000-2000 ops the local cache_pool is fully populated and subsequent reads
all hit R2hit locally (~250 ns) — no further R3 RTT fires.

P4 corroboration: workloadc T=64 cache=on path_decomp had R3 N=89 out of
R1 N=10016 = 0.89% R3 incidence. P6 with NB=1M (no overflow) is similar.

This means **P6 in current form does NOT measure the cross-host slow-path
(R3 RTT)** — it measures the steady-state local cache_pool hit path. P6.5
attempted to force R3 by NB=1024 overflow; see P6.5 analysis.

### 2. zipf >> uniform across all T (TLS L1 win)

| Scenario T=64 cold | uniform | zipf | zipf lift |
|---|---:|---:|---:|
| local_read | 12.19M | 21.47M | **+76%** |
| xhost_read | 12.11M | 21.37M | **+77%** |
| local_write | 13.01M | 6.53M | **-50%** ← write contention reverses |
| xhost_write | 12.71M | 6.86M | **-46%** |

- **Reads**: Zipf concentrates accesses on top-1024 keys → TLS L1 hit-rate
  much higher than uniform → r_p50 drops from ~9.8 µs (uniform) to ~0.6 µs
  (zipf) at T=64. TLS hit fast-path is ~50 ns vs cache_pool ~250 ns.
- **Writes**: Zipf concentrates WRITE contention on top-1024 keys → seqlock-
  CAS retry storm in cache_pool_insert (W10 anomaly P4) → throughput halves.

This is the cleanest evidence that **TLS L1 is providing measurable value**
on reads (it doubles throughput on zipf). For writes, the W10 hot-bucket
problem dominates regardless of TLS layer.

### 3. cold vs warm delta is essentially 0 for reads

| Scenario T=64 | cold vs warm delta |
|---|---:|
| local_read uniform | -0.22% |
| local_read zipf | +0.58% |
| xhost_read uniform | -0.59% |
| xhost_read zipf | -1.90% |

Confirms FUSEE_CACHE=0/1 is a Protocol-A no-op (as documented in microbench
script comment). Cache state in Protocol A is determined by warm-up phase
op flow, not the runtime flag.

### 4. local_write ≈ xhost_write (within ±2%)

| Cell T=64 | local | xhost | diff |
|---|---:|---:|---:|
| uniform | 13.01M | 12.71M | -2.3% |
| zipf | 6.53M | 6.86M | +5.1% |

The "local" vs "xhost" distinction in writes is also weak. Why: in this
workload, every write triggers a cache_pool_insert + InvalRing broadcast
to peer's cache_pool to invalidate. Whether the *trigger* came from the
local worker or via cross-host forward_write makes a small difference
because the dominant cost is the bucket-epoch + cache_pool_insert MESI
ping-pong on both sides.

### 5. Spread anomaly: xhost_write uniform T=64 spread 97%

| Rep | thpt |
|---|---:|
| rep 1 | 13.10M |
| rep 2 | 0.74M |
| rep 3 | 13.10M |

One rep collapsed to 736K. This is the bimodal-collapse phenomenon
re-observed in P6, consistent with P3.C bimodal noise + P5 fast/slow
mode duality. iter-15A backlog.

### 6. Spread anomaly: xhost_write zipf T=32 spread 44%

| Rep | thpt |
|---|---:|
| rep 1 | 6.20M |
| rep 2 | 8.52M |
| rep 3 | 5.80M |

Lesser bimodal at T=32 but still visible.

## Implications

1. **TLS L1 layer works for reads** (zipf +77% over uniform).
2. **cache_pool W10 dominates writes** under hot Zipf (zipf -50% vs uniform).
3. **Cross-host vs local distinction is absorbed by steady-state cache_pool
   warmup** — current NB=1M test is unable to measure the cross-host slow
   path. P6.5 attempted to force overflow but yielded a different surprising
   result (see P6.5 analysis).
4. **Bimodal collapse remains present** across read+write scenarios. iter-12A's
   fix did not eliminate the slow mode. Top iter-15A priority.
5. **Hardware ceiling**: 21.5 Mops/s at T=64 zipf read is the current peak.
   YCSB-A target is 20 Mops/s aggregate; YCSB-C target is 20 Mops/s. P6
   shows microbench-style read-only Zipf at T=64 already reaches the
   YCSB-C target. Full-workload YCSB-A (50% write) is gated by the write
   path which is W10-bound at ~13 Mops/s (uniform) / 6.5 Mops/s (zipf).
