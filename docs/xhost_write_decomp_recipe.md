# `xhost_write_decomp_recipe.md` — process-ready decomposition recipe

Canonical procedure for measuring per-stage latency of the **cross-host
write path** in Protocol A. Established iter-16A. The same machinery
extends naturally to `xhost_read` / `local_write` / `local_read` by
replacing the probe set + the stage definitions for those paths (§7).

**Trigger words**: "xhost write decomp", "decomp recipe", "stage latency
sweep", "probe sweep" — run this recipe.

**Spec-parent**:
- `docs/microbench_xhost_spec.md` — the throughput-only xhost spec
- `docs/microbench_4path_spec.md` — the parent 4-path framework

---

## 1. Stage definition (xhost write only)

Cross-host write path = 8 canonical stages. Stages 1-5 run on the
**originating worker** (the host issuing the write); stages 6-8 run on
the **owner host's receiver thread**.

| # | Stage name | Code phase | Side | One-line description |
|---:|---|---|---|---|
| 1 | `slot_reserve` | B (B3 `ring->tail.fetch_add` + flush + sfence) | worker | reserve a slot on owner-host CXL ring tail |
| 2 | `slot_wait` | C (spin on `e->req_op_id == 0`) | worker | wait for prior owner of this slot to release |
| 3 | `value_xfer` | D (worker writes 1024 B value to owner's CXL pool) | worker | **CPU-side time only — actual CXL propagation hidden in Stage 5** |
| 4 | `ctrl_publish` | E (worker fills entry cacheline 1 + publishes `req_op_id`) | worker | single CXL cacheline write + flush + sfence |
| 5 | `ack_wait` | F (worker spin on `resp_op_id == op_id`) | worker | dominant; includes CXL RTT + receiver work + spin overhead |
| 6 | `rcv_poll` | G (flush+load `ring->tail` + `req_op_id`) | receiver | per-op poll cost (excludes outer-loop overhead) |
| 7 | `rcv_work` | H (bucket+dir+CoW publish in `execute_write_local_with_blk`) | receiver | bucket lookup + slot publish |
| 8 | `ack_publish` | I (write `resp_op_id` + flush + sfence) | receiver | single CXL cacheline write + flush + sfence |

**Stage code reference**: [src/cxl_kv_ops_A.cc:1453](src/cxl_kv_ops_A.cc#L1453)
(`forward_write_direct`, stages 1-5) and
[src/cxl_kv_ops_A.cc:2415](src/cxl_kv_ops_A.cc#L2415) (`write_receiver_loop`,
stages 6-8).

---

## 2. Probe machinery

Probes via `PROBE_OP(tag, op_id)` macro in
[src/cxl_probe.h](src/cxl_probe.h). Per-thread mmap'd ring at
`$FUSEE_PROBE_DUMP.{pid}.{tid}`, 24 B frames:
- 8 B: tag (zero-padded ASCII, ≤ 6 char to be safe)
- 8 B: low 48 b = TSC cycles, high 16 b = cpu_id (rdtscp's TSC_AUX)
- 8 B: op_id (or stage-specific payload)

`FUSEE_PROBE=1` compile flag enables; default off.

### Probe tag inventory (15 tags, all 5 chars)

Naming convention: `XW[S|R][stage_number][S|E|event_letter]`.

**Worker** (8 tags):

| Tag | Position | Payload | Type |
|---|---|---|---|
| `XWS1S` | function entry, after op_id construction | op_id | regular |
| `XWS1E` | after B4 sfence (= stage 2 start) | op_id | regular |
| `XWS2E` | after C exits (= stage 3 start) | op_id | regular |
| `XWS2R` | conditional: if C loop spun > 0 iters | c_iters count (not op_id) | conditional |
| `XWS3E` | after D ends (= stage 4 start) | op_id | regular |
| `XWS4E` | after E4 sfence (= stage 5 start) | op_id | regular |
| `XWS5E` | F exit (success or timeout) | op_id | regular |
| `XWS5T` | conditional: F timed out | op_id | conditional |

**Receiver** (7 tags):

| Tag | Position | Payload | Type |
|---|---|---|---|
| `XWR6S` | inner-loop entry per new slot | (head & 0xFFFFFFFF) \| ((tail & 0xFFFFFFFF) << 32) | regular |
| `XWR6E` | op_id != 0 seen, before write_handler | op_id | regular |
| `XWR6Z` | conditional: gap (op_id == 0 on first load) | head | conditional |
| `XWR6H` | conditional: gap healed within budget | op_id | conditional |
| `XWR6X` | conditional: gap budget exhausted | head | conditional |
| `XWR7E` | write_handler returned | op_id | regular |
| `XWR8E` | after I3 sfence | op_id | regular |

---

## 3. Latency definitions (11 latencies)

Derived from probe timestamps:

### Per-op, single host (10)

| Name | Formula | Side |
|---|---|---|
| `Stage1` | XWS1E − XWS1S | worker |
| `Stage2` | XWS2E − XWS1E | worker |
| `Stage3` | XWS3E − XWS2E | worker |
| `Stage4` | XWS4E − XWS3E | worker |
| `Stage5` | XWS5E − XWS4E | worker |
| `StageW` | XWS5E − XWS1S | worker (= Σ Stage1..5) |
| `Stage6` | XWR6E − XWR6S | receiver |
| `Stage7` | XWR7E − XWR6E | receiver |
| `Stage8` | XWR8E − XWR7E | receiver |
| `StageR` | XWR8E − XWR6S | receiver (= Σ Stage6..8) |

### Cross-host (1)

| Name | Formula | Notes |
|---|---|---|
| `RTT` | StageW − StageR (per matched op_id) | includes pure CXL prop + worker spin overhead + receiver poll-loop overhead |

### Strict invariants (analyzer sanity-checks)

```
Stage1 + Stage2 + Stage3 + Stage4 + Stage5  ==  StageW
Stage6 + Stage7 + Stage8                    ==  StageR
RTT > 0
```

---

## 4. Event counters (4 rates)

| Name | Triggered by | Interpretation |
|---|---|---|
| `cnt_XWS2R` | XWS2R | C-stage spin loop ran > 0 iters. Rate = % of ops where ring was contended. Steady-state expectation ≈ 0%. |
| `cnt_XWS5T` | XWS5T | Stage 5 timed out (5 ms budget exceeded). **Any non-zero rate signals receiver pathology — investigate.** |
| `cnt_XWR6Z` | XWR6Z | Receiver hit a gap (worker fetch_add'd but hadn't published yet). |
| `cnt_XWR6H` / `cnt_XWR6Z` | ratio | Gap heal rate (within 4096-pause budget). |
| `cnt_XWR6X` / `cnt_XWR6Z` | ratio | Gap exhaust rate. If > 0, receiver is bailing on unfinished entries. |

---

## 5. Canonical experiment parameters

Match iter-15A Phase 2 / RN-A so all data lines up:

| Parameter | Value |
|---|---|
| `V` (KV size) | **1024** |
| Workload name | `xhost_write` |
| Key distribution | `zipf-0.99` |
| `--num-load` (trace gen) | **10000000** |
| `--num-trans` (trace gen) | **5000000** |
| `NUM_BUCKETS` | **8388608** (8M, fixed since iter-15A) — main hashtable size, sized to hold 10M unique keys |
| `FUSEE_CACHE` | 1 (enabled) |
| `FUSEE_CACHE_BUCKETS` | **131072** — cache pool size = 524288 entries (4 per bucket) ≈ **10% of per-host key coverage (5M keys)**, NOT 10% of NUM_BUCKETS |
| `T` (workers per host) | **{1, 2, 4, 8, 16, 32, 64}** |
| reps per cell | 3 |
| `protocol_a_ycsb` trans_ops arg | 5000000 |

**These parameters MUST be identical between probe-off (thpt) and
probe-on (decomp) sweeps so comparisons are valid.**

---

## 6. Recipe to run

### 6.1 One-time setup

**Traces** (3 min, only if not present in `setup/microbench_traces/`):

```bash
python3 scripts/iter14A_gen_microbench_traces.py setup/microbench_traces \
  --num-load 10000000 --num-trans 5000000 --num-hosts 2 --keydists "zipf-0.99"
```

**Two builds on g3 + g4**:

```bash
# Main build (probe-OFF) — for thpt numbers
ssh root@g3 'cd /root/FUSEE_CXL/build-cxl-w1-v1024 && make -j16 protocol_a_ycsb'

# Probe build (probe-ON) — for decomp; separate dir
ssh root@g3 'cd /root/FUSEE_CXL && rm -rf build-cxl-w1-v1024-probe && \
  mkdir build-cxl-w1-v1024-probe && cd build-cxl-w1-v1024-probe && \
  cmake -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="-DFUSEE_READ_GUARD=2 -DFUSEE_WRITE_ALLOC=1 -DFUSEE_PATH_COUNTERS=1 -DFUSEE_CACHE_VALUE_MAX=1024 -DFUSEE_PROBE=1" \
        .. && make -j16 protocol_a_ycsb'
# Repeat for g4.
```

### 6.2 Probe-off T-sweep (thpt baseline)

```bash
bash scripts/iter16A_xhost_T_sweep.sh
# → docs/iter16A_xhost_T_sweep_<ts>/
#     grid.csv, raw/*, xhost_write_T_plot_loglin.png,
#     xhost_write_T_summary_table.png, xhost_write_T_summary.txt
```

~10 min for 21 runs. Output is comparable to iter-15A Phase 2 grid.csv.

### 6.3 Probe-on decomp sweep (stage latencies)

```bash
# Default: T={1,8,64} sanity (~15 min, 9 runs)
bash scripts/iter16A_xhost_decomp_sweep.sh

# Full: T={1,2,4,8,16,32,64} (~30 min, 21 runs)
bash scripts/iter16A_xhost_decomp_sweep.sh "1 2 4 8 16 32 64"
```

Output: `docs/iter16A_xhost_decomp_sweep_<ts>/`:
- `aggregate.csv` — per-cell row, all 11 latencies + 4 counters + thpt
- `cells/T<T>_rep<r>/`:
  - `h0.out`, `h1.out` — stdout
  - `probes_h0/`, `probes_h1/` — raw probe files (~5-20 MB each after truncate)
  - `per_op.csv` — per-op stage row
  - `summary.txt` — analyzer output (the stats table + counters)

### 6.4 Visualization (14 plots)

```bash
python3 scripts/iter16A_decomp_viz.py docs/iter16A_xhost_decomp_sweep_<ts>/aggregate.csv
```

Produces in the same directory:
- **8 mandatory per-stage plots**: `Stage1_T_p50p99.png` ... `Stage8_T_p50p99.png`
- **3 aggregate plots**: `StageW_T_p50p99.png`, `StageR_T_p50p99.png`, `RTT_T_p50p99.png`
- **3 summary plots**: `worker_stages_overlay.png`, `receiver_stages_overlay.png`, `stage_contribution_stacked.png`
- `decomp_summary.txt` — text table

### 6.5 After decomp done — clean up

The raw probe files (`probes_h0/`, `probes_h1/` per cell) can total several
GB. After viz + `decomp_summary.txt` are saved, **delete raw probe dirs**:

```bash
rm -rf docs/iter16A_xhost_decomp_sweep_<ts>/cells/*/probes_h0 \
       docs/iter16A_xhost_decomp_sweep_<ts>/cells/*/probes_h1
```

Keep: `aggregate.csv`, `per_op.csv` (per cell), `summary.txt` (per cell),
all PNG plots, `decomp_summary.txt`.

---

## 7. How to extend to other paths

The 8-stage decomp above is **specific to xhost_write**. Other paths have
different stage structures:

### 7.1 `xhost_read`

Receiver path is `read_handler` (already has L0/L2/L3 receiver-NOOP
implementations). Worker side is `forward_read_direct`. Stages would be
similar in shape (ring reserve → publish → ack-wait) but with read-specific
substages on receiver (directory lookup + pool read instead of CoW publish).

**To extend**:
1. Define stages for read path (likely 5 worker + 3 receiver, naming `XRS*` / `XRR*`).
2. Add probes in `forward_read_direct` ([src/cxl_kv_ops_A.cc:~1988](src/cxl_kv_ops_A.cc#L1988)) and `read_handler` / `read_receiver_loop`.
3. Update analyzer to support read tag namespace + per-op pairing.

### 7.2 `local_write`

No cross-host, no ring, no receiver. Path is purely worker → bucket lookup
→ directory → CoW publish → cache update. Stage shape is **shorter (3-4
stages)**, no ack-wait.

**To extend**:
1. Define stages: bucket_fetch, dir_lock+publish, cache_update.
2. Add probes in `execute_write_local` ([src/cxl_kv_ops_A.cc:455](src/cxl_kv_ops_A.cc#L455)).
3. Existing `W1..W12` probes already exist at relevant points — could be
   repurposed/renamed into stage scheme.

### 7.3 `local_read`

Similar to `local_write`: pure local. Probes `R1, R2hit, R2miss, R3, R4, R6` already exist; redefine as stages.

### Extension template

For each new path:
1. **Stage definition**: enumerate the stages (write into this doc's §7 as a new subsection).
2. **Probe tags**: ≤ 5 chars, follow `XW[S|R]<stage><event>` or `XR<stage><event>` (read) convention.
3. **Code probe insertion**: PROBE_OP at stage boundaries.
4. **Build flag**: same `-DFUSEE_PROBE=1` build dir works.
5. **Analyzer**: extend `iter16A_xhost_decomp_analyze.py` to recognize the new tag set.
6. **Sweep driver**: copy `iter16A_xhost_decomp_sweep.sh`, change `SCEN=` to the new workload.
7. **Viz**: copy `iter16A_decomp_viz.py`, change `STAGES = [...]` list.
8. **Doc**: write a sibling recipe doc `<path>_decomp_recipe.md`.

---

## 8. Methodology caveats

### 8.1 Probe overhead — interpretation of small stages

Each `PROBE_OP` ≈ 30-50 ns (rdtscp + lfence + mmap'd write). Per op, ~10
probes → ~300 ns added overhead per op.

For stages with p50 > 500 ns (Stage 1, 2, 5, 6, 7), probe overhead is
< 10% — measured numbers are representative.

For stages with p50 < 100 ns (Stage 3, 4, 8), probe overhead dominates —
**measured values should be treated as upper-bounds**, not as the true
stage cost. These are annotated as "probe-overhead-bound" in the viz.

### 8.2 Stage 3 (value_xfer) caveat

`clflushopt` is async on x86; `sfence` only drains pending flushes from
the CPU side, NOT "writes have landed in CXL memory". Stage 3 measures
CPU-side time only (~40 ns). The **actual CXL propagation cost is hidden
in Stage 5** (when the receiver tries to read the value bytes, the
propagation delay surfaces).

### 8.3 Cross-host op_id pairing ambiguity

`encode_op_id` packs `(host_id + 1)` in high 8 bits + per-host
`write_op_counter_` in low 56 bits. After fork, child workers have
**independent** counters → op_ids can collide across workers on the
same host. The analyzer's RTT calculation pairs worker XWS* with
receiver XWR* by op_id; pairings are correct in aggregate (distributions
of latencies are valid), but individual op-level timelines may be
mismatched. Acceptable for p50/p99 aggregates.

### 8.4 Receiver-only data on the "wrong" probe dir

A host runs BOTH worker AND receiver simultaneously: h0 has both its
own workers (issuing XWS* events) AND its receiver thread (processing
h1's writes, issuing XWR* events). The analyzer currently treats one
dir as worker-side, one as receiver-side, which only captures one
direction of the pairing (h0_workers → h1_receivers). The other half
(h1_workers → h0_receivers) is in the OPPOSITE dirs.

For aggregate stats this typically doesn't matter (both directions
should be symmetric), but if asymmetry is suspected, run the analyzer
twice with dirs swapped.

### 8.5 Probe-on thpt is NOT comparable to probe-off thpt

Probe-on builds run ~3× slower than probe-off (smoke measurement). Use:
- **probe-off build** for throughput reporting + cross-iter comparison
- **probe-on build** for stage decomposition only

The two sweeps share identical params (§5) so the decomp gives a
representative view of stage proportions even though absolute numbers
are biased.

---

## 9. Iter cadence

- **After any cross-host write code change**: run §6.2 probe-off T-sweep.
  Verify thpt within ±5% of baseline.
- **For optimization studies**: run §6.3 probe-on decomp. Identify
  dominant stage. Target that stage for fix.
- **End of iter**: archive `aggregate.csv` + key plots; delete raw probe
  dirs per §6.5.

---

## 10. Iter-15A / iter-16A established baselines

| Cell (V=1024, zipf-0.99, cache=10%) | T=1 | T=8 | T=64 |
|---|---:|---:|---:|
| **probe-off thpt (post-H9-fix, 2026-05-21)** | 0.205 | 0.676 | 0.741 |
| iter-15A Phase 2 (pre-H9) | 0.197 | 0.555 | 0.604 |
| Δ% | +4 | +22 | +23 |

Any future change must regression-test against the post-H9-fix line.

## 11. Cross-references

- Stage code: [src/cxl_kv_ops_A.cc](src/cxl_kv_ops_A.cc) (forward_write_direct, write_receiver_loop)
- Probe machinery: [src/cxl_probe.h](src/cxl_probe.h)
- Analyzer: [scripts/iter16A_xhost_decomp_analyze.py](scripts/iter16A_xhost_decomp_analyze.py)
- Probe-off sweep driver: [scripts/iter16A_xhost_T_sweep.sh](scripts/iter16A_xhost_T_sweep.sh)
- Probe-on sweep driver: [scripts/iter16A_xhost_decomp_sweep.sh](scripts/iter16A_xhost_decomp_sweep.sh)
- Viz: [scripts/iter16A_decomp_viz.py](scripts/iter16A_decomp_viz.py)
- Plot script (T-sweep): [scripts/iter16A_T_sweep_plot.py](scripts/iter16A_T_sweep_plot.py)
- Parent: [docs/microbench_xhost_spec.md](docs/microbench_xhost_spec.md), [docs/microbench_4path_spec.md](docs/microbench_4path_spec.md)
