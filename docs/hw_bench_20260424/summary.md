# FUSEE-CXL testbed — hardware performance measurements

Date: 2026-04-24. Measured on g3 (dual-socket 86-core Intel Xeon,
CXL Type-3 via PCIe switch, kernel 6.15, /dev/dax0.0 devdax).
Tool: `tests/bench_cxl_dram.c` (custom MLC-style bench; Intel MLC
binary is no longer freely downloadable).

## Bandwidth vs thread count

All values MB/s unless noted. Region size = 8 GiB per test,
single-host (no cross-host traffic).

### DRAM (local NUMA node 0)

| T  | seq_read | seq_write | rand_read (Mops/s) | rand_read BW | lat_idle (ns) | lat_flush (ns) |
|----|---------:|----------:|-------------------:|-------------:|--------------:|---------------:|
| 1  |   9 820  |   7 257   |     99.7           |   5 640      |   121         |   194          |
| 4  |  32 836  |  29 580   |    356.7           |  22 051      |   129         |   252          |
| 8  |  60 447  |  51 010   |    683.3           |  55 664      |   124         |   217          |
| 16 | 106 081  |  75 058   |  1 374.4           |  84 578      |   129         |   255          |
| 32 | 158 128  | 101 452   |  2 516.0           | 156 506      |   123         |   217          |
| 64 | 217 071  | 182 817   |  4 548.4           | 261 670      |   123         |   207          |
| 86 |**238 881**|**209 114**|**5 489.7**         |**328 121**   |   131         |   246          |

- Saturation: seq_read ≈ **239 GB/s**, seq_write ≈ **209 GB/s**,
  rand_read ≈ **328 GB/s** at T=86.
- Idle latency ≈ **125 ns** (DDR5 random, L3 miss), flat in T.
- clflushopt + mfence + load ≈ **220 ns**, flat in T.

### CXL (/dev/dax0.0 devdax, single-host)

| T  | seq_read | seq_write | rand_read (Mops/s) | rand_read BW | lat_idle (ns) | lat_flush (ns) |
|----|---------:|----------:|-------------------:|-------------:|--------------:|---------------:|
| 1  |   6 625  |   7 873   |     32.2           |   2 055      |   590         | 1 102          |
| 4  |  18 589  |  16 469   |    138.2           |   8 691      |   621         | 1 147          |
| 8  |  21 073  |  14 565   |    261.1           |  16 458      |   588         |   727          |
| 16 |  22 559  |  17 685   |    406.3           |  25 672      |   629         | 1 150          |
| 32 |  23 451  |  21 750   |    427.6           |  26 282      |   606         | 1 150          |
| 64 |  24 003  |  14 770   |    403.0           |  26 109      |   608         | 1 149          |
| 86 |**24 405**|  14 128   |    416.1           |  26 354      |   555         |**1 120**       |

- Saturation: seq_read ≈ **24 GB/s**, seq_write ≈ **22 GB/s**
  @T=32 (regresses at T≥64 under write contention).
- Random read BW saturates at ≈ **26 GB/s / 416 Mops/s** from T=16 onward.
- Idle (warm-cache) random load ≈ **600 ns** flat.
- clflushopt + mfence + load ≈ **1.12 µs** flat — this is the
  **single-host** CXL round-trip cost.

## Cross-host cost (from Phase-1, not re-measured here)

- Peer-written cacheline load (clflushopt + mfence + load after
  peer's store): **p50 ≈ 2.82 µs** (PCIe switch goes to peer host)
- Cross-host delta = **2.82 − 1.12 = 1.70 µs** overhead for
  "observe peer's state" vs "observe own state on CXL".
- LFM uncontended acquire = 1 × peer_scan + 1 × enter_cs = **4.37 µs**.

## DRAM / CXL gap on this platform

| Metric | DRAM (T=86) | CXL (T=32 peak) | ratio |
|--------|------------:|----------------:|------:|
| seq_read BW | 239 GB/s | 24 GB/s | **10.0 ×** |
| seq_write BW | 209 GB/s | 22 GB/s | 9.5 × |
| rand_read BW | 328 GB/s | 26 GB/s | **12.5 ×** |
| rand_load latency (warm) | 125 ns | 600 ns | 4.8 × |
| clflush + load | 220 ns | 1120 ns | **5.1 ×** |

BW gap **widens with T** (CXL saturates near 32 threads while DRAM
keeps scaling). Latency gap is T-invariant.

## Cross-check with FUSEE measurements

| Value | Phase-1 (cross-host) | Bench (single-host) | Delta |
|-------|---------------------:|--------------------:|------:|
| peer_scan / lat_flush p50 | 2820 ns | 1120 ns | +1700 ns (cross-host) |
| Uncontended LFM acquire | 4370 ns | — (not applicable) | — |

The 2 × factor between single-host and cross-host CXL flush-and-load
quantifies the **PCIe switch + peer-side coherence cost**. This is
the hardware floor any software protocol must pay per peer-state
observation.

## Revised throughput ceilings for FUSEE-CXL

### Read-only (workload C)

| Component | Per-op cost | Per-host ceiling | 2-host ceiling |
|-----------|------------:|-----------------:|---------------:|
| Hash lookup, DRAM cache hit (r_p50 in C)  | 1.6 µs | 86 / 1.6 µs = 54 Mops/s | 108 Mops/s |
| Hash lookup, cold, 1 CXL RTT | 1.12 µs + compute | 86 / 2 µs = 43 Mops/s | 86 Mops/s |
| CXL rand_read BW (single host) | — | 416 Mops/s | 832 Mops/s |
| DRAM rand_read BW (single host) | — | 5 490 Mops/s | — (local) |

**Observed C peak 65 Mops/s** (2M @T=64) → we are at **60 % of the
DRAM-cached lookup ceiling**. The gap is thread scheduling / cache
thrash on the hash directory; CXL BW is not the bottleneck.

### Write path — LFM + epoch bump (no batching, iter-2 floor)

- Per op: lock(~8 µs) + scan(1 µs) + publish(1.5 µs) + bump_epoch
  (~3 µs) + unlock = **~14 µs per UPDATE**
- Per-host ceiling 86 / 14 µs = 6.1 Mops/s; 2 hosts = 12 Mops/s
- **Matches iter-2 B peak of 10.5 Mops/s** (pre-batching) ✓

### Write path — with micro-batching (iter-3)

- Writer cost: DRAM ring append ≈ 100–200 ns
- Flusher cost: per drain = 1 × cross-host bump_epoch (~3 µs) +
  N cacheline stores to CXL bucket (~1 µs total if batched)
- **Single-flusher ceiling** ≈ 1 / (3 µs per drain) × avg ops per
  drain. Zipf-hot bucket merge ≈ 40–60 ops/drain →
  **ceiling ≈ 13–20 Mops/s**
- **Matches iter-3 A peak 17 Mops/s (200 k) / 15 Mops/s (2 M)** ✓

### CXL BW is NOT the bottleneck

Worst case (A, 15 Mops/s, 16 B KV + 8 B metadata = 24 B per op
touched on CXL) = 15 × 24 = 360 MB/s. CXL has 22 GB/s writes available.
**60 × headroom**. Bandwidth is idle.

## Targets for breaking the 20 Mops/s bar on A

Ordered by expected ROI:

| Lever | Expected peak | Cost |
|-------|--------------:|------|
| Multi-flusher (N=2, correctly) | 2 × 15 = **30 Mops/s** | fix V2 tail race |
| Per-drain-cycle epoch bump collapse | ~2 × = **30 Mops/s** | small protocol change, no correctness hit |
| Per-host shard (no cross-host bump) | ~3 × = **40–50 Mops/s** | breaks LRC symmetry; needs design |
| CXL 3.0 hardware atomics | ~5 × = **70+ Mops/s** | hardware upgrade |

All software-only options are gated by the **3 µs flusher drain
latency**, which is itself **1 cross-host CXL atomic RTT**. Breaking
that needs either (a) avoiding cross-host atomics (per-host shard)
or (b) amortising better (multi-flusher / per-cycle bump).

## Files

- Raw bench log: `docs/hw_bench_20260424/g3_cxl_dram_sweep.log`
- Plot: `docs/hw_bench_20260424/dram_vs_cxl.png` (3 panels: BW, latency, ratio)
- Plot script: `docs/hw_bench_20260424/plot_hw_bench.py`
- Bench source: `tests/bench_cxl_dram.c`
