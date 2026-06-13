# Study: CXL cross-host write atomicity granularity

**Status**: drafting (2026-06-13)
**Branch**: feat/cxl-migration
**Hosts**: g1 + g2 (CXL Type-3 via XConn switch, NO CPU coherence)
**Type**: standalone hardware characterization study (NOT iter*A series)

---

## §1 Goal

Characterize the granularity at which concurrent cross-host writes to a
single CXL address are **atomic** (one writer's payload wins entirely) vs
**interleaved** (the final byte pattern mixes bytes from both writers).
Use the result to redesign LFM (Lamport Fast Mutex) for the no-coherence
CXL world.

Concretely, partition write-unit sizes into three buckets:

- **Overwrite-only**: one writer's payload always wins atomically. Safe
  for any mutex flag/cookie at this granularity.
- **Could-go-either-way**: sometimes overwrite, sometimes interleave —
  depends on store-instruction split / PCIe transaction boundaries.
  Unsafe for LFM flag fields.
- **Interleave-possible**: byte-level mixing observed. Useless for any
  consensus primitive without a higher-level guard.

LFM v2 design target: the largest **overwrite-only** unit becomes the new
"flag" primitive. If only 8 bytes are overwrite-only, LFM v2 still works
but with reduced state per slot. If only 1 byte is, LFM v2 needs entirely
different scaffolding.

## §2 Motivation — why LFM v2

Current LFM (cxl_shm_profiling/lfm_lock.c, ported through
src/cxl_fusee_slot_lock.cc) was designed for shared-memory + coherent
CPU caches. On CXL g1/g2 it relies on `clflushopt + mfence` + careful
ordering — every `b[id] = 1` write costs ~600 ns minimum (cache flush
+ peer read-back), and the algorithm does ≥4 of those per lock/unlock.
Measured: contended LFM @ T=64 p50 = 9 µs, p99 = 691 µs
(docs/iter9A_redo_path_decomp_*/Phase 0 baseline.md), or
~50 % of every UPDATE op's latency in Protocol A.

If we know e.g. "any 16-byte store is overwrite-only across hosts on
g1/g2", a flag-cell can be a 16-byte struct flipped with a single
movdqa + sfence — no clflushopt back-and-forth, no per-flag mfence,
and Lamport's algorithm collapses to ~1 cacheline of CXL traffic per
lock.

## §3 Methodology

### 3.1 Distinguishing interleave from overwrite

Each host writes a **rank-tagged byte pattern** into the contested cell.
For unit size N:

```
g1 pattern: [0xA0, 0xA1, 0xA2, ..., 0xA(N-1)]   // top nibble 1010
g2 pattern: [0xB0, 0xB1, 0xB2, ..., 0xB(N-1)]   // top nibble 1011
```

After both writes complete (with a post-write barrier), a third reader
or one of the hosts reads N bytes and classifies:

- All N bytes have top nibble `1010` → **g1 won** (overwrite)
- All N bytes have top nibble `1011` → **g2 won** (overwrite)
- Mixed top-nibble pattern → **interleave**
- Anything else (e.g., low nibble doesn't match index) → **corruption**
  (would flag a deeper bug; expected: 0 occurrences)

This works for any N up to a few KB without ambiguity, and the low-nibble
index check catches partial-update corruption that might masquerade as
overwrite (e.g., if a peer's partial cacheline merge happens to match
your top nibble by accident).

### 3.2 Synchronization

Both hosts:
1. Attach to the same CXL region with a shared header.
2. **Phase barrier #1**: spin on `go_round[i]` flag. Host 0 sets
   `go_round[i] = i+1` to kick off round i; both hosts observe and race
   to write.
3. **Phase barrier #2**: post-write, each host sets `done_round[i][host_id]`.
   Host 0 (also the reader) waits for both bits, then reads the cell and
   classifies the outcome.
4. Loop K rounds (e.g., 1000) per (size, alignment, flush_mode) cell.

The barrier itself must use a 8-byte field known to be atomic on x86
(CXL store of an 8-byte uint64_t is itself a question, but on g1/g2 the
existing fetch_add cross-host test passes for 8-byte counters, so this
is the working assumption — we'll spot-check it in Phase 0).

### 3.3 Variables to sweep

| Variable | Values | Rationale |
|---|---|---|
| **N** (unit size) | 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024 bytes | CPU-atomic widths through multi-cacheline |
| **Alignment** | N-aligned vs N-1-misaligned vs cacheline-straddling | Cacheline boundaries change PCIe transaction count |
| **Store mode** | `memcpy` / vector store (`movdqa` for 16 B) / `movnti` non-temporal | Different instruction sequences = different PCIe behavior |
| **Post-write** | none / sfence / clflushopt+sfence | Whether reordering is the source of interleave |
| **Cell location** | same cacheline / cross-cacheline / 4 KB-page-boundary-straddling | PCIe controller might batch within a page |

### 3.4 Per-cell trial count

1000 trials per cell × ≥ 30 cells = 30 K trials. Per-trial cost ≈ barrier
+ two writes + one read ≈ a few µs. Whole sweep ≤ 5 min.

### 3.5 Pass / outcome classification

For each `(N, alignment, store_mode, post_write, location)` cell, report:

| Metric | Definition |
|---|---|
| `P_overwrite_A` | # trials g1 won / total |
| `P_overwrite_B` | # trials g2 won / total |
| `P_interleave` | # trials with mixed top-nibble / total |
| `P_corruption` | # trials failing low-nibble index check / total (should be 0) |
| `bucket` | "overwrite-only" if P_interleave = 0 AND P_corruption = 0; "could-go-either-way" if 0 < P_interleave < 1; "interleave-possible" if P_interleave ≥ X% (threshold TBD) |

The "could-go-either-way" bucket needs sufficient samples that we can
say "in 30 K trials we saw 0 interleave" — that gives ~99.9% confidence
the interleave probability is < 1e-4. Lower than that we can't bound
with this trial count; need 1 M+ trials per cell for stronger bounds,
done only on the boundary sizes.

## §4 Open design questions for you

These are the choices I want your call on before writing code:

### Q1: barrier primitive
The phase barrier itself uses 8-byte CXL atomic stores. Existing
`cxl_atomic_xhost.cc` validates fetch_add atomicity — sufficient
evidence to use a uint64_t barrier counter. Confirm OK or want a
finer-grained spot-check first?

### Q2: scope of sweep
The full matrix is `11 sizes × 3 alignments × 3 store modes ×
3 post-write modes × 3 cell locations = 891 cells × 1000 trials`.
That's ~1 hr testbed time and a lot of post-processing. Want me to:
- (a) start with N × {aligned} × {memcpy} × {sfence} × {same-cacheline}
  = 11 cells, classify first, then widen toward the interesting boundary
  (probably 8 → 64 byte range)
- (b) just run the full 891-cell matrix once and chart it out
- (c) something in between

### Q3: when interleave is "rare but nonzero"
A unit might have `P_interleave = 0.001` (1 in 1000) — useless for
mutex flags but might still be a stable "almost-atomic" primitive
worth knowing about. Where do you want the line drawn between
"could-go-either-way" and "interleave-possible"?
- Strict: >0 interleave events → could-go-either-way
- Practical: < 1e-6 → overwrite-only-practical; 1e-6 to 1e-3 →
  could-go-either-way; ≥ 1e-3 → interleave-possible

### Q4: cross-cacheline / multi-cacheline write semantics
For N > 64 bytes the write necessarily spans cachelines. Do you want
to classify the FULL N bytes (likely always interleave) or also
report the per-cacheline outcome (each cacheline might still be
overwrite-only)? The latter is more useful for LFM v2 design.

### Q5: outcome deliverable
- (a) raw CSV + heatmap PNG + writeup in PLAN.md
- (b) above + a prototype LFM v2 sketch in src/cxl_fusee_lfm_v2.{c,h}
- (c) above + actual LFM v2 implementation drop-in for cxl_fusee_slot_lock.cc

If you pick (b) or (c), the design depends on Phase 1 results so it's a
two-phase deliverable.

## §5 Proposed phase plan

(Pending answers to §4.)

| Phase | What | Output |
|---|---|---|
| 0 | Sanity: 8-byte CXL store/load + 8-byte fetch_add atomicity spot-check on g1/g2 | go / no-go on §3.2 barrier |
| 1 | Test binary `tests/cxl_write_atomicity_probe.cc` + sweep script. Run the agreed-on cell matrix | docs/study_cxl_write_atomicity/raw/*.csv |
| 2 | Analyze + plot: P_overwrite / P_interleave vs N, faceted by alignment + store mode | docs/study_cxl_write_atomicity/atomicity_chart.png + table.md |
| 3 | Bucket classification + LFM v2 design implication writeup | docs/study_cxl_write_atomicity/FINDINGS.md |
| 4 (optional) | LFM v2 prototype | src/cxl_fusee_lfm_v2.{c,h} + microbench vs LFM v1 |

## §6 Known unknowns

- **CXL Type-3 controller buffering**: the XConn switch might coalesce
  PCIe transactions in ways that make atomicity look better than the
  underlying spec promises. Results are specific to g1/g2 hardware
  generation.
- **Write order on the wire**: even if both hosts issue stores
  simultaneously from their CPU pipelines, the order they arrive at
  the CXL controller depends on uncontrollable bus arbitration. We
  measure the OUTCOME distribution, not a particular order.
- **CPU store-buffer flush**: without `sfence` between the two writes
  on the same host, the writes can reorder. We test with and without
  sfence to expose this.

---

## §7 Locked decisions (2026-06-13 design discussion)

| § | Choice |
|---|---|
| Q1 barrier | uint64_t CXL atomic, validate in Phase 0 spot-check (assumed OK based on `cxl_atomic_xhost.cc` precedent) |
| Q2 sweep scope | **Phase 1 = 11-cell baseline** (N ∈ {1,2,4,8,16,32,64,128,256,512,1024} × aligned × memcpy × sfence × same-cacheline). Phase 2 widens at boundaries (the N where 0 < interleave < 100 %, OR where alignment/store-mode matters) |
| Q3 interleave threshold | **Strict**: any single interleave event in 1000 trials disqualifies the unit from "overwrite-only". A unit only counts as overwrite-only if **0 interleave AND 0 corruption** observed in the trial budget. |
| Q4 multi-cacheline (N>64) | Report **both** full-N classification AND per-cacheline breakdown. Per-cacheline tells us if LFM v2 can use 64-byte flag-cell with N independent cachelines for N bits of state. |
| Q5 deliverable | FINDINGS.md + LFM v2 **design sketch** in src/cxl_fusee_lfm_v2.h (interface + pseudocode only, no implementation this iter) |

## §8 Locked phase plan

1. **Phase 0** — 1-hour sanity probe:
   - Verify uint64_t CXL store + read across hosts is atomic (g1 writes 0xAA..., g2 reads same).
   - Verify the 11-cell harness scaffold works on a single trivial cell (1-byte write).
2. **Phase 1** — 11-cell baseline + classification table.
   - Binary: `tests/cxl_write_atomicity_probe.cc`
   - Sweep: `scripts/run_cxl_atomicity_study.sh`
   - Output: `docs/study_cxl_write_atomicity/phase1/{raw.csv,table.md}`
3. **Phase 2** — widen at boundary N values found in Phase 1.
   - Output: `docs/study_cxl_write_atomicity/phase2/{raw.csv,table.md,atomicity_chart.png}`
4. **Phase 3** — FINDINGS.md + LFM v2 design sketch.
   - Output: `docs/study_cxl_write_atomicity/FINDINGS.md`
   - Output: `src/cxl_fusee_lfm_v2.h` (interface + pseudocode comments)
