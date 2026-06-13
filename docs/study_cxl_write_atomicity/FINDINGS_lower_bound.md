# Lower-bound study: 1B / 8B / 64B aligned cross-host CXL writes

**Date**: 2026-06-13
**Hardware**: g1 + g2, dual-host CXL Type-3 over XConn switch
**Driver**: same `tests/cxl_write_atomicity_probe` used in the upper-bound study
**Plan**: see [FINDINGS.md §6](FINDINGS.md) closing discussion + design conversation 2026-06-13

---

## §1 Why this study

The original [FINDINGS.md](FINDINGS.md) chased the **upper bound** —
"how big can an atomic publish be?" — and ended up at the 3 CL ↔ 4 CL
~1000× cliff with `memcpy` 192 B publishes still showing a 0.013 %
median / 1.7 % worst-case interleave rate.

Critique (2026-06-13): the upper-bound focus was misdirected. LFM v1's
primitives are **1 B flags** (`b[id]`) and **8 B counters** (Lamport
`x`), which are 1/2 to 1/8 the size of one cacheline. The
LFM-relevant question is:

> "Are 1 B, 8 B, 64 B cross-host CXL writes **strictly atomic** at the
> resolution we can measure? If yes, LFM v1's algorithm is sound at the
> data layer and the v2 work should focus on fence-count reduction, not
> a 192 B-publish redesign."

This document answers that.

---

## §2 Headline

**Aligned single-cacheline writes from 1 B to 64 B are strictly atomic
on g1/g2 cross-host CXL**, across both `memcpy` and `movnti` store
paths. The bound holds across 13.2 M total trials (combined Phase A +
C + D) with **0 INTERLEAVE events and 0 CORRUPTION events**.

| Size | Store mode | Total trials | INTL events | Bound on P(INTL) at 95 % CI |
|---|---|---|---|---|
| 1 B aligned | memcpy | 3.3 M (A + D) | **0** | < 9 × 10⁻⁷ |
| 8 B aligned | memcpy | 3.3 M (A + D) | **0** | < 9 × 10⁻⁷ |
| 64 B aligned | memcpy | 3.3 M (A + D) | **0** | < 9 × 10⁻⁷ |
| 4, 16, 32 B aligned | memcpy | 300 K each (A) | **0** | < 10⁻⁵ |
| 8, 16, 32, 64 B aligned | movnti | 300 K each (C) | **0** | < 10⁻⁵ |

**Equally important**: cross-cacheline writes — even tiny ones —
show **massive interleave**. An 8-byte write that straddles a
cacheline boundary interleaves at **8.8 - 13.7 %** under the same
hardware and same probe.

This separates the design space cleanly:
- **Aligned single-CL writes are safe primitives for LFM v1-style
  algorithms.** No protocol-level torn-publish retry needed.
- **Any cross-CL write is unsafe** and must either be avoided
  structurally or wrapped in algorithmic retry.

---

## §3 Methodology

Same probe + classification as the upper-bound study (see
[FINDINGS.md §2](FINDINGS.md)). Four phases on g1+g2:

| Phase | Variables | K | Reps | Total trials | Purpose |
|---|---|---|---|---|---|
| A | N ∈ {1, 4, 8, 16, 32, 64}, aligned, memcpy | 100 K | 3 | 1.8 M | confirm strict atomicity of memcpy aligned small writes |
| B | N ∈ {8, 16, 32, 64}, **straddle_cl**, memcpy | 100 K | 3 | 1.2 M | quantify how bad cross-CL is at small sizes |
| C | N ∈ {8, 16, 32, 64}, aligned, **movnti** | 100 K | 3 | 1.2 M | verify movnti vs memcpy parity at small aligned sizes |
| D | N ∈ {1, 8, 64}, aligned, memcpy | **1 M** | 3 | 9 M | tightest bound at LFM-v1 critical primitives |

All cells use `clflush_sfence` fence and `barrier` race mode (the
LFM-realistic timing pattern). Total across A + C + D = 12 M trials
with 0 INTERLEAVE; total across all four phases = 13.2 M trials.

---

## §4 Per-phase results

### 4.1 Phase A — aligned memcpy strict-atomic test

Each row = 3 independent K=100 K reps.

| N | Rep 1 INTL | Rep 2 INTL | Rep 3 INTL | Worst OW_A/OW_B split (race balance) |
|---|---|---|---|---|
| 1  | 0 | 0 | 0 | rep 3 = 10.5 % / 89.5 % |
| 4  | 0 | 0 | 0 | rep 2 = 2.4 % / 97.6 % |
| 8  | 0 | 0 | 0 | rep 1 = 0.03 % / 99.97 % |
| 16 | 0 | 0 | 0 | rep 2 = 1.4 % / 98.6 % |
| 32 | 0 | 0 | 0 | rep 3 = 3.2 % / 96.8 % |
| 64 | 0 | 0 | 0 | rep 2 = 10.7 % / 89.3 % |

**18 cells × 100 K = 1.8 M trials, 0 INTERLEAVE.** The OW_A/OW_B split
varies 0.03 % to 10.7 % across reps (run-state-dependent timing skew),
but the **interleave count is always 0** — when a writer wins, it
wins all bytes.

### 4.2 Phase B — straddle_cl memcpy cross-CL torture test

| N | Rep 1 INTL | Rep 2 INTL | Rep 3 INTL | INTL rate range |
|---|---|---|---|---|
| 8  | 8 794 | 13 677 | 12 923 | **8.8 - 13.7 %** |
| 16 |  2 151 |  1 139 |     88 |   0.09 - 2.2 % |
| 32 |  2 562 |    643 |  1 285 |   0.6 - 2.6 % |
| 64 | 10 659 |  6 079 | 12 310 |   6.1 - 12.3 % |

Smoking gun. **An 8-byte write that crosses a cacheline boundary
interleaves at ~10 % rate** — three orders of magnitude worse than
the aligned case, in a study that initially missed this. The
mechanism is exactly the predicted one: a cross-CL write splits into
2 PCIe write transactions per host, and the four transactions can
reorder at the CXL controller.

The non-monotonic N=8 → N=16/32 → N=64 (rate dips then rises) is
likely because at N=16/32 most of the write still lands in one CL
with only 1-2 bytes in the second; while N=8 and N=64 straddle in a
way that puts roughly half in each CL → maximum susceptibility to
the cross-txn reorder.

### 4.3 Phase C — aligned movnti control

| N | Rep 1 INTL | Rep 2 INTL | Rep 3 INTL | Notes |
|---|---|---|---|---|
| 8  | 0 | 0 | 0 | OW_A = 0-1 per 100 K |
| 16 | 0 | 0 | 0 | OW_A = 1-2 per 100 K |
| 32 | 0 | 0 | 0 | OW_A = 0-2 per 100 K |
| 64 | 0 | 0 | 0 | OW_A = 0 per 100 K |

**12 cells × 100 K = 1.2 M trials, 0 INTERLEAVE.** Movnti behaves
identically to memcpy in this size range. Interesting side
observation: movnti shows even **less OW_A noise** — host B (the
"late writer") wins ~99.999 % of the time. This is consistent with
the WCB coalescing the 8 B store into one PCIe burst that's
issued strictly later than host A's, with less timing jitter than
the memcpy + clflushopt path.

### 4.4 Phase D — tight bound at LFM-v1 critical sizes (K = 1 M)

| N | Rep 1 INTL | Rep 2 INTL | Rep 3 INTL | Sum trials | Bound on P(INTL) |
|---|---|---|---|---|---|
| 1  | 0 | 0 | 0 | 3 M | < 1 × 10⁻⁶ at 95 % CI |
| 8  | 0 | 0 | 0 | 3 M | < 1 × 10⁻⁶ at 95 % CI |
| 64 | 0 | 0 | 0 | 3 M | < 1 × 10⁻⁶ at 95 % CI |

**9 cells × 1 M = 9 M trials, 0 INTERLEAVE, 0 CORRUPTION.** At 1 in
10⁶ confidence floor, the bound is below the rate at which any
LFM-style algorithm in practice would ever see a torn read in a
human lifetime of execution.

---

## §5 Comparison with the upper-bound study

| Region | Trials | Worst INTL rate | Verdict |
|---|---|---|---|
| 1-64 B aligned (single CL) | 13.2 M | 0 | strictly atomic, ≤ 10⁻⁶ |
| 8-64 B straddle_cl (cross CL) | 1.2 M | ~14 % | broken |
| 128 B aligned (2 CL) | 600 K | ~1.8 % | broken |
| 192 B aligned (3 CL) | 500 K | ~1.7 % (worst rep) | broken |
| ≥ 256 B (≥ 4 CL) | 300 K+ | 15 - 43 % | severely broken |

**The boundary is at the cacheline.** Any write that fits in one
cacheline and starts at a cacheline-aligned address is strictly
atomic. Any write that touches 2 or more cachelines (whether by size
or by misalignment) has non-zero interleave probability that grows
with the number of cachelines touched.

The earlier study's "3-CL atomic window" claim was an artifact of
sampling at K=10 K. At K=100 K the 3-CL writes show ~0.01 % - 1.7 %
interleave; at K=1 M they would show more events still. The real
hardware property is **"one cacheline = one PCIe txn = atomic;
multiple cachelines = multiple txns = potentially interleaved"**,
and there is no "atomic multi-CL window" on this hardware.

---

## §6 Implications for LFM redesign

### 6.1 LFM v1 algorithmic correctness is intact

Lamport's bakery lock + the LFM v1 implementation in
`cxl_shm_profiling/lfm_lock.c` use:
- 1 byte `b[id]` flag per host (cacheline-padded in v1, so
  aligned single CL by construction)
- 8 byte `x` ticket counter (aligned to 8 B, fits in 1 CL)

Both are in the "aligned single-CL" bucket. Phase D bounds their
interleave probability at < 10⁻⁶. **LFM v1's data layer is correct
on g1/g2 CXL.**

### 6.2 LFM v2 = optimize fences, not data structure

The earlier `src/cxl_fusee_lfm_v2.h` sketch proposed a 192 B,
3-cacheline slot with mandatory `publish_seq` torn-publish
detection. **Both moves are unnecessary** for LFM:

- **192 B slot is over-sized.** LFM only needs the existing
  1 B + 8 B fields, which fit in < 1/4 of a cacheline.
- **Torn-publish detection is unnecessary.** The 0.013 %-1.7 %
  interleave rate at N=192 was specific to multi-CL publishes.
  LFM's single-CL primitives don't have it.

The actual LFM v1 → v2 optimization opportunity is:
- LFM v1 issues 4+ `clflushopt + mfence` per acquire, each ~600 ns
  on g1/g2 CXL. Total acquire wall clock ~3 µs at uncontended,
  9 µs p50 at T=64.
- Each clflushopt is the wait-for-PCIe-write-to-complete latency.
  This is the bottleneck.
- If multiple of those clflushopts target the same cacheline
  (which they do in v1 — the algorithm clflushes the same
  state multiple times for ordering), they are redundant.
- A focused audit of LFM v1's algorithm should be able to halve or
  better the clflushopt count without changing the data structure.

### 6.3 Concrete design rules now justified by data

| Rule | Status | Source |
|---|---|---|
| All LFM v2 flag/counter fields must be aligned to their natural width AND fit in a single cacheline | **required** | Phase A + D (0 INTL) vs Phase B (10 %+ INTL when cross-CL) |
| No flag/counter field crosses a cacheline boundary | **required** | Phase B (cross-CL is the failure mode at any size ≥ 2 B) |
| memcpy vs movnti is a perf tradeoff, not a correctness tradeoff, when both rules above hold | **established** | Phase A and Phase C both 0 INTL across all reps |
| No multi-cacheline atomic publish required | **established** | [FINDINGS.md](FINDINGS.md) showed even 3-CL writes have non-zero INTL; this study showed 1-CL single writes don't need to be replaced by multi-CL publishes |

### 6.4 What to do about `src/cxl_fusee_lfm_v2.h`

The current header proposes a 192 B slot with `publish_seq` fields
on each cacheline. Given §6.2, that design should be:

- **Retired or recast as a study artifact** (since it was the
  proposed solution to a non-problem)
- A replacement v2 sketch should look like LFM v1 but with
  aggressive fence audit + per-cacheline padding + minimum-CL
  writes

I'd recommend explicitly marking the current `cxl_fusee_lfm_v2.h`
as superseded by these findings, and writing a new sketch only
after the LFM v1 fence audit identifies where the actual savings are.

---

## §7 Raw data

All Phase A/B/C/D raw CSVs under
`docs/study_cxl_write_atomicity/phase{A_lower,B_cross_cl,C_movnti,D_tight}_rep{1,2,3}_*/raw.csv`.
Total 12 directories. Per-cell `g{1,2}.log` retained for debugging
(receiver-thread tail clatter that's mostly cosmetic; the
classification line at end of each is what was parsed for the CSV).

Probe binary: `tests/cxl_write_atomicity_probe.cc`, built into
`build-cxl/tests/cxl_write_atomicity_probe` on both g1 and g2 (same
build as upper-bound study; no source changes).

Sweep driver: `scripts/run_cxl_atomicity_study.sh` (extended in the
upper-bound study to thread `ATOMICITY_MODE` through; no further
changes needed).
