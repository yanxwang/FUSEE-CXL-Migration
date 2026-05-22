# `microbench_4path` — 4-path Protocol A microbench specification

This is the canonical microbench that characterizes Protocol A's
four fundamental code paths. Established in iter-15A and used in every
subsequent iter that touches the read or write hot path.

**Trigger words**: when the user says "4 microbench", "4-path microbench",
"path microbench", "the standard microbench", "the 4-path study" or any
similar phrase, this is what they mean. Skip to §1 Quick start.

## 0. The 4 paths

| Cell name | Owner relation | Op kind | Path | Bottleneck character |
|---|---|---|---|---|
| `local_read` | own-key | READ | TLS / cache_pool fast lookup, fallback to local CXL pool | Bound by cache_pool memcpy + LLC pressure on KvCacheEntry |
| `xhost_read` | peer-key | READ | `forward_read_direct` → ReadRing → owner's read_handler → response via staging | Bound by single-thread `ReadReceiver` |
| `local_write` | own-key | UPDATE | `execute_write_local` → LFM bucket lock → CoW publish → invalidate broadcast | Bound by LFM bucket lock + invalidate broadcast wait |
| `xhost_write` | peer-key | UPDATE | `forward_write_direct` → WriteRing + ForwardStaging → owner's write_handler → invalidate broadcast | Bound by single-thread `WriteReceiver` |

Iter-15A established the post-fix baseline thpt (V=1024, T=64, cache=10%, zipf-0.99):
- local_read ≈ 47 Mops/s
- xhost_read ≈ 0.62 Mops/s
- local_write ≈ 6.7 Mops/s
- xhost_write ≈ 0.6 Mops/s

`xhost_*` performance ceiling is **single-thread receiver bound** — verified
by Phase 5 (any xhost ops → 20× cluster drop), Phase 6b (no ring tail HITM),
Phase 6c (xhost_* saturates at T=4-8).

## 0. Canonical test cells (default sweep parameters)

For T-sweep / parallelization / RN-study / decomp work, use these
parameters (matches iter-15A Phase 2 so cross-iter comparisons hold):

| Parameter | Value |
|---|---|
| V (KV size) | **1024** for primary T-sweep; full V slice = {8, 256, 512, 1024} |
| Workloads | local_read, xhost_read, local_write, xhost_write |
| Key distribution | zipf-0.99 (primary); uniform + zipf-{0.5,1.5} for distribution slice |
| Num-load (trace gen) | **10000000** |
| Num-trans (trace gen) | **5000000** |
| Cache | enabled (FUSEE_CACHE=1), cache_buckets = **131072** = 524288 cache entries ≈ **10% of per-host key coverage (5M)**, NOT 10% of NUM_BUCKETS |
| NUM_BUCKETS | **8388608** (8M, fixed since iter-15A) |
| T (workers per host) | **{1, 2, 4, 8, 16, 32, 64}** |
| Reps per cell | 3 (median used in plots) |
| `protocol_a_ycsb` trans_ops arg | 5000000 |

Any deviation must be called out in the iter summary. The xhost-only
subset (`xhost_read` + `xhost_write`) is documented separately in
`docs/microbench_xhost_spec.md` with the same defaults.

## 1. Quick start

### Step 1 — Generate traces

```bash
mkdir -p setup/microbench_traces
python3 scripts/iter14A_gen_microbench_traces.py \
  setup/microbench_traces \
  --num-load 10000000 \
  --num-trans 5000000 \
  --num-hosts 2 \
  --keydists "uniform,zipf-0.5,zipf-0.99,zipf-1.5" \
  --mix-fractions "25,50,75"
```

This produces ~88 trace pairs (44 unique scenarios × 2 hosts) totalling
~18GB:
- 4 base scenarios × 4 distributions × 2 hosts = 32 pairs
- 2 mix ops × 3 fractions × 2 hosts = 12 pairs (for Phase 5)

### Step 2 — Rsync to g3 + g4 (tmpfs `/tmp`, NOT `/root`)

`/root` overlay is 4 GB; traces are 18 GB. Use tmpfs:

```bash
# Per-host file filter; trace pair owner is encoded in filename
ssh root@g3 'rm -rf /tmp/microbench_traces; mkdir -p /tmp/microbench_traces; \
  ln -sfn /tmp/microbench_traces /root/FUSEE_CXL/setup/iter15A_microbench_traces'
ssh root@g4 'rm -rf /tmp/microbench_traces; mkdir -p /tmp/microbench_traces; \
  ln -sfn /tmp/microbench_traces /root/FUSEE_CXL/setup/iter15A_microbench_traces'

rsync -av --include='bench_*_h0.spec_load' --include='bench_*_h0.spec_trans' --exclude='*' \
  setup/microbench_traces/ root@g3:/tmp/microbench_traces/ &
rsync -av --include='bench_*_h1.spec_load' --include='bench_*_h1.spec_trans' --exclude='*' \
  setup/microbench_traces/ root@g4:/tmp/microbench_traces/ &
wait
```

Each host gets ~8.8 GB of traces (its own h0 or h1 files).

### Step 3 — Build 4 V-matched binaries on both hosts

```bash
# 4 builds: V=64, 256, 512, 1024 (each with matching FUSEE_CACHE_VALUE_MAX)
for h in g3 g4; do
  for V in 64 256 512 1024; do
    ssh root@$h "
      mkdir -p /tmp/builds/build-cxl-w1-v${V}
      cd /tmp/builds/build-cxl-w1-v${V}
      cmake -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS='-DFUSEE_READ_GUARD=2 -DFUSEE_WRITE_ALLOC=1 -DFUSEE_PATH_COUNTERS=1 -DFUSEE_CACHE_VALUE_MAX=${V}' \
        /root/FUSEE_CXL > /tmp/build-v${V}.log 2>&1
      make -j16 protocol_a_ycsb >> /tmp/build-v${V}.log 2>&1
      ln -sf /tmp/builds/build-cxl-w1-v${V} /root/FUSEE_CXL/build-cxl-w1-v${V}
    " &
  done
done
wait
```

### Step 4 — Run any/all phases

```bash
# Standard 5-phase grid (in order)
bash scripts/iter15A_microbench_run.sh phase0     # path-correctness gate (32 cells × 1 rep, 16 min)
bash scripts/iter15A_microbench_run.sh phase1     # V slice (48 runs, 24 min)
bash scripts/iter15A_microbench_run.sh phase2     # T slice (84 runs, 42 min)
bash scripts/iter15A_microbench_run.sh phase3     # cache% slice (84 runs, 42 min)
bash scripts/iter15A_microbench_run.sh phase4     # distribution slice (48 runs, 24 min)
bash scripts/iter15A_microbench_run.sh phase5     # xhost% slice core (18 runs, 9 min)
bash scripts/iter15A_microbench_run.sh phase5_ext # xhost% slice ext (12 runs, 6 min)
```

Each phase writes to `docs/iter15A_microbench_phase{N}_<ts>/` (the naming
includes `iter15A_` because that's where this lived first — keep it for
continuity OR fork the driver script for new-iter usage).

### Step 5 — Generate plots + summary tables

```bash
# Per-phase line plots (lin + log Y subplots)
for dir in docs/iter15A_microbench_phase{0..5}_*/ docs/iter15A_microbench_phase5_combined_*/ docs/iter15A_microbench_phase5_ext_*/; do
  python3 scripts/iter15A_microbench_plot.py "$dir" 2>/dev/null
done

# Cross-phase summary tables
python3 scripts/iter15A_phase_summary_tables.py
```

## 2. Standard sweep grid (5 phases + optional 6d)

Each cell is **3 reps median** unless otherwise noted. All under
`FUSEE_PATH_COUNTERS=1` for invariant counter audit.

### Phase 0 — Path-correctness gate (32 cells × 1 rep)

V=1024, cache=10%, T ∈ {1, 4, 16, 64}, scenario × keydist (2 × 4 = 8) × T = 32 cells.

**Acceptance gate (HR-2)**: per-cell cluster sum of receiver-side counters:
- `local_read`: `n_r3 == 0` strict
- `xhost_read`: `rh_served ∈ [5M × 0.05, 5M × 1.1]`
- `local_write`: `lw_blk_fwd ≤ 1000` (near-zero)
- `xhost_write`: `lw_blk_fwd ∈ [5M × 0.9, 5M × 1.1]` strict

If any cell fails Phase 0 → STOP and debug. Otherwise proceed.

### Phase 1 — V slice (48 runs)

V ∈ {64, 256, 512, 1024} × 4 scenarios × keydist=zipf-0.99 × T=64 × cache=10% × 3 reps.

Each V uses its matched build (`build-cxl-w1-v$V`).

**Plot**: `phase1_plot_loglin.png` — x=V (log base 2), 4 lines (scenarios), y=thpt Mops; lin + log Y subplots.

**Headline finding (iter-15A baseline)**: local_read non-monotonic across V (peaks at V=256). xhost_* roughly flat across V (receiver-bound, V-insensitive).

### Phase 2 — T slice (84 runs)

T ∈ {1, 2, 4, 8, 16, 32, 64} × 4 scenarios × keydist=zipf-0.99 × V=1024 × cache=10% × 3 reps.

**Plot**: x=T (log base 2), lin + log Y.

**Headline**: local_* scales linearly to T=16 (passes §13 1.5× gate). xhost_* saturates at T=4-8 (single-thread receiver).

### Phase 3 — cache% slice (84 runs)

cache% ∈ {1, 2, 5, 10, 20, 50, 100} × 4 scenarios × keydist=zipf-0.99 × V=1024 × T=64 × 3 reps.

cache% maps to `FUSEE_CACHE_BUCKETS` env via driver lookup:
```
1% → 16K, 2% → 32K, 5% → 64K, 10% → 128K, 20% → 256K, 50% → 512K, 100% → 2M
```

**Plot**: x=cache% (log), lin + log Y.

**Headline**: local_read monotonically DROPS with cache% (LLC pressure, per Phase 6a finding). Writes flat (don't read cache).

### Phase 4 — distribution slice (48 runs)

Distribution ∈ {uniform, zipf-0.5, zipf-0.99, zipf-1.5} × 4 scenarios × V=1024 × T=64 × cache=10% × 3 reps.

**Plot**: x=distribution (categorical), line plot (not bars) with lin + log Y.

**Headline**: local_read peaks at zipf-0.99 (sweet spot); local_write collapses 6× from zipf-0.99 to zipf-1.5 (LFM bucket lock queueing, per Phase 6.0c).

### Phase 5 — xhost% slice (core 18 + ext 12 = 30 runs)

Core: xhost% ∈ {0, 50, 100} × {READ, WRITE} × V=1024, T=64, cache=10%, zipf-0.99 × 3 reps = 18.
Ext: xhost% ∈ {25, 75} × {R, W} × 3 reps = 12.

Mix scenarios (50%, 25%, 75%) need trace gen `--mix-fractions "25,50,75"`.

**Plot**: x=xhost% with labels `100/0`, `75/25`, ..., `0/100` (local/xhost). Lines = {READ, WRITE}. Merge core + ext if both present (combined dir).

**Headline**: even 25% xhost ops → 20× thpt drop (Amdahl-like queueing on receiver).

### Phase 6d (optional) — cache% under uniform (84 runs)

Replicates Phase 3 but with uniform distribution. Use when you want to
disentangle skew-induced effects from cache-size-induced effects.

```bash
bash scripts/iter15A_phase6d_cachepct_uniform.sh
```

## 3. Build flags (required)

```
-DFUSEE_READ_GUARD=2
-DFUSEE_WRITE_ALLOC=1
-DFUSEE_PATH_COUNTERS=1
-DFUSEE_CACHE_VALUE_MAX=$V    # must match the V being tested in that build
```

TLS off by default (post iter-15A 2-tier study; no per-worker cache).

## 4. Hard requirements (auto-enforced by driver)

- **HR-1 (online gate)**: at each (V, T, cache%, keydist) point, `local_X.thpt > xhost_X.thpt`. If violated, driver aborts with non-zero exit.
- **HR-2 (counter gate)**: per Phase 0 + ongoing during all phases. See above.
- **HR-3 (online thpt audit)**: driver prints `local/xhost` ratio after each xhost cell.

All three encoded in `scripts/iter15A_microbench_run.sh::hr1_check()`, `::hr2_check()`.

## 5. Path counters (instrumentation)

Built into `protocol_a_ycsb` under `FUSEE_PATH_COUNTERS=1`. 14 counters dumped per host per phase (after_LOAD + after_TRANS snapshots):

| Counter | Side | Increment site |
|---|---|---|
| `n_tls_hit` | reader worker | TLS L1 lookup success |
| `n_r2hit` | reader worker | cache_pool seqlock hit |
| `n_r2miss_local` | reader worker | cache_pool miss, own key → local CXL pool read |
| `n_r3` | reader worker | cache_pool miss, peer key → `forward_read_direct` |
| `n_cache_pool_insert_from_read` | reader worker | cache_pool_insert after R3 fill |
| `n_local_write_worker` | writer worker | own-key write via `execute_write_local` |
| `n_forward_write` | writer worker | `forward_write_direct` (peer-key write) |
| `n_local_write_with_blk_forwarded` | receiver | W1 RESERVED-path forwarded write committed |
| `n_local_write_staging_forwarded` | receiver | STAGING-path forwarded write committed |
| `n_cache_pool_insert_from_write` | both | cache_pool_insert after a write |
| `n_cache_pool_evict` | both | `cache_pool_evict()` calls (lazy delete) |
| `n_cache_pool_set_stale` | both | invalidate (lazy stale flag) |
| `n_cache_pool_lru_evict` | both | implicit LRU eviction inside `cache_pool_insert` |
| `n_read_handler_served` | receiver | incoming forward_read processed |

Receiver-side counters (`lw_blk_fwd`, `rh_served`) are fork-safe (single
thread in primary). Worker-side counters (`r0_tls`, `r2hit`, `r3`, `fwd_w`,
`local_w_*`) only see primary worker[0]'s share due to thread_local + fork
semantics — they're useful for ratio analysis but NOT for cluster totals.

## 6. Output structure (per phase)

```
docs/<iter>_microbench_phase{N}_<timestamp>/
  grid.csv                       # one row per cell-rep, 30 columns
  hr1_state.tsv                  # tab-sep V T cache% kd scenario thpt for HR-1 audit
  raw/
    <cell_id>_rep<R>_h0.out
    <cell_id>_rep<R>_h0.err
    <cell_id>_rep<R>_h1.out
    <cell_id>_rep<R>_h1.err
  phase{N}_plot_loglin.png       # MANDATORY — lin + log Y subplot line plot
  phase{N}_summary_table.png     # MANDATORY — tabular thpt view (median of reps)
  ANALYSIS.md                    # optional — write when phase has noteworthy findings
```

**MANDATORY post-phase artifact generation** (apply to every phase, no exception):

```bash
python3 scripts/iter15A_microbench_plot.py <phase_out_dir>          # → phaseN_plot_loglin.png
python3 scripts/iter15A_phase_summary_tables.py                      # → phaseN_summary_table.png
```

If a new phase type doesn't have a built-in plotter, add it to both scripts
before declaring the phase complete.

## 7. Required scripts

| Script | Purpose |
|---|---|
| `scripts/iter14A_gen_microbench_traces.py` | Trace generator (4 scenarios + mix variants + θ sweep) |
| `scripts/iter15A_microbench_run.sh` | Phase-by-phase driver with HR-1/HR-2 gates |
| `scripts/iter15A_microbench_plot.py` | Per-phase line plot generator (lin + log Y subplots) |
| `scripts/iter15A_phase_summary_tables.py` | Summary table image generator (matplotlib table) |
| `scripts/iter15A_phase6d_cachepct_uniform.sh` | Optional Phase 6d uniform variant |

## 8. Iter cadence guidance

Run the **full 5-phase grid** when:
- A new optimization changes the read or write critical path
- After a bug fix that could affect baseline numbers
- For paper-grade results

Run a **subset** when:
- Quick smoke-test of a fix → just Phase 0 + Phase 5 (≈ 25 min)
- T-scaling check after a parallelism change → Phase 2 only (≈ 42 min)
- Cache architecture change → Phase 3 + Phase 6d (≈ 84 min)

Run **Phase 0 alone as a regression test** after rebuild — 16 min, catches
most bugs that violate routing invariants.

## 9. Bug history (counters that catch these)

**Bug 1 (iter-15A): `wr_=null` on forked children** — Phase 0's
`lw_blk_fwd` for xhost_write would be far below the [4.5M, 5.5M] gate,
catching this within Phase 0.

**Bug 2 (iter-15A): pool cursor stale across reps** — manifests as
bimodal thpt across reps for the same cell. Caught by Phase 6's CV
audit (`scripts/iter15A_phase_summary_tables.py` could be extended to
flag CV>25% cells, or driver could auto-detect during Phase 1-5).

Both bugs were undetectable in earlier iters because:
- HR-2 gate (`fwd_w > 0`) was too loose pre-iter-15A
- Bimodal results often dismissed as "noise" without quantification

iter-15A's stronger HR-2 (`lw_blk_fwd ∈ ±10%`, `rh_served ∈ [5%, 110%]`)
and CV ≤ 25% audit prevent regression.

## 10. Cross-references

- Plan: `docs/iter15A_microbench_plan/README.md` (the canonical 5-phase plan)
- Iter-15A summary: `docs/iters/iter15A_summary_20260520.md`
- iter-15A results: `docs/iter15A_microbench_phase{0..5,6d}_*/` (with ANALYSIS.md)
- Phase 6 RCA (where lock contention + LLC pressure were proven): `docs/iter15A_phase6{_0,a,b,c}_*/ANALYSIS.md`
- scaling_ycsb spec (the OTHER canonical bench): `docs/scaling_ycsb_spec.md`

## 10.5 Latency decomp (xhost write only)

Stage-level decomposition of the cross-host write path is documented in
the xhost spec under "Latency decomp (iter-16A onwards)":
`docs/microbench_xhost_spec.md` → §"Latency decomp".

The decomp uses PROBE_OP per-thread mmap'd ring (see `src/cxl_probe.h`)
with 15 tags covering 8 canonical stages (5 worker + 3 receiver), 11
derived latencies, and 4 event counters.

To run:
```bash
bash scripts/iter16A_xhost_decomp_run.sh <T> [<trans_ops>] [<rep>]
```

Requires a separate `build-cxl-w1-v1024-probe` build dir on g3/g4 (with
`-DFUSEE_PROBE=1`). Output dir has the analyzed `summary.txt` and
per-op `per_op.csv`. Probe-instrumented runs are 30-50% slower than
probe-off — **do not use the same binary for both decomp and thpt
reporting**.

## 11. Caveats

- LOAD phase is **primary-thread-only**; children only run TRANS. LOAD dominates wallclock (~17 s vs TRANS 0.4-5 s). Means whole-process perf stat can't isolate TRANS.
- thread_local counters from forked children do NOT aggregate into the dump. Worker-side counters undercount; use receiver-side counters for cluster totals.
- Standard cells use zipf-0.99 by default. For paper-grade results, also report uniform + zipf-1.5 to show distribution sensitivity.
- TLS layer is OFF by default since iter-15A 2-tier study; if testing TLS impact, add `FUSEE_DISABLE_TLS=0` to build flags AND re-run all baselines.
