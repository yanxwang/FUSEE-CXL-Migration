# CXL cross-host write atomicity — learnings (synthesis + hardware primer)

**Date**: 2026-06-13
**Hardware**: g1 + g2, dual-host CXL Type-3 over XConn switch, kernel 6.15.0-uintr
**Studies referenced**:
- [PLAN.md](PLAN.md)
- [FINDINGS.md](FINDINGS.md) — upper-bound (~1000× rate cliff at 3-CL ↔ 4-CL; no strict atomic at any size)
- [FINDINGS_lower_bound.md](FINDINGS_lower_bound.md) — single-CL aligned 1B/4B/8B/16B/32B/64B all strict-atomic at K=10⁶ bound
- [FINDINGS_concurrency.md](FINDINGS_concurrency.md) — false sharing + concurrent reader

This document is the **cross-experiment synthesis + the hardware-mechanism
reference**. It does NOT propose any software-lock or LFM redesign — those
live elsewhere (and may evolve independently). The point of this document
is to capture what we now know about the **hardware** so any future design
can start from a clean reference.

---

## §1 Hardware-mechanism primer

### 1.1 Cacheline

A **cacheline** (CL) is the CPU's atomic unit of cache management. On x86 it
is fixed at **64 bytes**. Every L1/L2/L3 entry is a 64-byte aligned chunk.
`clflushopt` and natural evictions operate at this granularity.

```
addr:   0   1   ... 63 | 64  65  ... 127 | 128 ... 191 | 192 ... 255
        └──── CL 0 ────┘└──── CL 1 ──────┘└── CL 2 ───┘└── CL 3 ────┘
        |←—— 64 bytes ——→ |
```

A cacheline is identified by its base address aligned to 64.

### 1.2 PCIe transaction

A **PCIe transaction (txn)** is one bus-level write packet between the CPU
side and the CXL controller. Each txn has:

```
+---------------+-------------+--------------+
| target addr   | byte enable | payload data |
+---------------+-------------+--------------+
```

PCIe txns can carry up to ~128–256 bytes of payload (link-dependent
Max Payload Size) but in practice on the cached + clflushopt path one txn
naturally maps to **one cacheline**. This is because clflushopt evicts at
cacheline granularity, and each eviction triggers one PCIe write.

**Cacheline ≠ PCIe txn** as concepts — they are two different layers — but on
the cached-store-and-flush path the natural unit per PCIe txn is one
cacheline.

### 1.3 Alignment and crossing-cacheline

"N-byte aligned" = starting address divisible by N.

| Alignment | Legal start addresses |
|---|---|
| 1B | any |
| 8B | 0, 8, 16, 24, …, 56, 64, 72, … |
| 64B (= cacheline aligned) | 0, 64, 128, 192, … |

A write that occupies `[X, X+N)` **crosses cacheline** if `[X, X+N)` spans
two or more CL boundaries.

Examples:
- 8B at addr 56 → bytes 56–63, all in CL 0 → **does not cross**
- 8B at addr 60 → bytes 60–67 = 4 in CL 0 + 4 in CL 1 → **crosses**
- 64B at addr 0 → all of CL 0 → does not cross
- 64B at addr 32 → CL 0 [32..63] + CL 1 [64..95] → crosses
- 128B at addr 0 → CL 0 + CL 1 → crosses (by size)

### 1.4 Store instructions vs `memcpy()`

A **store instruction** is a single, indivisible CPU micro-op that writes
a fixed number of bytes:

| Instruction | Width | Type |
|---|---|---|
| `mov byte [addr], al` | 1 | cached |
| `mov qword [addr], rax` | 8 | cached |
| `vmovdqu [addr], ymm0` | 32 | cached (AVX2) |
| `vmovdqu [addr], zmm0` | 64 | cached (AVX-512) |
| `movnti [addr], rax` | 8 | **non-temporal** |
| `vmovntdq [addr], ymm0` | 32 | non-temporal |

A `mov qword [addr], rax` (8B aligned) is atomic at the CPU's micro-op level
— it never splits into two 4B stores. This is the CPU-level guarantee
underneath `std::atomic<uint64_t>`.

`memcpy(dst, src, N)` is a C function, NOT an instruction. The compiler /
libc expands it into multiple store instructions:

| `memcpy` size | Expansion (typical) |
|---|---|
| 1 B | 1 × `mov byte` |
| 8 B | 1 × `mov qword` |
| 16 B | 1 × `movdqu` |
| 32 B | 1 × `vmovdqu` (AVX2) |
| 64 B | 1 × `vmovdqu zmm` (AVX-512) or 2 × `vmovdqu ymm` |
| ≥ 128 B | libc loop with vector stores |

So `memcpy(dst, src, 8)` at 8B-aligned `dst` ≈ ONE store, and is just as
atomic as the underlying `mov qword`. Larger `memcpy` is multiple stores
in sequence.

### 1.5 Cached store vs non-temporal store

**Cached store** (default `mov`, vector stores, what `memcpy` emits):

```
memcpy(dst, src, 8)
   ↓ store enters L1 cache (Modified state)
   ↓ cacheline now dirty; data still in L1
   ↓ clflushopt(dst): evict cacheline → one PCIe write txn
   ↓ data lands on CXL; peer can now see it via flushed read
```

**Non-temporal store (movnti)**:

```
movnti [dst], rax
   ↓ bypasses L1 cache entirely
   ↓ enters write-combining buffer (WCB)
   ↓ sfence: drains WCB → one PCIe write txn with byte enables
   ↓ data lands on CXL
```

Key consequence: cached store needs `clflushopt` to push data out (because
data starts in L1). Non-temporal store doesn't need `clflushopt` (because
data was never in L1) — only `sfence` to drain WCB.

### 1.6 Why memcpy false-shares — the **write-allocate** mechanism

When a cached store targets an address whose cacheline is NOT in L1 (or is
in I/S state), the CPU first **reads the cacheline from memory/CXL into L1**
(write-allocate), then dirties the bytes being written, then later evicts.

For two hosts writing disjoint 8B regions in the same CL:

```
T0: host A's L1 = (full CL fetched), [0..7] dirty with A's pattern
T1: host A clflushopt → PCIe write of full CL (A's [0..7] + L1 stale [8..63])
T2: host B writes 8B at [8..15]
    - host B's cacheline not in L1 → write-allocate
    - host B reads CL from CXL → catches whatever state landed at T1
    - if T1 finished: B's L1 = A's [0..7] + reset [8..63]
    - if T1 not finished: B's L1 = full reset
T3: host B dirties [8..15] with B's pattern
T4: host B clflushopt → PCIe write of full CL
    - B's L1 contents (incl. its possibly-stale view of [0..7]) overwrite
    - A's bytes are gone ~75% of the time (the "if T1 not finished" path)
```

The fundamental issue: cached store's writeback is **whole-cacheline** (or
nearly so), and L1's "what other bytes look like" is captured at
write-allocate time, then bundled into the writeback. Whatever stale view
B has of A's bytes wins on B's eviction.

`movnti` doesn't write-allocate. Its WCB-to-PCIe write uses **byte enables**
that target only the 8B B wrote. A's bytes are untouched.

### 1.7 Write-combining buffer (WCB) caveats

The WCB has only a handful of entries (typically 4–8 buckets). When more
distinct cachelines are being written-combined than WCB capacity, the WCB
**partially drains** mid-combine. For a multi-CL movnti sequence under
sustained pressure, this can let other-host PCIe txns interleave at
arbitrary points within a single CL → **per-CL atomicity breaks for movnti
at large N**.

This is observed in FINDINGS.md at N ≥ 256 movnti: ~0.001–0.4 % of trials
show per-CL interleave events. It is NOT observed at N ≤ 64 (single-CL
movnti).

---

## §2 The four experiments

### 2.1 Each experiment tests a different facet of "atomic"

The word "atomic" hides several distinct guarantees. Mapping the four
experiments to which guarantee they test:

| Experiment | Sense of "atomic" tested | Failure mode looked for |
|---|---|---|
| Upper-bound (FINDINGS.md, multi-CL memcpy) | A single multi-CL publish lands either all-old or all-new at the **CL-boundary level** | Different CLs show different writers' content → INTL |
| Lower-bound (FINDINGS_lower_bound.md) | A single store to ≤ 1 cacheline appears as one writer's bytes | Mix of A and B bytes within the written region → INTL |
| Cross-CL torture (Phase B in lower-bound) | Same N as lower-bound, but write straddles a CL boundary | Same as lower-bound but expected to fail |
| False sharing (FINDINGS_concurrency Exp 1) | Two writers in same CL but **disjoint regions**; do their writebacks preserve each other? | One writer's region clobbered → A_LOST / B_LOST |
| Concurrent reader (FINDINGS_concurrency Exp 2) | Reader sees ONE writer's complete publication, not a mid-publish snapshot | Reader's observation mixes A and B bytes → TORN |

### 2.2 The combined picture

For **single-CL, aligned, single-writer** writes (1–64B aligned to natural
width inside one CL):

| Atomicity dimension | Tested by | Result |
|---|---|---|
| Each byte is preserved as one writer's value | All experiments via CORRUPT counter | 0 / 13.2 M trials. **CPU + PCIe preserve byte-level integrity always.** |
| Multiple writers' all-or-nothing CL ownership | Upper-bound + lower-bound | per-CL **never mixes** A and B for memcpy at any N. For movnti at N ≥ 256, < 0.4 % per-CL interleave (WCB drain). |
| Single writer's CL ends up as one writer's content | Lower-bound Phase A + D | **0 / 13.2 M trials** for 1B/4B/8B/16B/32B/64B aligned single-CL. P(interleave) < 10⁻⁶ at 95 % CI. |
| Concurrent reader sees complete published state | Concurrent reader Exp 2 | **0 torn reads / 7 M reads** for N ≤ 64. |

For **same-CL different-region** multi-writer writes:

| Combination | Result |
|---|---|
| Different regions, **memcpy** + clflushopt | **64–89 % data loss** (false sharing via write-allocate) |
| Different regions, **movnti** + sfence | **100 % BOTH_OK across 900 K trials** (byte-enable writeback) |

For **cross-CL or multi-CL** writes:

| Combination | Result |
|---|---|
| 8B write straddling CL boundary | 8.8–13.7 % interleave (two PCIe txns per host) |
| 256B aligned (4 CL) memcpy + clflushopt | 15–31 % full-N interleave (per-CL still atomic, but different CLs different winners) |
| 1024B (16 CL) movnti + sfence | 0.06 % per-CL interleave (WCB drain) |

### 2.3 The single-CL boundary is the hardware-defined edge

All four experiments converge on the same edge: **the cacheline is the unit
at which g1/g2 CXL preserves cross-host write integrity**.

- Inside one CL, aligned, single writer → clean (8M+ trials).
- Inside one CL, multi-writer with movnti → clean (~1.2M trials).
- Anything that crosses or fills multiple CLs → goes through multiple PCIe
  txns → ordering races → interleave possible.
- memcpy + cached store + multi-writer-same-CL → broken by write-allocate
  semantics, independent of CL boundary.

---

## §3 Hardware rules (the empirical "do this / don't do that")

These rules are what the experiments support. They are **hardware
observations**, not algorithmic prescriptions:

| Rule | Empirical backing |
|---|---|
| **R1**. Writes that fit in one cacheline AND are aligned to their natural width are atomic across hosts. | Lower-bound Phase A + D: 13.2 M trials, 0 INTL at 1/4/8/16/32/64 B aligned single-CL. |
| **R2**. Writes that cross a cacheline boundary can interleave at the CL-boundary level. The split happens for ANY reason that turns into 2+ PCIe txns — by size (N > 64) OR by misalignment. | Phase B: 8B straddling = 8.8–13.7 % INTL. Multi-CL aligned: 15–43 % full-N INTL (per-CL still clean). |
| **R3**. `memcpy` writes are subject to write-allocate. Writeback is whole-CL. Different hosts writing different regions of the same CL via `memcpy` overwrite each other ~75 % of the time. | Exp 1: 64–89 % A_LOST for memcpy false-sharing across GAP=0/8/48. |
| **R4**. `movnti` writes don't write-allocate. PCIe write uses byte enables, so other-host bytes in the same CL are untouched. Multi-writer same-CL is safe with movnti. | Exp 1: 100 % BOTH_OK across 900 K trials with movnti. |
| **R5**. `movnti` at LARGE N (multi-CL, sustained pressure) can occasionally show per-CL interleave (~0.001–0.4 %). This is WCB partial-drain. | FINDINGS.md raw data: 5 per-CL INTL events found across millions of multi-CL movnti trials. Always at N ≥ 256 under async sustained write. |
| **R6**. A reader doing `clflushopt + mfence + load` on a single-CL slot **never sees a torn snapshot** even when the writer is publishing concurrently. | Exp 2: 0 / 7 M+ reads torn at N = 1, 8, 64. |
| **R7**. A reader doing the same on a multi-CL target CAN catch a partial-publish state (one CL old, another new) at ~0.25–1 % rate. | Exp 2: 5 K–21 K torn / ~2.1 M reads at N = 128. |
| **R8**. byte-level write integrity holds always. No matter what config, the position-indexed low nibble has been consistent across ~700 K + 13.2 M + ~7 M reads = ~20 M trials, 0 CORRUPT events. | All experiments. |
| **R9**. Strict atomicity (P_interleave = 0) does NOT exist at any size at the K = 10⁶ trial bound. What was published as "0 events" in earlier 10 K trials was a sampling artifact; 100 K and 1 M trials show non-zero rates at every multi-CL size, and the per-CL movnti corner case at large N. The lower-bound P at single-CL is < 10⁻⁶, which is the strongest claim measurement supports. | Phase 4 variance reps + 10K → 100K → 1M scaling in FINDINGS.md retraction. |

---

## §4 Specifics worth knowing if revisiting

### 4.1 Per-CL atomicity is NOT a universal property

It is a property of the **cached store + clflushopt** path. Specifically:

- cached store + clflushopt → 1 PCIe txn per CL → arrives at CXL controller
  as one indivisible event → per-CL atomic
- non-temporal store + WCB-coalesce → usually 1 PCIe burst per CL → usually
  per-CL atomic, but breaks under WCB pressure (R5)

If a future workload pushes through movnti at large N, expect per-CL
breakage at ~0.1 % rate or so. memcpy + clflushopt has no such failure
mode at the per-CL level — only at the multi-CL level (different CLs from
different writers).

### 4.2 The "1 µs barrier skew" in the data

Almost all experiments showed OW_B (host 1 wins) much more often than
OW_A. This is because the barrier-sync mechanism's CXL coherence latency
is ~1 µs, so host 1 enters the race ~1 µs after host 0 — host 1 is always
the "later writer" in barrier mode and wins the overwrite under most
conditions.

This is a **timing artifact of the test methodology**, not a hardware
property. The interleave classification is independent of who wins; what
matters is whether bytes from both writers appear in the final state.

### 4.3 Variance is real

Phase 4 of the upper-bound study revealed that the same cell run 5 times
gave INTL counts of 3, 7, 13, 463, 1680 (at N=192 memcpy K=100K barrier).
The 1680 was a ~100× outlier. The mechanism is run-state dependent: the
exact PCIe arbitration / thermal / CPU frequency state at the moment of
the race affects how much host A's and host B's clflushopt bursts overlap.

For any **future verification**:
- 3 reps minimum
- 100 K trials minimum
- Report median + worst-case, not just one number

### 4.4 What was NOT tested (or only partially)

- **>2 hosts**: only 2 hosts on g1/g2. R1–R4 should follow from "one PCIe
  txn arrives at CXL atomically" regardless of how many hosts contend, but
  this is unverified. Layout B (8 hosts in one CL via movnti, per
  FINDINGS_concurrency §5.1) specifically would benefit from 4-host
  validation.
- **Different CXL hardware**: results are specific to g1/g2 XConn switch +
  this generation of CXL.mem controller. The "cacheline = atomic unit"
  claim is a likely-general property but the rates and edges might differ
  on other hardware.
- **Reader during multi-host write**: only 1 writer + 1 reader tested in
  Exp 2. Two writers + one concurrent reader (a realistic distributed-lock
  scenario) is unverified.
- **movnti per-CL break at smaller N**: We see it at N ≥ 256. We haven't
  characterized exactly where in [64, 256] it first appears.
- **Same-CL multi-region with > 2 hosts**: Exp 1 only had 2 hosts writing
  to disjoint regions. Whether 4–8 hosts can all coexist in one CL via
  movnti is unverified.

---

## §5 Where to find what

| Resource | Content |
|---|---|
| `docs/study_cxl_write_atomicity/PLAN.md` | Original scope + locked design decisions |
| `docs/study_cxl_write_atomicity/FINDINGS.md` | Upper-bound study (multi-CL atomicity, the rate cliff, strict-atomic retraction) |
| `docs/study_cxl_write_atomicity/FINDINGS_lower_bound.md` | Lower-bound study (1–64B aligned single-CL strict atomic at P < 10⁻⁶) |
| `docs/study_cxl_write_atomicity/FINDINGS_concurrency.md` | False sharing (Exp 1) + concurrent reader (Exp 2) |
| `docs/study_cxl_write_atomicity/LEARNINGS.md` | THIS FILE: hardware primer + 4-experiment synthesis + empirical rules |
| `docs/study_cxl_write_atomicity/atomicity_chart.png` | INTL rate vs N with variance dots, 4 (store, mode) variants |
| `docs/study_cxl_write_atomicity/atomicity_summary_table.md` | Per-N per-(store, mode) INTL/K table |
| `docs/study_cxl_write_atomicity/phase{1,2,3,4_*,A,B,C,D}_*/raw.csv` | Raw per-cell measurements |
| `docs/study_cxl_write_atomicity/false_sharing/raw.csv` | Exp 1 raw data |
| `docs/study_cxl_write_atomicity/concurrent_reader/raw.csv` | Exp 2 raw data |
| `tests/cxl_write_atomicity_probe.cc` | Main probe (upper + lower bound) |
| `tests/cxl_false_sharing_probe.cc` | Exp 1 probe |
| `tests/cxl_concurrent_reader_probe.cc` | Exp 2 probe |
| `scripts/run_cxl_atomicity_study.sh` | Driver for the main probe sweep |
| `scripts/plot_cxl_atomicity_study.py` | Generates atomicity_chart.png |
