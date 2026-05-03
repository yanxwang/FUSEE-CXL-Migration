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
| Clients per host (T) | 1, 2, 4, 8, 16, 32, 64 | `THREADS="1 2 4 8 16 32 64"` |
| Cache modes | on, off (**on first, off second**) | `CACHE_MODES="on off"` |
| KV value sizes | 256, 512, 1024 B (per Protocol A blockpool) | `KV_SIZES="256 512 1024"` |
| Reps per cell | **1** (default; specific experiments may override) | `REPS=1` |
| Bucket count | 65 536 | `NUM_BUCKETS=65536` |
| Ops cap | 200 000 per phase | `MAX_OPS=200000` |
| Hosts | 2 (g3 + g4, role-mode) | `HOST0=g3 HOST1=g4` |
| Per-run timeout | 600 s | `TIMEOUT_S=600` |

**Total runs**: 1 × 5 × 7 × 2 × 3 × 1 = **210** cell-runs (210 unique
cells × 1 rep). Headline numbers are single-rep observations.

**T grid rationale (set 2026-05-02 post-iter-5A)**: each host has
86 CPU cores. T_max=64 reserves 22 cores per host for
non-worker system threads — currently 1 forward responder + 1
inval cache_dispatcher (= 2); future K-shard dispatchers, K-shard
responders, sender/receiver pools, GC threads. The 22-core budget
prevents the bi-modal "T=86 collapses while T=64 hits 17.9 Mops/s"
artifact iter-5A surfaced (dispatcher CPU starvation when worker
count == core count). A separate **system-thread scaling experiment**
(varying K-shard count, dispatcher count, etc.) uses this 22-core
budget; results documented per-experiment outside scaling_ycsb spec.

**Reps policy (set 2026-05-02)**: standard sweep is 1 rep per cell.
Multi-rep (REPS=N, N>1) is reserved for **specific experiments
that explicitly require it**: e.g., investigating noise on a
suspected unstable cell, validating a stability fix, or producing
a paper-grade headline number. When a multi-rep run happens, the
iter summary doc / experiment log MUST state the rep count + why
multi-rep was needed. Default sweeps that report 1-rep numbers
are valid; the spec §IX G2 "multi-rep stability" gate is
NOT enforced at iter-completion unless the iter explicitly opts in.

KV-size dimension was added 2026-05-02 when Protocol A's blockpool
integration moved from Phase 4 (API-only) to Phase 6 (full CoW
write path). Prior to that date, slots stored an inline 8 B value;
post Protocol A, slots store a CXL block pointer + size_class and
the actual value lives in a per-host blockpool segment.

A sweep that reports fewer than 210 unique cells is **not a valid
scaling_ycsb run** and MUST NOT be cited as iter-completion
evidence. See iter-4A first attempt for the cautionary case (only
4 cells × 1 rep reported, violating cell-count AND kv-size gates;
flagged as iter-execution-discipline violation per `CLAUDE.md`).
Cell-count remains the primary completion criterion; rep-count is
a per-experiment knob.

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

Expected wall-clock: ~50-80 min for the full 210-cell single-rep
matrix (70 base cells × 3 KV sizes × 1 rep = 210 SUMMARY.log lines)
on an unloaded pair of g3/g4. Multi-rep experiments scale linearly.

## 6. Output layout

Every run writes to a fresh directory:

```
docs/g34_scaling_ycsb_<timestamp>/
├── SUMMARY.log                    # raw, one line per (cell, rep)
├── plot_commit.txt                # git SHA + date + runner env
├── A_thpt_workload{a,b,c,d,f}_kv{256,512,1024}.png   # 15 plots — protocol A throughput per (workload, KV size)
├── A_lat_workload{a,b,c,d,f}_kv*_{read,write}.png    # up to 30 plots — A latency per (workload, KV size, op kind)
├── cache_off/
│   └── same plot layout for cache-off subset
├── extra/
│   ├── A_target_workload{a,b,c,d,f}_kv*.png   # 15 — A throughput vs 20 Mops/s target, per (workload, KV size)
│   ├── A_kv_size_compare_workload*.png        # 5 — KV-size scaling overlay per workload
│   └── A_scaling_efficiency.png               # peak T thpt / single-T thpt across workloads
└── (auxiliary: cache_speedup_A_*.png, summary_table.md, gap_to_target.md)
```

When `REPS > 1` is explicitly opted-in, additionally produce:

```
├── A_thpt_workload*_kv*_band.png   # 15 plots — same as throughput plots with min/max shaded band
```

(`_band.png` only meaningful when REPS > 1; single-rep runs omit
this variant.)

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
  64).

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
- Each plot shows Protocol A's throughput line (single-rep
  observations OR multi-rep median, whichever the run used) plus a
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
YCSB opt=A cache=<0|1> num_hosts=<H> threads=<T> threads_eff=<T'> kv_size=<256|512|1024> rep=<r>
     load_ops=<N_load> load_thpt=<kops/s>
     trans_ops=<N_trans> trans_wall_max=<seconds> trans_agg_thpt=<kops/s>
     w_avg_ns=<...> w_p50_ns=<...> w_p99_ns=<...>
     r_avg_ns=<...> r_p50_ns=<...> r_p99_ns=<...>
     # <workload>_optA_t<T>_cache<on|off>_kv<256|512|1024>_rep<r>
```

`rep` is the 1-based repetition index of the cell this line is.
For default sweeps `rep=1` always; multi-rep experiments produce
N lines per cell (`rep=1..N`).
`kv_size` ∈ {256, 512, 1024} identifies the value-byte size class
the runner used for this cell.

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

Across reps for the same cell (only applies when REPS > 1; default
single-rep sweeps skip this section):

- Headline throughput: **median of N**. Min/max shown as a band on
  `*_band.png` plots.
- Headline latency p50/p99: median of N.
- A cell where `(max − min) / median > 0.20` is flagged in
  `summary_table.md` as **unstable**; user is asked whether to
  re-run that cell with more reps.

For single-rep sweeps (REPS=1, default), each cell's headline
throughput / latency is the single observed value. No min/max
bands; no unstable-cell flag (need ≥ 2 reps to compute spread).
Single-rep numbers should be treated as **indicative**, not
steady-state — when a number drives a major decision (e.g. paper
headline, "did the optimization work" verdict), the relevant
cells should be re-run with REPS = 5+ explicitly.

## 10. Run index

After every re-run, append one line to
`docs/scaling_ycsb_runs_index.md`:

```
| <timestamp> | <iter> | <git SHA> | <notes: peak A workloada / peak A workloadc / gap to 20 Mops/s target / key change> |
```

So we can trace at a glance which plot came from which code version
and what the sweep was intended to validate.

## 11. Known caveats

- **Short runs (200k ops) underestimate steady-state** at T=64 by
  roughly 30-50 % due to `max_wall / avg_wall` ratio sensitivity to
  OS jitter. The 210-cell single-rep matrix is the standard result;
  for "final paper-grade" numbers on key cells, follow up with a
  2M-ops sustained smoke (see `plot_smoke_2M.py`) and/or a
  multi-rep experiment (REPS=5+).
- **Single-rep is the default** (set 2026-05-02). Multi-rep
  experiments are explicit opt-ins, not the headline cadence. A
  single-rep number that swings between runs is normal — multi-rep
  is the tool to investigate or commit to a stable number.
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
   ≥ 210 unique cells (cell-count is the headline gate; reps are
   per-experiment so a default 1-rep sweep produces 210
   SUMMARY.log lines, while an opt-in 5-rep sweep produces 1050).
   FAILs and reruns excluded.
2. `gap_to_target.md` is generated and present.
3. The iter summary doc cites the `<timestamp>` of that directory.
4. **G6 (concurrent rw race test) PASS**: `tests/protocol_a_rw_race_test`
   run on the commit referenced in `plot_commit.txt` reports
   `violations=0`. This is the §I9 strict-A linearizability gate.
   iter-4A "Phase 8 hash-diff PASS but OP_CACHE_REGISTER never wired"
   was undetectable from hash-diff alone; G6 closes that hole.
   First enforced from iter-5A onwards.

Iters that report fewer cells (e.g., iter-4A's 4-cell × 1-rep
preview) violate this gate and must catch up before the next iter
starts.
