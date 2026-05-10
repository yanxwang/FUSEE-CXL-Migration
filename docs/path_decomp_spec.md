# `path_decomp` — standard read/write path decomposition diagnostic protocol

This document defines the canonical **per-stage performance forensics**
procedure used to diagnose a Protocol A iteration's read/write path
when (a) a sweep gate fails, (b) collapse appears, (c) throughput
plateaus below target, or (d) a structural change to the protocol
needs validation.

It is the diagnostic counterpart to `scaling_ycsb` (which validates
end-to-end performance across a workload matrix). `scaling_ycsb`
**measures**; `path_decomp` **explains**.

Every invocation produces a self-contained directory under
`docs/path_decomp_<iter_tag>_<timestamp>/` with all 5 phases'
artifacts.

**Origin**: codified from iter-8A Phase 3 mandate completion (the
spare-time work that produced `docs/iter8A_phase1_ubench/`). That
directory is the reference implementation of one full path_decomp run.

---

## 1. When to invoke

`path_decomp` is **mandatory** before:
- shipping any Protocol A performance fix (iter-9A onwards: `scaling_ycsb_spec §13 gate 6`)
- declaring root cause for any collapse / anomaly cell
- proposing structural architecture changes that touch the read/write path

`path_decomp` is **optional but recommended** for:
- routine end-of-iter validation that no stage regressed
- baseline establishment after a primitive (CXL hardware, OS kernel) change
- before a major refactor that might shift bottlenecks

`path_decomp` is **NOT** a replacement for `scaling_ycsb`. Both must
run for every iter shipping a perf change: scaling_ycsb shows
*whether* perf moved; path_decomp shows *which stage* moved and *why*.

---

## 2. Testbed / Build

Inherits from `scaling_ycsb_spec.md §1-2` unchanged:
- 2 hosts g3 + g4
- `/dev/dax0.0` shared CXL Type-3
- Branch `feat/cxl-migration`
- Build with `-DFUSEE_PROBE=1` (probe instrumentation enabled)

Additional requirements:
- `perf` 6.15+ on both hosts (`apt install linux-perf`)
- `bpftrace` 0.23+ + `bpfcc-tools` (`apt install bpftrace bpfcc-tools`)
- TSC must be reliable + invariant (verified via `dmesg | grep "tsc:"`)

---

## 3. The 5 phases

```
┌─────────────────┐
│ Phase 0: Bound  │  µbench (Sol-4) primitives one-shot per iter
│   Sol-4         │  → Expected[stage] table
└─────────────────┘
        │
        ▼
┌─────────────────┐
│ Phase 1: Capture│  RDTSCP probe (Sol-1) on healthy + anomaly cells
│   Sol-1         │  → per-thread mmap dumps
└─────────────────┘
        │
        ▼
┌─────────────────┐
│ Phase 2: Detect │  Per-stage table: Expected / Healthy / Anomaly
│   Sol-1 parser  │  → flag anomalous stages (criteria §6)
└─────────────────┘
        │
        ▼ (if anomalies flagged)
┌─────────────────┐
│ Phase 3: Diagnose│ For each flagged stage, follow decision tree §7:
│   Sol-2 + Sol-3 │   → perf record (on-CPU symbol + ASM)
│                 │   → perf sched (off-CPU + sched delay)
│                 │   → cross-reference with Sol-4 baseline
└─────────────────┘
        │
        ▼
┌─────────────────┐
│ Phase 4: Name   │  Single named root cause + RAP (per §XIII)
│ + Fix           │  → ship + re-run path_decomp Phases 1-2 to verify
└─────────────────┘
        │
        ▼
┌─────────────────┐
│ Phase 5: Codify │  → Update blueprint Part II Expected baselines
│                 │  → Add new anomaly pattern to anomaly-scan library
└─────────────────┘
```

---

## 4. Phase-by-phase deliverables

### Phase 0 — µbench baseline (Sol-4)

**Tool**: `tests/cxl_primitive_bench.cc`
**Run**: `./build-cxl/tests/cxl_primitive_bench` on g3 (single-host;
cross-host primitives optional unless suspected as cause).

**Required output**: `<dir>/baseline.md` with columns:
| Primitive | p50 ns | p99 ns | max ns |

**Required primitives** (any subset relevant to current iter's
suspected stages — full list is the union of all Protocol A
stage primitive lists):
- mfence / sfence / lfence
- clflushopt + sfence (CXL line)
- LD-CXL post-flush+mfence
- ST-CXL + flush + sfence
- CXL atomic fetch_add (no flush; with flush+sfence)
- Spinlock uncontested
- Spinlock T=N contended (N spans current sweep T grid)

**Skippable if**: previous iter's baseline still valid (no kernel /
hardware / compiler change). Document reuse in `<dir>/baseline.md`
header.

**Phase 0 exit criterion**: Expected[stage] computed for every stage
in `protocol_a_architecture_blueprint.md` Part II that the iter
touches.

### Phase 1 — Healthy + Anomaly capture (Sol-1)

**Tool**: `scripts/iter8A_residual_capture.sh` template (copy + edit
for your cell).

**Required captures** (minimum):
1. **Healthy** (1 capture, may need retry if anomaly is the common
   case): a representative non-anomaly run of the suspect cell.
2. **Anomaly** (1 capture; retry up to 12× to defeat Heisenberg):
   the slowest cell from the most recent `scaling_ycsb` sweep.

If anomaly is frequent (≥30%), 1-3 retries suffice; if rare (<10%),
budget up to 12 retries.

**Required output**: `<dir>/probes_healthy/`, `<dir>/probes_anomaly/`,
each containing 132+ probe files (per-thread mmap dumps).

**Phase 1 exit criterion**: both healthy + anomaly probe sets exist;
at least one healthy run >50% of target throughput; at least one
anomaly run <10% of healthy throughput (or equivalent severity).

### Phase 2 — Per-stage table (Sol-1 parser)

**Tool**: `scripts/parse_probes_v4.py`

**Required output**: `<dir>/per_stage_decomp.md` with columns:
| Stage | Expected | Healthy p50 | Healthy p99 | Healthy max | Anomaly p50 | Anomaly p99 | Anomaly max | C/H ratio | Status |

**Status column rule**:
- `OK`: C/H < 5× AND H/E < 5×
- `anomaly: <metric>`: C/H ≥ 5× OR H/E ≥ 5× → triggers Phase 3
- `inter-op`: stage marks an op-end; "duration" is gap to next op (not a stage cost)

**Phase 2 exit criterion**: all 32 stages from blueprint Part II
appear in the table. Stages with no probe data must be explicitly
labeled `✱ no data — inferred OK because <reason>` or
`✱ no data — Phase 3 cannot diagnose, defer to next iter with
instrumentation added`.

### Phase 3 — Diagnose flagged stages (Sol-2 + Sol-3)

For each stage tagged `anomaly: ...` in Phase 2:

**Sol-2 perf record**:
**Tool**: `scripts/iter8A_perf_sys_capture.sh` template
**Required output**: `<dir>/perf_capture/<stage>_perf_report.txt`
(top symbols filtered to `protocol_a_ycsb`) +
`<dir>/perf_capture/<stage>_hot_asm.txt` (annotation of the function
implementing this stage).

**Sol-3 perf sched**:
**Tool**: `perf sched record -a -o <dir>/perf_capture/sched.data
-- sleep 60` during a fresh anomaly capture
**Required output**: `<dir>/perf_capture/sched_latency.txt`
(filter to `protocol_a_ycsb`)

**Cross-reference table** (filled per-flagged-stage):
| Question | Method | Answer |
|---|---|---|
| Is this stage CPU-bound? | Sol-2 (look for our symbol in top-10) | yes/no |
| If CPU-bound, what fraction is on memory? | Sol-2 (ASM: % on LD/ST/MFENCE) | X% |
| Is the thread descheduled long enough to explain anomaly? | Sol-3 (max sched delay vs anomaly magnitude) | yes/no |
| Does primitive cost from baseline.md account for it? | Sol-4 (Expected vs measured H) | yes/no |

**Phase 3 exit criterion**: ≥80% of the C-H gap (anomaly minus healthy
median) on the flagged stage is attributed to a specific named
mechanism (lock contention / CXL polling / scheduler / algorithm).

### Phase 4 — Name root cause + ship fix

**Required output**: `<dir>/rap.md` per `design_goals.md §XIII RAP`
format (≥6 attack vectors, ablation, prior-art check, verdict).

**Fix verification**: After fix lands, re-run Phases 1-2 only on the
same anomaly cell. Status must transition from `anomaly: ...` to `OK`.

**Phase 4 exit criterion**: fix shipped + verification re-run shows
target stage now OK.

### Phase 5 — Codify

Update `protocol_a_architecture_blueprint.md` Part II:
- Replace stale Expected baselines with measured-derived values
- Add the discovered anomaly pattern + its named cause to the
  blueprint's Part III "Failure modes" section

Update `scripts/probe_anomaly_scan.py` library:
- Add the new anomaly signature so future runs auto-flag it

**Phase 5 exit criterion**: blueprint diff in iter commit.

---

## 5. Output directory layout

```
docs/path_decomp_<iter_tag>_<timestamp>/
├── README.md                 # links to all artifacts; cell config; iter context
├── baseline.md               # Phase 0
├── per_stage_expected.md     # Phase 0 derived Expected per stage
├── probes_healthy/           # Phase 1 raw mmap dumps (sparse files)
├── probes_anomaly/           # Phase 1 raw mmap dumps
├── per_stage_healthy.md      # Phase 2 parsed
├── per_stage_anomaly.md      # Phase 2 parsed
├── per_stage_decomp.md       # Phase 2 consolidated table
├── perf_capture/             # Phase 3 (only if anomalies flagged)
│   ├── perf.data             # raw
│   ├── perf.sched.data       # raw
│   ├── perf_report_top.txt
│   ├── <stage>_hot_asm.txt   # one per flagged stage
│   └── sched_latency.txt
├── rap.md                    # Phase 4
└── fix_verification.md       # Phase 4 re-run results
```

---

## 6. Anomaly flag criteria (Phase 2 → Phase 3 trigger)

A stage triggers Phase 3 if **any** of:

1. **Anomaly amplification**: `Anomaly_p50 / Healthy_p50 ≥ 5×`
2. **Healthy gap**: `Healthy_p50 / Expected ≥ 5×`
3. **Tail blowup**: `Anomaly_max ≥ 100 × Healthy_p50`
4. **Hard-cap detect**: `Anomaly_p99 ≈ N × known_cap_value` (e.g.,
   exact multiple of 5 ms = `forward_spin_wait` cap)

The threshold `5×` is tunable per iter but must be documented in
`<dir>/README.md` if changed.

---

## 7. Decision tree (Phase 3)

For each flagged stage:

```
Q1: Is on-CPU sample count for this stage's function in Sol-2 top-10?
├─ YES → Q2
└─ NO  → Q3 (the thread is mostly off-CPU during this stage)

Q2: What dominates ASM samples in the function?
├─ LD-CXL / mfence  → Named cause: CXL memory latency bound
│                     Fix candidate: K-shard / batching / cache locality
├─ Lock CAS         → Named cause: lock contention
│                     Fix candidate: lock-free / finer lock / per-thread state
├─ Specific algo    → Named cause: algorithm cost
│                     Fix candidate: algorithm replacement
└─ Spread evenly    → Named cause: ambient overhead
                      Fix candidate: re-evaluate stage decomposition

Q3: What does Sol-3 say?
├─ max sched delay >> anomaly magnitude
│   → Named cause: CPU starvation / scheduler displacement
│   → Fix candidate: thread pin / RT priority / dedicated core
├─ max sched delay << anomaly magnitude
│   → Named cause: thread is sleeping voluntarily (futex_wait, etc)
│   → Investigate: is there a hidden cross-thread synchronization?
└─ sched delay roughly matches
    → Named cause: combination of OS + thread sync; needs deeper trace
```

---

## 8. Pass/fail gates for the iter

`path_decomp` **passes** for an iter if:
- Phase 0 baseline exists for every primitive used by the iter's
  touched stages
- Phase 2 table covers all 32 blueprint stages (with `✱ no data`
  documented justification for those without probe coverage)
- Every flagged stage in Phase 2 has Phase 3 attribution
- Every fix shipped has Phase 4 verification showing the flagged
  stage now OK

`path_decomp` **fails** for an iter if any of:
- Phase 2 has unjustified `✱ no data` (hides anomaly potential)
- Phase 3 attribution is "probabilistic transients" / "system
  stochasticity" / equivalent hand-wave (per `design_goals.md §X
  P5` HARD enforcement)
- Phase 4 fix verification shows the flagged stage still anomalous
  (the fix didn't actually fix it; iter-N+1 must re-attempt)

---

## 9. Reproducibility requirements

- `<dir>/README.md` must include: iter tag, sweep cell config (wl,
  KV, T, cache mode), commit hash at capture time, retry count for
  anomaly capture, TSC frequency calibration value.
- All raw probe files preserved (NOT deleted to save disk; sparse
  files only consume populated pages).
- All `.sh` capture scripts saved into `<dir>/scripts_used/`.

---

## 10. Cost budget

A typical full path_decomp run (4 of 4 phases including perf+sched)
takes:
- Phase 0: 2-5 min (µbench)
- Phase 1: 3-15 min (depending on retry count)
- Phase 2: <1 min (parser)
- Phase 3: 5-10 min (perf annotate + sched analysis)
- Phase 4: variable (depends on fix complexity)
- Phase 5: 5-10 min (doc updates)

**Total non-fix wall**: ~30-45 min. Compatible with "短-密-快"
principle. **Not** a long sweep.

---

## 11. Reference implementation

`docs/iter8A_phase1_ubench/` is the reference instance. It deviates
from this spec only in directory naming (it predates the spec). It
contains:
- baseline.md (Phase 0)
- per_stage_expected.md (Phase 0 derived)
- per_stage_healthy.md / per_stage_collapsed.md (Phase 1+2)
- per_stage_decomp_table.md (Phase 2 consolidated)
- perf_capture/ (Phase 3)

Future iters' path_decomp output dirs follow the §5 layout.
