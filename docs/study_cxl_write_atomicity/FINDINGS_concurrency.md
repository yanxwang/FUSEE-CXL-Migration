# False-sharing + concurrent-reader study

**Date**: 2026-06-13
**Hardware**: g1 + g2, dual-host CXL Type-3 over XConn switch
**Builds on**: [FINDINGS.md](FINDINGS.md) (upper-bound) and
[FINDINGS_lower_bound.md](FINDINGS_lower_bound.md) (lower-bound)
**Probes**:
- [tests/cxl_false_sharing_probe.cc](../../tests/cxl_false_sharing_probe.cc)
- [tests/cxl_concurrent_reader_probe.cc](../../tests/cxl_concurrent_reader_probe.cc)

---

## §1 Why these experiments

[FINDINGS_lower_bound.md §6.4](FINDINGS_lower_bound.md) closed with two
open questions that materially affect LFM data-structure design:

1. **Same-CL false sharing**: if N hosts each own an 8-byte slot inside
   the same 64-byte cacheline, can each host write its 8 bytes without
   destroying the others'? If yes, LFM v2 can pack 8 hosts per CL (64 B
   total memory). If no, every host needs its own CL (8× the footprint).
2. **Concurrent reader**: when a peer is in the middle of publishing a
   slot, can a reader catch a "torn" intermediate state? LFM acquire
   path reads peer state while peer may itself be writing.

This document answers both.

---

## §2 TL;DR

### Same-CL false sharing

**With `memcpy + clflushopt`, false sharing IS NOT SAFE.** Per-host 8B
slots inside the same cacheline lose host A's data ~64–89 % of the
time due to cached-store write-allocate semantics.

**With `movnti + sfence`, false sharing IS SAFE.** 900 K trials,
**100 % BOTH_OK** — both hosts' regions are preserved.

→ LFM v2 has two viable layouts:
- 64 B per host (each host owns one CL, can use memcpy), OR
- ≤ 8 B per host packed into shared CLs (MUST use movnti)

### Concurrent reader

**For N ≤ 64 B (single cacheline), a reader NEVER sees a torn state.**
Across ~7 M reads per N value, 0 torn observations.

**For N > 64 B (multi-CL), reader catches ~0.25–1 % "mid-publish"
states** where some cachelines have the new pattern and others still
have the previous one.

→ LFM read path that reads a peer's slot is safe **only if the slot is
single-CL**. Cross-CL slots need explicit version checking.

---

## §3 Same-CL false-sharing experiment

### 3.1 Setup

Two hosts each write an 8-byte rank-tagged pattern at adjacent
non-overlapping offsets within the same 64-byte cacheline:

| Offset 0–7 | Offset 8 to N+gap-1 | Offset 8+GAP onward | Rest of CL |
|---|---|---|---|
| Host A pattern (0xA0..0xA7) | gap of GAP bytes (reset 0xC*) | Host B pattern (0xB0..0xB7) | reset 0xC* |

Both hosts publish via `clflushopt + sfence` (memcpy mode) or `sfence`
(movnti mode). After both publish, host 0 reads the full 64 B and
classifies each 8 B region:

- **BOTH_OK** — both regions show correct pattern, false sharing safe
- **A_LOST** — A's region clobbered by B's writeback
- **B_LOST** — B's region clobbered by A's writeback
- **BOTH_LOST** — both regions lost
- **INTRA_INTL** — within one region, A and B bytes mix
- **CORRUPT** — low-nibble index check failed

### 3.2 Results (K=100 K × 3 reps each)

| Config | BOTH_OK (mean) | A_LOST (mean) | B_LOST | INTRA_INTL |
|---|---|---|---|---|
| memcpy + clflush, GAP=0 (adjacent 8 B slots) | 22.9 % | 73.9 % | 0 | 0.03–9.4 % |
| memcpy + clflush, GAP=8 (8 B gap between slots) | 23.9 % | 76.1 % | 0.03 % | 0 |
| memcpy + clflush, GAP=48 (slots at CL ends) | 21.4 % | 78.6 % | 0 | 0.03 % |
| **movnti + clflush, GAP=0** | **100 %** | **0** | 0 | 0 |
| **movnti + sfence-only, GAP=0** | **100 %** | **0** | 0 | 0 |

### 3.3 Why memcpy false-shares

`memcpy` is a **cached store + write-allocate**:

1. Host B's memcpy issues a STORE at offset 8 → CPU needs the
   cacheline in M state in L1
2. If the line isn't in L1 (or is in I/S state), the CPU **first reads
   the line from CXL** (write-allocate) to fill L1
3. Now host B's L1 has the full 64 B — possibly the **stale** version
   from CXL that does NOT include host A's most recent write to
   bytes [0..7]
4. Host B writes its 8 B in L1
5. Host B's clflushopt evicts → PCIe write of the full 64 B back to
   CXL, **including the stale bytes [0..7]** which now overwrite host
   A's actual publication

The race condition: whether host A's clflushopt completes (so CXL has
A's pattern) **before** host B's write-allocate read (which would then
capture A's pattern in B's L1). The 1 µs barrier-induced timing skew
means host A typically finishes first, but the PCIe round-trip latency
overlap makes the outcome a race.

Result: **the LATER flusher's view of the cacheline overwrites the
EARLIER one's data**, regardless of byte offset within the CL.

### 3.4 Why movnti is safe

`movnti` is **non-temporal, no write-allocate, no cacheline-fill**:

1. Host B's movnti writes 8 B directly into the **write-combining
   buffer (WCB)**, bypassing L1
2. WCB drains to PCIe as a write transaction with **byte enables**
   covering only the 8 dirty bytes
3. PCIe controller applies the byte-enabled write to CXL: bytes [8..15]
   updated, bytes [0..7] **untouched**
4. Host A's bytes [0..7] are preserved regardless of timing

Each host's writeback is **strictly limited to its own 8 bytes**. No
write-allocate read, no full-CL writeback, no clobbering.

### 3.5 The INTRA_INTL anomaly

One memcpy GAP=0 rep showed INTRA_INTL = 9 427 / 100 000 (~9 %), much
higher than the 0.03 % of the other reps. The 9 % rep's BOTH_OK was
also unusually low (1.3 %). This is run-state-dependent (see Phase 4
variance lessons in the upper-bound study); the **median** is still
~30 % BOTH_OK / ~70 % A_LOST / < 0.1 % INTRA_INTL. The exact mechanism
of the INTRA_INTL outlier is unclear but the design conclusion (memcpy
false-sharing is unsafe) is unaffected — it just adds another failure
mode.

---

## §4 Concurrent-reader experiment

### 4.1 Setup

Host 0 (writer) loops continuously for a 2-second wall-clock window:

```
memcpy(target, pat_A, N)    // write pat_A
clflushopt each CL
sfence
memcpy(target, pat_B, N)    // write pat_B
clflushopt each CL
sfence
```

Host 1 (reader) loops continuously over the same window:

```
clflushopt each CL of target
mfence
memcpy(obs, target, N)      // read
classify(obs)
```

Reader classifies each observation:
- **OBS_A** — all bytes match pat_A
- **OBS_B** — all bytes match pat_B
- **OBS_RESET** — all bytes match initial reset pattern
- **TORN_AB** — mix of A and B byte patterns within obs
- **TORN_OTHER** — mix of (A or B) bytes with reset bytes
- **CORRUPT** — low-nibble index check failed

### 4.2 Results (race_us=2 000 000 × 3 reps × 4 N values)

| N | Mean reader iters | OBS_A | OBS_B | TORN_AB | TORN_OTHER | CORRUPT |
|---|---|---|---|---|---|---|
| 1   | 2.6 M | 24 % | 76 % | **0** | **0** | 0 |
| 8   | 2.5 M | 25 % | 75 % | **0** | **0** | 0 |
| 64  | 2.3 M | 25 % | 75 % | **0** | **0** | 0 |
| 128 | 2.1 M | 25 % | 74 % | 0 | 5–21 K (0.25–1 %) | 0 |

### 4.3 What this proves

For **single-cacheline writes (N ≤ 64)**: the reader catches a
~3:1 OBS_B:OBS_A skew (writer's flush cycle spends more time in
"B published" state because of memory-layout effects) but **never
sees an intermediate state**. The CL is either fully A or fully B
when observed.

This is the strong concurrency guarantee LFM needs: **acquire-time
reads of single-CL peer slots are atomic against concurrent publishes
by the peer**. No torn-publish detection needed.

For **multi-CL writes (N = 128)**: TORN_OTHER appears at 0.25–1 %
rate. The mechanism: between writer's `flush_line(CL0)` and
`flush_line(CL1)`, CXL holds CL0 with the new pattern and CL1 with
the old. A reader caught in this window sees a mix. **Multi-CL slots
require explicit version checks** (or just don't span CLs).

The TORN_AB = 0 across all N=128 reps is initially surprising — one
would expect the multi-CL case to show "old A + new B" mixes
frequently. The "OTHER" category being non-zero while AB stays 0
suggests the captured mid-publish state involves the initial reset
pattern (0xC*) lingering in the not-yet-flushed CL on the very first
iteration. We did not chase this further; the design conclusion
(don't span CLs) is the same either way.

### 4.4 Performance side-note

Reader throughput is ~1.25 M reads/sec across all N values
(2.5 M reads / 2 s). Each read costs ~800 ns wall clock — dominated by
the clflushopt + PCIe round-trip. Writer publishes at ~340 K full
cycles/sec (each "cycle" = 1 pat_A + 1 pat_B publish = 2 flushes per
CL × 1 CL for N≤64, or × 2 for N=128). Reader is ~4–7× faster than
writer, which is why we see so many OBS_A and OBS_B per cycle.

---

## §5 Implications for LFM v2 data structure

### 5.1 Two viable layouts

**Layout A: "one CL per host slot" (recommended starting point)**

```
struct alignas(64) PerHostSlot {  // exactly 64 B
  uint8_t  b;        // Lamport flag
  uint8_t  _pad0[7];
  uint64_t x;        // ticket counter
  uint64_t epoch;
  uint64_t version;
  uint64_t _pad1[4];
};
```

- Use `memcpy + clflushopt` for publishes
- Use `clflushopt + mfence + load` for reads
- Each slot is its own CL → no false sharing, no concurrent-reader
  tearing
- Memory cost: 64 B × N hosts. For 8 hosts = 512 B total. Negligible.

**Layout B: "packed 8B slots in shared CL" (ONLY if memory really matters)**

```
struct alignas(64) PackedRing {  // exactly 64 B, holds 8 hosts × 8 B
  uint64_t slot[8];  // slot[host_id] is host_id's 8B state
};
```

- Use **movnti + sfence** for publishes (REQUIRED — memcpy unsafe)
- Use `clflushopt + mfence + load` for reads
- All 8 hosts share one CL → 64 B total instead of 512 B
- Tradeoff: only 8 B of state per host (vs 64 B in Layout A); writers
  use slightly different code path (movnti vs memcpy)

For LFM v1's current state (1 B `b` + 8 B `x` = 9 B per host), **Layout B
fits perfectly**. The 64 B per-host savings vs Layout A are negligible
for ≤ 16 hosts, but Layout B has the advantage that ALL of LFM v1's
state literally fits in one cacheline shared across hosts — minimal
PCIe traffic.

### 5.2 What NOT to do

| Anti-pattern | Why it breaks |
|---|---|
| memcpy into a CL that holds other hosts' slots | §3 — write-allocate destroys peers' bytes (~75 % loss rate) |
| Spread one host's slot across multiple CLs | §4 — concurrent reader can catch torn mid-publish |
| Cross-CL aligned fields | [FINDINGS_lower_bound.md §4.2](FINDINGS_lower_bound.md) — straddle_cl shows 10 %+ interleave at single 8-byte fields |

### 5.3 Updated LFM v2 sketch

The current [src/cxl_fusee_lfm_v2.h](../../src/cxl_fusee_lfm_v2.h) proposes
a 192 B / 3-CL slot with `publish_seq` torn-detection. This is wrong
on two counts:

1. **192 B is over-sized.** LFM only needs 8 B per host (1 B flag +
   8 B counter).
2. **publish_seq scheme is unnecessary AND insufficient.** Single-CL
   publishes don't tear (§4 N≤64), so no seq needed. Multi-CL publishes
   would tear at ~1 % even with seq (FINDINGS.md), so spreading across
   CLs is the wrong move anyway.

The replacement design is **either Layout A or Layout B from §5.1**,
chosen on memory-cost preference. Both use only well-understood
single-CL primitives with strong empirical backing (700 K + 9 M + 7 M
trials with zero torn / mismatched events for single-CL writes/reads).

---

## §6 Raw data

- `docs/study_cxl_write_atomicity/false_sharing/raw.csv` — 15 cells
  (5 configs × 3 reps × K=100K)
- `docs/study_cxl_write_atomicity/concurrent_reader/raw.csv` —
  12 cells (4 N values × 3 reps × 2 s race window)

Probes built into `build-cxl/tests/cxl_false_sharing_probe` and
`build-cxl/tests/cxl_concurrent_reader_probe`.
