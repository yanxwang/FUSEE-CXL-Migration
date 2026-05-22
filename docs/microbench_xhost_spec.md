# `microbench_xhost` — xhost-only path microbench spec

Focused subset of the 4-path microbench. Run frequently during iter-16A+
work on the cross-host write/read path (multi-receiver, ring optimization,
etc.) to verify changes don't regress xhost and to compare against baseline.

**Trigger words**: when user says "xhost microbench", "xhost study",
"xhost test", "xhost path bench", run this spec.

**Parent spec**: `docs/microbench_4path_spec.md` — full 4-path version with
all 5 phases. This xhost-only spec uses the same infrastructure but
restricts to the 2 cross-host scenarios (`xhost_read`, `xhost_write`).

## Why a separate spec

xhost_read and xhost_write are the **performance ceilings** for any
realistic mixed RW workload (per iter-15A Phase 5: even 25% xhost ops drops
cluster thpt 20×). All optimization work that touches the cross-host
path needs to verify against these baselines. Running just these 2 cells is
~10× faster than the full 4-path; makes regression testing cheap.

## Established baseline (iter-15A, post-fix)

V=1024, T=64, cache=10%, zipf-0.99:

| Cell | thpt (Mops/s) | p50 (µs) | p99 (µs) |
|---|---:|---:|---:|
| xhost_read | 0.623 | 261.99 | 303.75 |
| xhost_write | 0.604 | 213.40 | 255.65 |

The 0.6 Mops ceiling: original iter-15A hypothesis was "single-thread
receiver bound". iter-16A RN study (2026-05-21) refined this: architecture
is N workers → MPSC WriteRing (CXL) → single receiver; the 3 sender
threads exist but are idle (`aggr_==nullptr`). L3 (receiver-strip-to-ack)
ceiling = 1.52 Mops/s/host at T≥16. The 1.52 floor is bound by ring +
tail-cacheline contention + ack RTT, not by receiver work. Receiver
work itself adds ~150% on top of the L3 floor (= L0 0.60 vs L3 1.52).

**No quantitative target is fixed for iter-16A** — the optimization
direction is set by the RN findings (parallelize CXL slot publish +
reduce ring contention), but the achievable speedup will be measured,
not pre-declared.

## Canonical test cells (default sweep)

For any T-sweep / parallelization study, use these exact parameters
(matches iter-15A Phase 2 baseline so cross-iter comparisons are valid):

| Parameter | Value |
|---|---|
| V (KV size) | **1024** |
| Workload | xhost_write, xhost_read |
| Key distribution | zipf-0.99 |
| Num-load (trace gen) | **10000000** |
| Num-trans (trace gen) | **5000000** |
| Cache | enabled (FUSEE_CACHE=1), cache_buckets = **131072** = 524288 cache entries ≈ **10% of per-host key coverage (5M)**, NOT 10% of NUM_BUCKETS |
| NUM_BUCKETS | **8388608** (8M, fixed since iter-15A) |
| T (workers per host) | **{1, 2, 4, 8, 16, 32, 64}** |
| Reps per cell | 3 (median used in plots) |
| `protocol_a_ycsb` trans_ops arg | 5000000 |

Any deviation from these defaults must be called out in the iter
summary. Cells using these defaults can be compared apples-to-apples
with iter-15A Phase 2 grid.csv and the iter-16A RN study baseline (L0
column).

## Quick start

### Step 1 — Trace gen (one-time, ~3 min)

Same as 4-path. If traces already exist at `setup/microbench_traces/`, skip.
Otherwise:

```bash
python3 scripts/iter14A_gen_microbench_traces.py \
  setup/microbench_traces \
  --num-load 10000000 --num-trans 5000000 --num-hosts 2 \
  --keydists "zipf-0.99"  # or add more for distribution sweep
```

### Step 2 — Host transfer (one-time, ~3 min)

Same as 4-path. xhost-only would use only `bench_xhost_*` files but for
simplicity copy all (they're all on tmpfs anyway).

### Step 3 — Build (one-time per code change)

Per 4-path: 4 V-matched builds with `FUSEE_PATH_COUNTERS=1`. For
xhost-only smoke, only V=1024 build is needed (most iter-16A
optimization targets V=1024 baseline).

### Step 4 — Run

Choose cadence:

#### A. Quick smoke (4 min, 6 runs) — for any code change

T=64, V=1024, cache=10%, zipf-0.99, both scenarios, 3 reps each = 6 runs.

```bash
bash scripts/iter15A_microbench_run.sh phase5  # core xhost runs include 0/50/100% cells
# Or invoke a dedicated 1-cell driver — runs are identical to phase5 0%/100% cells
```

Pass gate: `xhost_*.thpt > baseline.thpt × 0.95` (no >5% regression).

#### B. T scaling check (15 min, 42 runs) — for parallelization changes

T ∈ {1,2,4,8,16,32,64} × 2 scenarios × 3 reps = 42 runs. Re-use
Phase 2 framework but filter to xhost only.

#### C. Full xhost characterization (90 min, ~180 runs)

V slice (24 runs) + T slice (42 runs) + cache% slice (42 runs) +
distribution slice (24 runs) + receiver-NOOP study (72 runs).

#### D. Receiver-NOOP study (iter-16A onwards, 30 min, 72 runs)

4 noop levels × T ∈ {1, 8, 64} × 2 scenarios × 3 reps = 72 runs.

Env var: `FUSEE_RECEIVER_NOOP_LEVEL` (0–3).

| Level | Receiver work | Tests |
|---:|---|---|
| 0 | Full (= xhost_*) | Real baseline |
| 1 | Skip step 7 (invalidate broadcast)  | Cost of invalidate broadcast wait |
| 2 | Only step 4 (CXL slot publish) + step 8 (ack) | Cost of bucket/directory/inval combined |
| 3 | Only step 8 (ack) | Pure ring pipeline ceiling |

Interpretation:
- (L3 thpt) = ring + minimal ack ceiling. Sets upper bound for multi-receiver.
- (L0 / L3) ratio = "what fraction of receiver time goes to data-plane work"
- (L1 / L0) = "speedup if we eliminated invalidate broadcast"
- (L2 / L1) = "speedup if we eliminated bucket/directory work"
- (L3 / L2) = "speedup if we eliminated CXL publish"

## Output structure

`docs/<iter>_microbench_xhost_<timestamp>/`:
- `grid.csv` — same schema as 4-path
- `raw/` — per-cell stdout/stderr
- `xhost_*_plot_loglin.png` — **MANDATORY** lin + log Y line plot
- `xhost_*_summary_table.png` — **MANDATORY** tabular thpt view
- `ANALYSIS.md` — required for any non-trivial result

**MANDATORY post-run artifact generation** — same pattern as 4-path:
both a summary table PNG AND a loglin line plot must be generated for
every cell set produced. If running a new sub-study (e.g. NOOP levels),
add an appropriate plotter to `scripts/iter15A_microbench_plot.py` and
table generator to `scripts/iter15A_phase_summary_tables.py` before
declaring the run complete.

## Receiver-NOOP code change (iter-16A code work)

Add env `FUSEE_RECEIVER_NOOP_LEVEL` parsed at startup in
`CxlKvStoreA::enable_*_ring` or pulled as static thread_local. Branch
inside:

- `write_handler()` ([src/cxl_kv_ops_A.cc:2104](src/cxl_kv_ops_A.cc#L2104)) — write-side
- `read_handler()` ([src/cxl_kv_ops_A.cc:2167](src/cxl_kv_ops_A.cc#L2167)) — read-side

Level-3 early-return at function entry: write `e->status=0`, set
`e->resp_op_id.store(req_op_id, release)`, flush_line + sfence, return.

Level-2: do the CXL slot publish (write path) or directory lookup (read
path) but no further work.

Level-1: full path except invalidate broadcast loop in write case
(write only; read path has nothing analogous, so L1 = L0 for reads).

~30-50 LOC change total. Verify with Phase 0 path gate first — counter
expectations DIFFER for L > 0 (lw_blk_fwd etc. drop), so phase-0 gate
needs to be conditional on `FUSEE_RECEIVER_NOOP_LEVEL == 0`.

## Hard requirements (xhost-only)

- **HR-1 (regression)**: any code change must keep
  `xhost_X.thpt > baseline.thpt × 0.95` (no >5% regression at any T).
- **HR-2 (counter gate)**: same as 4-path, but only `xhost_*` rows checked.
- **HR-NOOP (iter-16A)**: `L3 thpt > L2 > L1 > L0` (monotonic) — sanity
  check that progressive stripping always speeds up. Violation = code bug
  in the noop branching.

## Iter cadence

- **After every iter-16A commit**: Run A (quick smoke, 4 min). Verify no
  regression vs baseline.
- **After major arch change**: Run B (T scaling, 15 min).
- **At end of iter**: Run C (full characterization, 90 min) + write
  ANALYSIS.md comparing to iter-15A baseline.
- **First iter-16A experiment**: Run D (NOOP levels) to find which step
  is the biggest cost. Determines whether multi-receiver is the right fix.

## Latency decomp (iter-16A onwards)

Stage decomposition of the cross-host write path via per-thread PROBE_OP
trace files. Lets you see WHERE time is spent at sub-stage granularity.

### Stage definition

| # | Stage canonical | Code phase | Side | What it measures |
|---:|---|---|---|---|
| 1 | slot_reserve | B (worker B3 fetch_add + flush) | worker | CXL atomic on ring tail |
| 2 | slot_wait | C (worker spin on req_op_id==0) | worker | wait for prior slot owner to ack |
| 3 | value_xfer | D (worker writes value to CXL pool) | worker | **CPU-side time only**, see caveat |
| 4 | ctrl_publish | E (worker fills entry + publishes req_op_id) | worker | single CXL cacheline write+flush |
| 5 | ack_wait | F (worker spin on resp_op_id) | worker | dominant; includes CXL RTT + receiver work |
| 6 | rcv_poll | G (receiver flush+load req_op_id) | receiver | receiver's per-op poll cost |
| 7 | rcv_work | H (write_handler + execute_write_local_with_blk) | receiver | bucket lookup + dir update + slot publish |
| 8 | ack_publish | I (receiver writes resp_op_id + flush) | receiver | single CXL cacheline write+flush |

**CAVEAT about Stage 3 (value_xfer)**: `clflushopt` returns immediately;
`sfence` only drains pending flushes from the CPU side, NOT
"writes have landed in CXL memory". So Stage 3 measures CPU-side time
(typically 40-80 ns), NOT the actual CXL propagation latency for the
1024 B value. The propagation cost is **hidden in Stage 5** (the receiver's
flush+read of the pool block is where the wait surfaces).

### Probe tag inventory (15 tags)

Worker-side: `XWS1S, XWS1E, XWS2E, XWS3E, XWS4E, XWS5E` (boundary timestamps)
+ `XWS2R` (conditional, fires only if Stage 2 spin loop ran > 0 iters; payload =
iteration count) + `XWS5T` (conditional, fires only if Stage 5 timed out).

Receiver-side: `XWR6S, XWR6E, XWR7E, XWR8E` (boundary) + `XWR6Z, XWR6H, XWR6X`
(gap subsystem: encountered / healed within budget / exhausted budget).

See `src/cxl_probe.h` for the per-thread mmap ring frame format (24 B: tag 8B
+ TSC cycles 48b + cpu_id 16b + op_id 8B).

### Derived latencies (11)

| Name | Formula | Notes |
|---|---|---|
| `Stage1`..`Stage5` | adjacent worker boundary deltas | strict: sum == `StageW` |
| `Stage6`..`Stage8` | adjacent receiver boundary deltas | strict: sum == `StageR` |
| `StageW` | XWS5E − XWS1S | total worker op latency |
| `StageR` | XWR8E − XWR6S | total receiver work time |
| `RTT` | StageW − StageR (per matched op_id pair) | cross-host CXL roundtrip + receiver poll loop noise |

### Event counters

- `XWS2R rate` = % of worker ops where C-stage spin loop actually ran > 0 times. Steady-state expectation ≈ 0%; non-zero = ring saturation.
- `XWS5T rate` = % of worker ops where Stage 5 timed out. **Any non-zero rate signals a receiver pathology — investigate**.
- `XWR6Z rate` = gap encounter rate on receiver. High rate = receiver routinely reaches new slot before worker finished publish.
- `XWR6H` / `XWR6Z` = gap heal rate; `XWR6X` / `XWR6Z` = gap exhaust rate. If exhausted > 0, receiver is regularly bailing on incomplete entries.

### How to run a decomp experiment

Build the probe-instrumented binary (one-time, separate build dir to avoid
biasing main-build perf measurements):

```bash
# On each host (g3, g4):
cd ~/FUSEE_CXL && mkdir build-cxl-w1-v1024-probe && cd build-cxl-w1-v1024-probe
cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS="-DFUSEE_READ_GUARD=2 -DFUSEE_WRITE_ALLOC=1 -DFUSEE_PATH_COUNTERS=1 -DFUSEE_CACHE_VALUE_MAX=1024 -DFUSEE_PROBE=1" ..
make -j16 protocol_a_ycsb
```

Run a single-cell decomp (T=8, 1M trans ops example):

```bash
bash scripts/iter16A_xhost_decomp_run.sh 8 1000000 1
# → docs/iter16A_xhost_decomp_T8_<ts>/{raw,probes_h0,probes_h1,summary.txt,per_op.csv}
```

The driver:
1. starts probe-instrumented run on g3 + g4
2. rsync's per-thread probe.{pid}.{tid} files back
3. runs the analyzer; dumps summary + per-op CSV

### How to interpret the output

- **Identify the dominant stage**: look at which Stage has the largest p50.
  In steady-state today (V=1024, T=8 on g3/g4): Stage 5 (ack_wait) dominates
  at ~5000 ns p50, followed by Stage 2 (slot_wait) and Stage 6 (rcv_poll) at
  ~1100 ns each.
- **Verify probes don't break invariants**: `Stage1+2+3+4+5 == StageW`,
  `Stage6+7+8 == StageR`, `RTT > 0`. Analyzer reports each separately so
  divergence is visible.
- **Compare Stage 5 vs StageR**: their difference ≈ pure CXL roundtrip
  propagation + worker's slot-release + receiver poll-loop noise. Useful
  for distinguishing "is the worker waiting on receiver, or on the
  network" — large gap = network/poll, small gap = receiver bound.
- **Watch XWS5T**: any non-zero timeout signals receiver pathology;
  investigate before trusting numbers.
- **Probe overhead caveat**: rdtscp+lfence per probe ~30-50 cycles; 15
  probes per op ≈ 200-300 ns added overhead. Probe-instrumented thpt is
  typically 30-50% lower than probe-off baseline. **Use probe-off builds
  for thpt reporting; probe builds only for decomp analysis**.

### Cross-host op_id pairing (limitation)

`op_id` encodes `src_host` in the high 8 bits but the lower 56 bits are
the per-host `write_op_counter_`. After fork, child workers have
independent counters → op_id values can collide ACROSS workers on the
same host. The analyzer's RTT calculation pairs worker XWS* with
receiver XWR* by op_id; this pairing is correct in aggregate but
individual op-level latencies may be mismatched across colliding op_ids.
For aggregate p50/p99/avg this is acceptable; for exact per-op timeline
correlation, additional disambiguation (e.g., ring slot index) would be
needed.

## Cross-references

- Parent spec: `docs/microbench_4path_spec.md`
- Baseline data: `docs/iter15A_microbench_phase2_20260520_063314/` (T sweep)
- RCA evidence: `docs/iter15A_phase6_0c/` (latency), `docs/iter15A_phase6b_ring_c2c_*/` (no MESI), `docs/iter15A_phase6c_cv_doubling/` (saturation)
- iter-15A summary: `docs/iters/iter15A_summary_20260520.md`
