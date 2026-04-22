# g3+g4 YCSB Scaling Sweep — Results Index

**Date**: 2026-04-22 (overnight)
**Sweep**: A/B/C × {a,b,c,d,f} × T={1,2,4,8,16,32,64,86} × cache_{on,off}
           = 240 runs, 0 failures
**Raw log**: [`SUMMARY.log`](SUMMARY.log)

## Quick navigation

### Start here
- [`../morning_summary_20260422.md`](../morning_summary_20260422.md) — one-page digest
- [`analysis.md`](analysis.md) — interpretation of the main sweep
- [`followup_experiments.md`](followup_experiments.md) — 7 proposed next experiments (P1-P7)

### The 84 primary plots

#### Cache-ON (30 throughput + latency plots you asked for, actually 42):

Throughput (15):

| | workloada | workloadb | workloadc | workloadd | workloadf |
|---|---|---|---|---|---|
| A | [thpt](A_thpt_workloada.png) | [thpt](A_thpt_workloadb.png) | [thpt](A_thpt_workloadc.png) | [thpt](A_thpt_workloadd.png) | [thpt](A_thpt_workloadf.png) |
| B | [thpt](B_thpt_workloada.png) | [thpt](B_thpt_workloadb.png) | [thpt](B_thpt_workloadc.png) | [thpt](B_thpt_workloadd.png) | [thpt](B_thpt_workloadf.png) |
| C | [thpt](C_thpt_workloada.png) | [thpt](C_thpt_workloadb.png) | [thpt](C_thpt_workloadc.png) | [thpt](C_thpt_workloadd.png) | [thpt](C_thpt_workloadf.png) |

Write latency (bar: avg/p50/p99) — 12 plots (workloadc has no writes):

| | workloada | workloadb | workloadd | workloadf |
|---|---|---|---|---|
| A | [lat](A_lat_workloada_write.png) | [lat](A_lat_workloadb_write.png) | [lat](A_lat_workloadd_write.png) | [lat](A_lat_workloadf_write.png) |
| B | [lat](B_lat_workloada_write.png) | [lat](B_lat_workloadb_write.png) | [lat](B_lat_workloadd_write.png) | [lat](B_lat_workloadf_write.png) |
| C | [lat](C_lat_workloada_write.png) | [lat](C_lat_workloadb_write.png) | [lat](C_lat_workloadd_write.png) | [lat](C_lat_workloadf_write.png) |

Read latency — 15 plots (all workloads have reads):

| | workloada | workloadb | workloadc | workloadd | workloadf |
|---|---|---|---|---|---|
| A | [lat](A_lat_workloada_read.png) | [lat](A_lat_workloadb_read.png) | [lat](A_lat_workloadc_read.png) | [lat](A_lat_workloadd_read.png) | [lat](A_lat_workloadf_read.png) |
| B | [lat](B_lat_workloada_read.png) | [lat](B_lat_workloadb_read.png) | [lat](B_lat_workloadc_read.png) | [lat](B_lat_workloadd_read.png) | [lat](B_lat_workloadf_read.png) |
| C | [lat](C_lat_workloada_read.png) | [lat](C_lat_workloadb_read.png) | [lat](C_lat_workloadc_read.png) | [lat](C_lat_workloadd_read.png) | [lat](C_lat_workloadf_read.png) |

#### Cache-OFF

Same 42-plot layout under [`cache_off/`](cache_off/). Used when you want
to quantify the DRAM cache's contribution per workload.

### Extra analysis plots

Under [`extra/`](extra/):

- `abc_compare_<wl>.png` — A/B/C overlay, throughput, per workload (5 files)
- `abc_compare_<wl>_lat.png` — A/B/C overlay, write p99 latency (4 files; workloadc has no writes)
- `cache_speedup_{A,B,C}.png` — DRAM cache on/off ratio per opt, all workloads on one plot
- `scaling_efficiency_C.png` — C's thpt / (T × thpt(1)) — ideal would be flat at 100%
- `cross_host_gain.png` — 2-host vs 1-host (P6), 5 subplots
- `summary_table.md` — peak-T table per (opt, wl) cell
- `bucket_sweep_finding.md` — bucket count tested 16k-4M at T=86, not the bottleneck (P1)
- `cross_host_gain_finding.md` — 2-host gain is 1.5-2× at low T, <1× for writes at high T (P6)

### Raw data

- [`SUMMARY.log`](SUMMARY.log) — the 240-row canonical log
- [`extra/g4_solo_scaling.log`](extra/g4_solo_scaling.log) — 40-row solo sweep (P6)
- [`extra/bucket_sweep_T86.log`](extra/bucket_sweep_T86.log) — 10-row bucket sweep (P1)

## Important caveats for reading these numbers

1. **A and B are clamped to 1 worker per host**. PendingRing state is
   per-host, not per-client; intra-host client scaling for A/B needs a
   refactor. Their flat curves across T are expected, not a bug. See
   `analysis.md` for the full reasoning.
2. **C scales with T but not linearly**. On pure-read workloads (c, d)
   C peaks at T=86 (48 M / 44 M ops/s). On write-heavy (a, f) C peaks
   at T=4-8 and then degrades; at T=86 throughput is lower than T=1.
   Root cause is NOT bucket contention (ruled out by P1); it's
   scheduler tail + LFM spin-sleep interaction (see
   `extra/bucket_sweep_finding.md`).
3. **Cross-host gain is workload-dependent**: reads ~1.35-1.4× over
   solo at T=86; write-heavy goes below 1× at T > 32 due to
   cross-host coordination overhead (see
   `extra/cross_host_gain_finding.md`).
4. **One real bug was found during this sweep**: `enable_dram_cache()`
   in A/B had a store-then-allocate race with the replicator thread,
   causing segfault. Commit `9323ac8` fixes it.

## How to reproduce

```bash
# Ship workloads + code (scripts handle PXE wipes)
scripts/rekey_slave.sh g3 && scripts/rekey_slave.sh g4
ssh g3 'chmod 666 /dev/dax0.0' ; ssh g4 'chmod 666 /dev/dax0.0'
scripts/bootstrap_slave.sh g3 && scripts/bootstrap_slave.sh g4

# Main 240-run sweep (~25 min)
bash scripts/run_g34_scaling_sweep.sh

# Plots
latest=$(ls -td logs/g34_scaling_sweep_* | head -1)
python3 docs/plot_scaling_sweep.py "$latest/SUMMARY.log" \
        docs/g34_scaling_ycsb            --cache=on
python3 docs/plot_scaling_sweep.py "$latest/SUMMARY.log" \
        docs/g34_scaling_ycsb/cache_off  --cache=off
python3 docs/plot_scaling_extra.py "$latest/SUMMARY.log" \
        docs/g34_scaling_ycsb/extra
```
