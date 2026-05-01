# `scaling_ycsb` — standard benchmark procedure for FUSEE-CXL Protocol A

This document defines the canonical YCSB-scaling experiment used to
validate each iteration of Protocol A (the directory-based
cache-coherent CXL protocol described in `docs/design_goals.md`
§Protocol A) and to produce the official comparison figures.

Every re-run of this procedure produces a self-contained directory
under `docs/g34_scaling_ycsb_<timestamp>/` with the raw log, the
required plot set (see §6), plus this spec.

**Scope (2026-05-01 onwards)**:
- Only **Protocol A** is swept. Protocol A is and remains "A" — there
  is no `A_v2` rename: the directory-based cache-coherent design
  documented in `design_goals.md §Protocol A (§I-XIII)` *is* Protocol
  A. iter-1A through iter-3A's prior implementations of A are
  considered superseded historical milestones, not separate protocols.
- Protocols **B** (eager push) and **C** (lazy release) are **frozen
  baselines**: their code path remains buildable but is **not** part
  of the standard sweep matrix. Reference numbers from the last
  full A/B/C sweep (`docs/g34_scaling_ycsb/`, 2026-04-22) are
  preserved for historical comparison only and will NOT be regenerated.
- If a future iter requires re-running B or C (e.g., for an apples-to-
  apples publication figure), that is an out-of-band ad-hoc run, not
  the standard sweep — and it must be explicitly justified in the
  iter plan.

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
- Binary: `cxl_ycsb_runner_A` under `build-cxl/tests/`. (B/C runners
  may still build for ad-hoc use but are not invoked by the sweep
  driver.)

## 3. Parameters

| Knob | Value | Env var |
|---|---|---|
| Protocols | A (only) | `OPTS="A"` |
| Workloads | a, b, c, d, f (e **skipped**: scan unimplemented) | `WORKLOADS="workloada workloadb workloadc workloadd workloadf"` |
| Clients per host (T) | 1, 2, 4, 8, 16, 32, 64, 86 | `THREADS="1 2 4 8 16 32 64 86"` |
| Cache modes | on, off (**on first, off second**) | `CACHE_MODES="on off"` |
| Reps per cell | 5 (per spec §IX G2 multi-rep stability) | `REPS=5` |
| Bucket count | 65 536 | `NUM_BUCKETS=65536` |
| Ops cap | 200 000 per phase | `MAX_OPS=200000` |
| Hosts | 2 (g3 + g4, role-mode) | `HOST0=g3 HOST1=g4` |
| Per-run timeout | 600 s | `TIMEOUT_S=600` |

**Total runs**: 1 × 5 × 8 × 2 × 5 = **400** cell-runs (80 unique cells
× 5 reps). Headline numbers are 5-rep medians.

A sweep that reports fewer than 80 unique cells (or fewer than 5 reps
per cell) is **not a valid scaling_ycsb run** and MUST NOT be cited
as iter-completion evidence. See iter-4A for the cautionary case
(only 4 cells × 1 rep reported, violating both the cell-count and
multi-rep gates; flagged as iter-execution-discipline violation per
`CLAUDE.md`).

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

Expected wall-clock: 60-90 min for the full 80-cell × 5-rep matrix
on an unloaded pair of g3/g4.

## 6. Output layout

Every run writes to a fresh directory:

```
docs/g34_scaling_ycsb_<timestamp>/
├── SUMMARY.log                    # raw, one line per (cell, rep)
├── plot_commit.txt                # git SHA + date + runner env
├── A_thpt_workload{a,b,c,d,f}.png           # 5 plots — protocol A throughput, 5-rep median
├── A_thpt_workload{a,b,c,d,f}_band.png      # 5 plots — same with min/max shaded band
├── A_lat_workload{a,b,c,d,f}_{read,write}.png  # up to 10 plots — A latency (read+write split)
├── cache_off/
│   └── same plot layout for cache-off subset
├── extra/
│   ├── A_target_workload{a,b,c,d,f}.png    # 5 — A throughput vs 20 Mops/s target line
│   └── A_scaling_efficiency.png            # peak T thpt / single-T thpt across workloads
└── (auxiliary: cache_speedup_A_*.png, summary_table.md, gap_to_target.md)
```

(workloadc has only reads → no `_write.png`; other workloads have both
`_read.png` and `_write.png`.)

The historical comparison directory `docs/g34_scaling_ycsb/`
(2026-04-22, last full A/B/C run) is preserved as-is for context;
new sweeps do NOT regenerate B/C bars.

## 7. Plot conventions

### Visual style — MANDATORY

All plots produced from a `scaling_ycsb` sweep MUST use the project
plotting standard defined in
[`docs/tools/plot_style.py`](tools/plot_style.py) (Style B —
greyscale + accent red `#c44e52`, with `axes.titlepad=10` and
y-headroom 25 % above bar tops).

Every new `plot_*.py` for a sweep starts with:

```python
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from plot_style import apply_style, COLORS, bar_with_headroom
apply_style()
```

and uses `COLORS["kv256"]` / `COLORS["N2"]` / etc. instead of
hard-coded hex. To change the project visual style, edit
`docs/tools/plot_style.py` once — do NOT modify per-script.

Reference figure (the look every scaling_ycsb plot should match):
[`docs/iter5_kv_n_compare/iter5_bestn_thpt_bars.png`](iter5_kv_n_compare/iter5_bestn_thpt_bars.png).

### Y-axis defaults

- **Linear**, not log. Units: **Mops/s**.
- The log variants of the code remain in `docs/tools/plot_scaling_sweep.py`
  and `docs/tools/plot_scaling_extra.py` but **commented-out** under
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

### `extra/A_target_*.png` — gap-to-target plots

- 5 throughput plots, one per workload.
- Each plot shows Protocol A's 5-rep median throughput line plus a
  horizontal red dashed line at **20 Mops/s** (the design target per
  `docs/design_goals.md`).
- The gap (target − peak observed) is annotated at the rightmost
  data point. This makes the "distance to ceiling" visually
  unmissable, replacing the old A/B/C overlay (which is no longer
  meaningful since B/C are frozen).

### `extra/A_scaling_efficiency.png`

- Per workload, plot `peak_T_throughput / single_T_throughput` as a
  bar. Identifies which workloads scale and which saturate early.

## 8. Required raw fields per run (SUMMARY.log format)

Every non-FAIL line has exactly this shape:

```
YCSB opt=A cache=<0|1> num_hosts=<H> threads=<T> threads_eff=<T'> rep=<r>
     load_ops=<N_load> load_thpt=<kops/s>
     trans_ops=<N_trans> trans_wall_max=<seconds> trans_agg_thpt=<kops/s>
     w_avg_ns=<...> w_p50_ns=<...> w_p99_ns=<...>
     r_avg_ns=<...> r_p50_ns=<...> r_p99_ns=<...>
     # <workload>_optA_t<T>_cache<on|off>_rep<r>
```

`rep` ∈ {1..5} identifies which repetition of the cell this line is.

`threads_eff < threads` indicates the runner clamped (legacy from
A/B's old per-host PendingRing path). For Protocol A as defined in
the current `design_goals.md` spec, no clamp is expected; plotters
MUST still ignore `threads_eff` and plot against the requested
`threads` value so curves across iters are aligned.

## 9. Aggregation rule

Within a single rep:
`trans_agg_thpt = (Σ per-client trans_ops) / max(per-client trans_wall_ns)`.

Latency percentiles within a single rep are computed per-client then
aggregated across clients via:

- `avg`: total sum / total count.
- `p50`: median of per-client p50 values (across the `2×T` workers).
- `p99`: max of per-client p99 values (conservative; captures tails).

Across reps for the same cell (5 reps):

- Headline throughput: **median of 5**. Min/max shown as a band on
  `*_band.png` plots.
- Headline latency p50/p99: median of 5.
- A cell where `(max − min) / median > 0.20` is flagged in
  `summary_table.md` as **unstable**; user is asked whether to
  re-run that cell with more reps.

## 10. Run index

After every re-run, append one line to
`docs/scaling_ycsb_runs_index.md`:

```
| <timestamp> | <iter> | <git SHA> | <notes: peak A workloada / peak A workloadc / gap to 20 Mops/s target / key change> |
```

So we can trace at a glance which plot came from which code version
and what the sweep was intended to validate.

## 11. Known caveats

- **Short runs (200k ops) underestimate steady-state** at T=86 by
  roughly 40-60 % due to `max_wall / avg_wall` ratio sensitivity to
  OS jitter. The 80-cell × 5-rep sweep is the "headline" result; for
  "final paper-grade" numbers on key cells, follow up with a 2M-ops
  sustained smoke (see `plot_smoke_2M.py`).
- **Workload e (scan) is skipped** until scan is implemented; plots
  silently omit workloade.

## 12. Reference runs

| Date | Iter | Directory | Notes |
|------|------|-----------|-------|
| 2026-04-22 | iter-3A (C-focused) | `docs/g34_scaling_ycsb/` | Last full A/B/C sweep. Frozen baseline. C workloadc T=86 cache-on = 48.2 Mops/s (200k) / 79.0 Mops/s (2M); C workloadd T=86 cache-on = 44.0 / 66.4 Mops/s. A/B numbers from this run reflect superseded protocol implementations and are NOT valid baselines for the current Protocol A. |

Reference targets (per `docs/design_goals.md`):

- **YCSB-A (R50/W50 Zipf) ≥ 20 Mops/s aggregate** — primary target
- **YCSB-C (100% read) ≥ 20 Mops/s aggregate** — primary target

Every Protocol A sweep MUST report the gap to these two targets in
`gap_to_target.md` (auto-generated from SUMMARY.log).

## 13. Iter-completion gate

A protocol-A iter cannot be marked COMPLETE in its summary doc unless:

1. A `docs/g34_scaling_ycsb_<timestamp>/` directory exists with
   ≥ 80 unique cells × 5 reps (= 400 SUMMARY.log lines, ignoring
   FAILs and reruns).
2. `gap_to_target.md` is generated and present.
3. The iter summary doc cites the `<timestamp>` of that directory.

Iters that report fewer cells (e.g., iter-4A's 4-cell × 1-rep
preview) violate this gate and must catch up before the next iter
starts.
