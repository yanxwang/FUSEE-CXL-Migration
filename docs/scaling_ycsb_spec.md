# `scaling_ycsb` — standard benchmark procedure for FUSEE-CXL A/B/C

This document defines the canonical YCSB-scaling experiment used to
validate each phase of the throughput-improvement plan (see
`ABC_throughput_improvement_plan.md`) and to produce the official
comparison figures.

Every re-run of this procedure produces a self-contained directory
under `docs/g34_scaling_ycsb_<timestamp>/` with the raw log, 30 main
plots, a 10-plot `extra/` comparison set, plus this spec.

---

## 1. Testbed

- **2 hosts**: `g3`, `g4`, each 86 CPU cores (Intel Xeon, 1 socket),
  Linux kernel 6.15.0, connected to a shared CXL Type-3 memory server
  via a PCIe switch.
- **Shared CXL device**: `/dev/dax0.0`, 512 GiB, 2 MiB-aligned,
  `target_node=1`, devdax mode. Both hosts mmap the same physical
  bytes (verified by `tests/cxl_xhost_test`).
- **devdax perms**: chmod 666 on both hosts before each run.

## 2. Build

- Branch `feat/cxl-migration` on each slave's `~/FUSEE_CXL/`.
  Re-bootstrap via `scripts/bootstrap_slave.sh {g3,g4}` whenever code
  changes or after a PXE wipe.
- `MAX_HOST_NUM=200` in `cxl_shm_profiling/common.h` (must match on
  both hosts — verify after each re-sync).
- Binaries: `cxl_ycsb_runner_{A,B,C}` under `build-cxl/tests/`.

## 3. Parameters

| Knob | Value | Env var |
|---|---|---|
| Protocols | A, B, C | `OPTS="A B C"` |
| Workloads | a, b, c, d, f (e **skipped**: scan unimplemented) | `WORKLOADS="workloada workloadb workloadc workloadd workloadf"` |
| Clients per host (T) | 1, 2, 4, 8, 16, 32, 64, 86 | `THREADS="1 2 4 8 16 32 64 86"` |
| Cache modes | on, off (**on first, off second**) | `CACHE_MODES="on off"` |
| Bucket count | 65 536 | `NUM_BUCKETS=65536` |
| Ops cap | 200 000 per phase | `MAX_OPS=200000` |
| Hosts | 2 (g3 + g4, role-mode) | `HOST0=g3 HOST1=g4` |
| Per-run timeout | 600 s | `TIMEOUT_S=600` |

**Total runs**: 3 × 5 × 8 × 2 = **240**.

## 4. Client model

Each client is a **fork child process** (not a pthread) with its own
`CxlKvStore` instance and unique LFM slot. Cross-host coordination
through a 2 MiB shared-stats region at the end of the CXL mapping,
ordered by an orchestrator-supplied `FUSEE_RUN_COOKIE` (unique per
run).

- Load phase: host 0's primary client (client_id=0) does all
  `load_ops_v` inserts sequentially. Other clients wait for load_done.
- Trans phase: all `2 × T` clients execute in parallel, each taking
  `trans_ops[i]` where `i mod (2T) == global_id`,
  `global_id = host_id × T + client_id`.

## 5. Reproducibility — single command

```bash
# slaves already bootstrapped and workloads shipped:
bash scripts/run_g34_scaling_sweep.sh
```

Output directory pattern: `logs/g34_scaling_sweep_<yyyymmdd_HHMMSS>/`.
The orchestrator then copies the raw log + regenerated plots into
**`docs/g34_scaling_ycsb_<yyyymmdd_HHMMSS>/`** (see § 6).

Expected wall-clock: 25-40 min for the full 240 runs on an unloaded
pair of g3/g4.

## 6. Output layout

Every run writes to a fresh directory:

```
docs/g34_scaling_ycsb_<timestamp>/
├── SUMMARY.log                    # raw, one line per run
├── plot_commit.txt                # git SHA + date + runner env
├── A_thpt_workload{a,b,c,d,f}.png           # 5 plots — protocol A throughput
├── A_lat_workload{a,b,c,d,f}_{read,write}.png  # up to 10 plots — A latency (read+write split)
├── B_thpt_workload{...}.png                 # 5
├── B_lat_workload{...}_{read,write}.png     # up to 10
├── C_thpt_workload{...}.png                 # 5
├── C_lat_workload{...}_{read,write}.png     # up to 10
├── cache_off/
│   └── same 30+ plot layout for cache-off subset
├── extra/
│   ├── abc_compare_workload{a,b,c,d,f}.png      # 5 — A/B/C throughput overlay, linear Mops/s
│   └── abc_compare_workload{a,b,c,d,f}_lat.png  # up to 5 — A/B/C write-p99 overlay
└── (auxiliary: cache_speedup_*.png, scaling_efficiency_C.png, summary_table.md)
```

(workloadc has only reads → no `_write.png`; other workloads have both
`_read.png` and `_write.png`.)

## 7. Plot conventions

### Y-axis defaults

- **Linear**, not log. Units: **Mops/s**.
- The log variants of the code remain in `docs/plot_scaling_sweep.py`
  and `docs/plot_scaling_extra.py` but **commented-out** under
  `# LOG-Y (commented; uncomment to re-enable):`. To regenerate log
  versions, uncomment those blocks and rerun the plot scripts.

### X-axis

- `#clients per host`, log-scale base 2 (values 1, 2, 4, 8, 16, 32,
  64, 86).

### Throughput plots

- One line, 8 points per plot.
- Y-axis: agg trans throughput, Mops/s (`= trans_agg_thpt / 1e6`).

### Latency plots

- Grouped bars: 3 bars per x value — **avg**, **p50**, **p99**.
- For workloads with both reads and writes: 2 separate plots
  (`<opt>_lat_<wl>_read.png`, `<opt>_lat_<wl>_write.png`).
- Y-axis: per-op latency, **Mops/s-compatible**: strictly we report
  *microseconds per op*, not Mops/s (it's a latency, not a rate). Axis
  label: `latency (μs)`.

### `extra/abc_compare_*.png` — overlay plots

- 5 throughput overlay plots: one subplot per workload, 3 lines (A/B/C).
  Y-axis: Mops/s, linear.
- 5 latency overlay plots: one subplot per workload, 3 lines (A/B/C)
  showing **write p99** vs `#clients`. If a workload has no writes
  (workloadc), plot reads' p99 instead and label accordingly.

## 8. Required raw fields per run (SUMMARY.log format)

Every non-FAIL line has exactly this shape:

```
YCSB opt=<A|B|C> cache=<0|1> num_hosts=<H> threads=<T> threads_eff=<T'>
     load_ops=<N_load> load_thpt=<kops/s>
     trans_ops=<N_trans> trans_wall_max=<seconds> trans_agg_thpt=<kops/s>
     w_avg_ns=<...> w_p50_ns=<...> w_p99_ns=<...>
     r_avg_ns=<...> r_p50_ns=<...> r_p99_ns=<...>
     # <workload>_opt<X>_t<T>_cache<on|off>
```

`threads_eff < threads` indicates the runner clamped (currently A/B
clamp to 1 unless `FUSEE_UNSAFE_UNCLAMP=1` is set). Plotters MUST
ignore `threads_eff` and plot against the requested `threads` value so
curves across phases are aligned.

## 9. Aggregation rule

`trans_agg_thpt = (Σ per-client trans_ops) / max(per-client trans_wall_ns)`.

Latency percentiles are computed per-client then aggregated across
clients via:

- `avg`: total sum / total count.
- `p50`: median of per-client p50 values (across the `2×T` workers).
- `p99`: max of per-client p99 values (conservative; captures tails).

## 10. Run index

After every re-run, append one line to
`docs/scaling_ycsb_runs_index.md`:

```
| <timestamp> | <phase> | <git SHA> | <notes: peak C workloadc / peak A workloada / key change> |
```

So we can trace at a glance which plot came from which code version
and what the sweep was intended to validate.

## 11. Known caveats

- **Short runs (200k ops) underestimate steady-state** at T=86 by
  roughly 40-60 % due to `max_wall / avg_wall` ratio sensitivity to
  OS jitter. The 240-run sweep is the "headline" result; for
  per-phase validation this is adequate. For "final paper-grade"
  numbers on key cells, follow up with a 2M-ops sustained smoke (see
  `plot_smoke_2M.py`).
- **A and B clamp**: currently A/B are runtime-clamped to 1 client per
  host because of the per-host PendingRing (see Phase 1 + 4 of the
  improvement plan). Post-Phase-1 the clamp is conditional on the
  workload having writes; post-Phase-4 it goes away entirely.
- **Workload e (scan) is skipped** until scan is implemented; plots
  silently omit workloade.

## 12. Reference run

As of 2026-04-22 commit `62a4d54`, the baseline scaling_ycsb run lives
at `docs/g34_scaling_ycsb/` (no timestamp — legacy directory name).
From the next run onward the convention is
`docs/g34_scaling_ycsb_<timestamp>/`.

Peak numbers that should be regression-checked:

- C workloadc T=86 cache-on agg: 48.2 Mops/s (200k) / 79.0 Mops/s (2M).
- C workloadd T=86 cache-on agg: 44.0 Mops/s (200k) / 66.4 Mops/s (2M).
- A/B workloadc T=86 cache-on agg: 3.3-3.4 Mops/s (clamped — will
  change after Phase 1).
