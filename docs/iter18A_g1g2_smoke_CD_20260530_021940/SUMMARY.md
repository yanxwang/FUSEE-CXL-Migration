# g1/g2 smoke C+D: extend coverage of x8-regression validation

**Date**: 2026-05-30

After RCA v3 confirmed both Micron CXL modules at x8 (downgraded from
x16), ran two additional cells on g1/g2 to test whether the ~50 %
throughput reduction holds across the (T, N) space, not just at the
two A/B cells from yesterday.

## Results

g1/g2 (3-rep median, all cache=0, V=1024, zipf-0.99, worker_id routing,
build-cxl-w1-v1024 with iter-18A flags), compared to iter-18A Phase 4
sweep (worker_id reps 1-3):

| Cell | T | N | Today (Mops) | iter-18A median (Mops) | ratio |
|------|---|---|----:|----:|----:|
| A | 32 | 0 | 0.634 | 1.181 | **54 %** |
| B | 16 | 4 | 2.278 | 4.066 | **56 %** |
| **C** | **64** | **4** | **2.777** | **5.203** | **53 %** |
| **D** | **8**  | **0** | **0.578** | **1.108** | **52 %** |

## Implication

**All 4 cells regressed to 52-56 % of historical** — consistent within
4 pp across a 16× range of effective parallelism (T·max(N,1) =
8 → 256). This is much stronger evidence than two cells alone.

Important: cell D (T=8 N=0) is the lowest-concurrency cell I sampled,
which I initially expected to be latency-dominated (i.e., less affected
by BW reduction). It also halved. This indicates **the FUSEE
xhost_read path is BW-bound across the entire sampled regime**, not
just at high T. The bottleneck stage in FUSEE (likely receiver-side
load aggregation against the CXL slab) is the BW-amplifier, and
halving CXL BW directly halves throughput regardless of input
concurrency.

## Predicted post-recovery numbers

If h3-side link retraining recovers Micron to x16, expect each cell's
throughput to come back to ~1.9× its current value (since
x16/x8 = 2, but with some real-world efficiency loss). The 4 cells
form a calibration vector: post-recovery sweep should land at:

| Cell | Predicted (×1.9) | iter-18A historical |
|------|----:|----:|
| A | ~1.20 Mops | 1.18 Mops |
| B | ~4.33 Mops | 4.07 Mops |
| C | ~5.28 Mops | 5.20 Mops |
| D | ~1.10 Mops | 1.11 Mops |

A match across all 4 to within ±5 % will confirm a clean recovery.
Use this as the gate before declaring testbed "back to iter-18A
baseline" — anything else (asymmetric recovery, only some cells
back) needs separate investigation.

## Data
[grid.csv](grid.csv) + [raw/](raw/) (3 reps per cell, all wallclock
21-31 s).

---

## 2026-05-31 — Switch remap to 4×Micron (post-remap smoke)

**What changed**: user reconfigured XConn switch poolmap so g1/g2 now
attach **4 Micron 128 GB DDR4 modules (4×128 = 512 GB total)** via
dax0.0 instead of the previous **2 Samsung 128 GB DDR5 modules** (256 GB).
Switch DSP port HW remains G5x8 (this is the physical port limit; not
changeable by remap). Goal: test whether aggregating more modules
behind the same x8 switch port lifts the BW ceiling.

After remap, `daxctl list` on both g1 and g2 reports
`size: 549755813888` (= 512 GiB ✓).

### Smoke setup

Identical to the 2026-05-30 smoke: `build-cxl-w1-v1024` (iter-18A
flags `-DFUSEE_READ_GUARD=2 -DFUSEE_WRITE_ALLOC=1 -DFUSEE_PATH_COUNTERS=1
-DFUSEE_CACHE_VALUE_MAX=1024`), `FUSEE_CACHE=0`, V=1024, zipf-0.99,
worker_id routing. 4 cells × 3 reps. Script:
[scripts/iter20A_remap_smoke.sh](../../scripts/iter20A_remap_smoke.sh).
Raw: [docs/iter20A_remap_smoke_20260531_091057/](../iter20A_remap_smoke_20260531_091057/).

### Per-cell 3-rep results — three platforms side-by-side

All throughput in Mops. Three columns:

- **g34 hist** = g3/g4, iter-18A Phase 4 sweep (2026-05-23, zipf-0.99,
  worker_id routing). Source:
  [docs/iter18A_phase4_7group_sweep_20260523_061524/grid.csv](../iter18A_phase4_7group_sweep_20260523_061524/grid.csv).
  At the time g3/g4 mlc-measured 51.78 GB/s CXL BW (G5x16 switch path).
- **g12 pre** = g1/g2 with 2×Samsung 128 GB DDR5 (256 GB total) via
  G5x8 switch port. A/B from
  [smoke_20260529_082015](../iter18A_g1g2_smoke_20260529_082015/grid.csv),
  C/D from
  [smoke_CD_20260530_021940](grid.csv).
- **g12 post** = g1/g2 with 4×Micron 128 GB DDR4 (512 GB total) via
  same G5x8 switch port. Today (2026-05-31). Source:
  [docs/iter20A_remap_smoke_20260531_091057/grid.csv](../iter20A_remap_smoke_20260531_091057/grid.csv).

| Cell | rep | g34 hist | g12 pre (2×Sam) | g12 post (4×Mic) | post÷hist | post÷pre |
|------|----:|----:|----:|----:|----:|----:|
| **A** (T=32 N=0) | 1 | 1.180 | 0.625 | **0.639** | 54.2% | +2.3% |
|                  | 2 | 1.172 | 0.637 | **0.636** | 54.3% | −0.2% |
|                  | 3 | 1.052 | 0.623 | **0.628** | 59.7% | +0.9% |
|                  | **median** | **1.172** | **0.634** | **0.636** | **54.3%** | **+0.4%** |
| **B** (T=16 N=4) | 1 | 4.066 | 2.132 | **2.147** | 52.8% | +0.7% |
|                  | 2 | 4.271 | 2.138 | **2.172** | 50.9% | +1.6% |
|                  | 3 | 4.064 | 2.304 | **2.160** | 53.2% | −6.3% |
|                  | **median** | **4.066** | **2.138/2.278** | **2.160** | **53.1%** | **±5%** |
| **C** (T=64 N=4) | 1 | 4.952 | 2.943 | **2.538** | 51.3% | −13.8% |
|                  | 2 | 5.203 | 2.648 | **2.872** | 55.2% | +8.5% |
|                  | 3 | 5.897 | 2.777 | **2.893** | 49.1% | +4.2% |
|                  | **median** | **5.203** | **2.777** | **2.872** | **55.2%** | **+3.4%** |
| **D** (T=8 N=0)  | 1 | 1.163 | 0.568 | **0.585** | 50.3% | +3.0% |
|                  | 2 | 1.108 | 0.579 | **0.573** | 51.7% | −1.0% |
|                  | 3 | 1.086 | 0.578 | **0.581** | 53.5% | +0.5% |
|                  | **median** | **1.108** | **0.578** | **0.581** | **52.4%** | **+0.5%** |

**4-cell median geometric mean**: g12 post = **53.7%** of g34 hist
(individual cell ratios 52–55%, very tight).

Note on Cell B: g12 pre A/B medians from the second batch
(`docs/iter18A_g1g2_smoke_20260529_082015/`); the first batch
(20260529_081658) gives B median 2.138 (vs 2.278 second batch). Both
are noise-level from post-remap 2.160 — verdict unchanged regardless
of which pre-remap batch is used as reference.

### Latency cross-check

p50 latency at each platform tells the same story from the latency side:

| Cell | g34 hist p50 (µs) | g12 pre/post p50 (µs) | g12÷g34 |
|---|---:|---:|---:|
| A (T=32 N=0) | 0.41 | 0.84 | 2.05× |
| B (T=16 N=4) | 0.39 | 1.09 | 2.79× |
| C (T=64 N=4) | 0.54 | 0.79 | 1.46× |
| D (T=8 N=0)  | 0.32 | 0.76 | 2.38× |

p99 latencies match g34 within noise (A: 145 vs 147 µs, B: 25.3 vs 27.5 µs)
— the long-tail receiver-side wait is identical because it's bounded by
single-receiver CPU-time, which is the same on both clusters. But p50
(common-case latency) is ~2× worse on g1/g2, exactly tracking the halved
BW. Together with the BW probes this fully attributes the ~50%
throughput gap to PCIe link rate.

### Verdict

**4×Micron 512 GB ≡ 2×Samsung 256 GB (within ±5%) at every cell.**
Doubling module count and switching DDR4→DDR5 produces statistically
zero throughput change.

Cross-checks confirm:

| Probe | pre-remap (2×Sam) | post-remap (4×Mic) | Δ |
|---|---:|---:|---:|
| `cxl_bw_probe` 1-thread, 8 GiB×3, g1 | 6.55 GB/s¹ | 6.57 GB/s | +0.3% |
| `cxl_bw_probe` 16-thread parallel aggregate, g1 | 26.6 GB/s¹ | 26.4 GB/s | −0.8% |
| YCSB xhost_read median (4-cell avg) | 1.567 Mops | 1.562 Mops | −0.3% |

¹ pre-remap BW probe values from RCA v3 doc.

### Bottleneck attribution

The switch **DSP port G5x8 is the hard ceiling**, not module count
or memory type:

- BW per host caps at ~26 GB/s regardless of what's behind the port.
  This is the x8 G5 lane-rate (~32 GB/s theoretical) minus protocol
  overhead.
- YCSB throughput ratio (g1/g2 ÷ g3/g4 historical) = **52–55%** ↔
  raw BW ratio (26.4 ÷ 51.78 mlc) = **51%**. The two match within
  noise → xhost_read is BW-bound and the BW ceiling tracks the
  PCIe link rate, not the device-side memory parallelism.
- The "4 modules give more parallelism" hypothesis is FALSIFIED:
  even at high T (cell C: T=64 N=4, max parallelism) where multi-bank
  parallelism would help most, throughput is unchanged.

---

## 2026-05-31 (afternoon) — Same 4×Micron remapped back to g3/g4

**What changed**: user remapped the same 4×Micron 128 GB DDR4 modules
from g1/g2 back to g3/g4 via switch poolmap. dax0.0 on g3/g4 now
reports `size: 549755813888` (= 512 GiB ✓). Goal: test whether the
historical g3/g4 51.78 GB/s baseline returns with these modules on
that host pair (= switch-port-specific hypothesis), or whether the
~26 GB/s ceiling follows the modules (= module/card-specific
hypothesis).

Smoke setup identical to the g1/g2 post-remap run (same `build-cxl-w1-v1024`,
same env, same 4-cell × 3-rep matrix). Script:
[scripts/iter20A_g34_4micron_smoke.sh](../../scripts/iter20A_g34_4micron_smoke.sh).
Raw: [docs/iter20A_g34_4micron_smoke_20260531_094515/](../iter20A_g34_4micron_smoke_20260531_094515/).

### Per-cell 3-rep results — four-platform side-by-side

All throughput in Mops; same 4 cells, same workload.

| Cell | rep | g34 hist | g12 pre (2×Sam) | g12 post (4×Mic) | **g34 now (4×Mic)** | vs hist |
|------|----:|----:|----:|----:|----:|----:|
| **A** (T=32 N=0) | 1 | 1.180 | 0.625 | 0.639 | **0.650** | 55.1% |
|                  | 2 | 1.172 | 0.637 | 0.636 | **0.640** | 54.6% |
|                  | 3 | 1.052 | 0.623 | 0.628 | **0.647** | 61.5% |
|                  | **median** | **1.172** | **0.634** | **0.636** | **0.647** | **55.2%** |
| **B** (T=16 N=4) | 1 | 4.066 | 2.132 | 2.147 | **2.140** | 52.6% |
|                  | 2 | 4.271 | 2.138 | 2.172 | **2.142** | 50.2% |
|                  | 3 | 4.064 | 2.304 | 2.160 | **2.157** | 53.1% |
|                  | **median** | **4.066** | **2.278** | **2.160** | **2.142** | **52.7%** |
| **C** (T=64 N=4) | 1 | 4.952 | 2.943 | 2.538 | **2.842** | 57.4% |
|                  | 2 | 5.203 | 2.648 | 2.872 | **2.751** | 52.9% |
|                  | 3 | 5.897 | 2.777 | 2.893 | **2.917** | 49.5% |
|                  | **median** | **5.203** | **2.777** | **2.872** | **2.842** | **54.6%** |
| **D** (T=8 N=0)  | 1 | 1.163 | 0.568 | 0.585 | **0.587** | 50.5% |
|                  | 2 | 1.108 | 0.579 | 0.573 | **0.583** | 52.6% |
|                  | 3 | 1.086 | 0.578 | 0.581 | **0.591** | 54.4% |
|                  | **median** | **1.108** | **0.578** | **0.581** | **0.587** | **53.0%** |

**g34 now (4×Micron) 4-cell median ratio vs hist**: 52.7 – 55.2%
(geomean **53.9%**) — **identical to g12 post (53.7%)**.

### BW probe cross-check on g3/g4 (4×Micron)

| Probe | g34 hist mlc¹ | g12 (4×Mic) | **g34 now (4×Mic)** | now÷hist |
|---|---:|---:|---:|---:|
| 1-thread, 8 GiB×3 | — | 6.57 GB/s | 6.55 / 6.58 GB/s² | — |
| 16-thread aggregate | — | 26.4 GB/s | 16 × 1.65 = **26.4 GB/s** | — |
| mlc DRAM (CXL) reference¹ | **51.78 GB/s** | — | — | **51.0%** |

¹ historical g34 mlc value from CLAUDE.md; not re-measured today.
² g3 / g4 respectively.

### Verdict — module hypothesis WINS, host hypothesis FALSIFIED

The ~26 GB/s ceiling **travels with the 4×Micron modules** —
**not** with the g1/g2 hosts. With the same 4×Micron now on g3/g4:

- YCSB 4-cell ratio vs g34 hist: 53.9% geomean (vs 53.7% on g12).
  Δ across the two hosts = 0.2 pp = **statistically zero**.
- BW probe aggregate: 26.4 GB/s on both g3/g4 and g1/g2.
- per-rep noise is the only source of variation (max ±5%).

The earlier hypothesis from
[RCA_pcie_link_width_v3.md](../iter18A_g1g2_smoke_20260529_082015/RCA_pcie_link_width_v3.md)
("g1/g2 switch port is G5x8") is **falsified as the unique cause**:
g3/g4's switch port also caps at ~26 GB/s **when these specific
Micron cards are behind it**. Either:

1. The **Micron cards themselves negotiate at G5x8** regardless of
   switch port capability (likely if the cards' LnkCap is x8) — in
   which case the historical 51.78 GB/s was delivered by the
   previous Samsung+Micron mix where Samsung cards held x16.
2. **Both g3/g4 and g1/g2 switch ports are G5x8** (fleet-wide) — in
   which case the historical 51.78 GB/s was a measurement artifact
   from a different load pattern (mlc seq) that aggregated across
   both Samsung DDR5 cards' parallel x8 channels rather than being
   single-card BW.

Hypothesis #1 is more consistent with the LnkCap evidence collected
in RCA v3 (Micron cards reported LnkCap=x8 there). Confirming
requires: read LnkSta/LnkCap on g3/g4's CXL endpoints now that
4×Micron are attached, and verify they show x8 (not x16). Listed
as iter-20A follow-up.

### What the historical g34 51.78 GB/s probably was

The pre-reshuffle g3/g4 configuration had Samsung DDR5 modules
included. If those Samsung modules negotiate at G5x16 (or sit on
ports that route them in parallel through multiple x8 paths) while
the Micron DDR4 modules are stuck at x8, the mlc seq-read across
the full pool would aggregate the two channels and report
~52 GB/s. The current 4×Micron-only pool shows the per-card x8
ceiling — even though there are 4 cards, they share the same
endpoint→switch path that caps at x8 aggregate (or the BW probe's
sequential-load pattern doesn't span enough cards in parallel to
expose multi-card aggregate).

This is consistent with what we observed: BW probe on 4×Micron
behaves identically on g3/g4 and g1/g2, confirming the modules'
ceiling is host-independent.

### Calibration vector — closed

The 4-cell vector (A/B/C/D) is now confirmed as a stable BW-bound
probe that gives:

- 4×Micron pool → ~0.6 / 2.2 / 2.9 / 0.6 Mops (53% of hist), on
  either g3/g4 or g1/g2.
- Historical g34 (Samsung+Micron mix) → 1.18 / 4.07 / 5.20 / 1.11
  Mops.

Any future configuration change can use this 4-cell smoke (~6 min
total) as the calibration gate. Numbers ~1.0×, ~3.5×, ~4.5×,
~1.0× would indicate full x16 BW restored; numbers at current
levels mean still BW-halved.

---

## 2026-05-31 (late) — Stride sensitivity REVEALS the actual ceiling

The earlier conclusion ("~26 GB/s is the hardware ceiling") was
**WRONG**. It was a measurement artifact of access pattern footprint,
not a hardware limit. Spread-pattern BW probes reveal the 4×Micron
pool can deliver close to the historical 51.78 GB/s — when the
access pattern covers the full 512 GiB region rather than
concentrating in the first ~32 GiB.

### Stride sweep on g3/g4 (4×Micron, all reading 1-2 GiB per thread)

| # | Threads | Per-thread region | Stride | Total footprint | Σ BW |
|---|---:|---|---|---|---:|
| 1 | 32 | 1 GiB | 1 GiB | first 32 GiB | 26.71 GB/s |
| 2 | 32 | 1 GiB | 16 GiB | **full 512 GiB** | **53.28 GB/s** |
| 3 | 16 | 2 GiB | 4 GiB | first 64 GiB | 26.45 GB/s |
| 4 | 8 | 2 GiB | 64 GiB | **full 512 GiB** | 45.17 GB/s |
| 5 | 4 | 2 GiB | 128 GiB | one per 128 GiB segment | 25.76 GB/s |

Hosts g3 / g4 give the same per-pattern numbers within ±1%.

### What this reveals

**The 4 Micron cards expose ≥53 GB/s aggregate** when threads spread
across the full pool. This matches the historical 51.78 GB/s mlc
number to within probe noise — so the "module-side BW capability"
hypothesis (these specific cards cap at G3 x8 = 8 GB/s each, total
32 GB/s) is **falsified**. The cards collectively can deliver more
than that.

The actual ceiling structure looks like:

- **Per single-issue stream**: ~6.5 GB/s (limited by CPU-side
  load-issue rate against CXL ~600 ns latency, fan-out by hardware
  prefetcher).
- **Per ~128 GiB region**: ~6.5 GB/s independent of thread count.
  Tests 3 & 4: same per-region BW reached at very different thread
  counts; sum scales with how many distinct 128-GiB regions are
  touched, not with thread count alone.
- **Per pool (full 512 GiB spread)**: ~53 GB/s, with ≥8 well-spread
  threads.

This is the signature of a **coarse memory interleave** at roughly
128 GiB granularity = 1 region per Micron card. Threads concentrated
in one 128-GiB region serialize on that card; threads spread across
all 4 regions parallelize the cards.

### Why the user's intuition was right

User asked: "could this be related to interleaving — historically
8 modules at 4 per host pair, now only 6 modules with one pair
getting 256 GB + the other 512 GB?"

Yes. Historically with **more modules in the chassis pool**, the
HDM range likely interleaved at **finer granularity** (or across
**more channels** = more cards in the stripe). When XConn poolmap
pools N cards into one HDM range with fine interleave, even a
narrow-footprint access stripes across all N cards in parallel and
sees N × per-card-BW immediately. With only 4 cards remaining and
a coarse 128-GiB-per-card layout, the same narrow access hits
**one card** and serializes.

The current dax0.0 layout (on either g3/g4 or g1/g2, both showing
the same pattern) gives 4 separate 128-GiB regions = 4 cards
end-on-end, not interleaved. mlc seq-read, which sweeps long ranges,
naturally crosses all 4 cards → reports the aggregate 51-53 GB/s.
A focused workload (like FUSEE's hot-key cache_pool 524288 entries
@ 1088 B = ~568 MiB working set sitting at the start of the dax
region) reads from **one card only** → hits the 6.5 GB/s
single-card ceiling × ~ working set ratio.

This is consistent with:
- 1-thread sequential 8 GiB at offset 0 = 6.55 GB/s (1 card)
- 4-thread parallel each in its own 128-GiB segment = 25.76 GB/s
  (4 × 6.5)
- 32-thread spread across full pool = 53.28 GB/s (≈ 8 × 6.5 —
  exceeds 4 × per-card because per-card concurrency saturates the
  card's internal queue, exceeding the steady-state 6.5 GB/s)

### Implication for the iter-18A throughput regression

FUSEE's xhost_read working set probably falls inside **one Micron
card's 128 GiB region** in the current layout. That's why the
4-cell smoke gives ~53% of historical — the FUSEE pattern is
single-card-bound, while mlc seq sees the full pool aggregate.

Historically (8-module pool, finer interleave), the same FUSEE
working set would have spanned **all 8 cards via fine-grained
interleave** and seen close to 8 × per-card BW = the historical
51.78 GB/s. Throughput scaled proportionally.

### Possible fixes (iter-20A backlog)

1. **Re-configure HDM with fine-grain interleave across all 4 cards.**
   XConn poolmap may let you specify interleave granularity (256 B
   / 4 KB) rather than per-card 128 GiB chunks. With fine
   interleave + 4 cards, a narrow FUSEE access would stripe across
   all 4 cards. Expected: 4 × 6.5 = 26 GB/s effective at FUSEE
   pattern (vs current ~6.5 effective). Throughput → ~50% of
   historical at FUSEE-level — still BW-halved vs historical
   8-module config but ~2× current.
2. **Place FUSEE arenas at staggered offsets across all 128-GiB
   regions.** Spread the hashtable / cache_pool / staging across
   the 4 segments rather than packing at offset 0. This is a
   software-side workaround for the coarse interleave. Same
   expected outcome as (1).
3. **Wait for 8 modules to return + reconfigure 8-way interleave**
   to fully restore historical BW. Real path back to 1.18 / 4.07
   / 5.20 / 1.11 Mops.

The earlier RCA conclusions about LnkCap / G5x8 / switch port are
**partially wrong**: link width matters as an upper bound but is
not currently the binding constraint; the coarse interleave layout
is.

### Diagnostic to verify interleave structure

Read XConn poolmap (BMC) to confirm:
- How many ports map into dax0.0's HDM range
- Interleave granularity (likely 128 GiB right now; should be 256 B
  or smaller for fine interleave)

Without poolmap visibility, the BW stride sweep above is the
strongest behavioral evidence: ~6.5 GB/s per single-issue stream
× per-card serialization ↔ no fine interleave today.
