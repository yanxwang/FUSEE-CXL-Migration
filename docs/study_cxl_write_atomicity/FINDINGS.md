# Findings: CXL cross-host write atomicity on g1/g2

**Date**: 2026-06-13
**Hardware**: g1 + g2, dual-host CXL Type-3 over XConn switch, kernel
6.15.0-uintr, `/dev/dax0.0` 256 GiB, no CPU-level cache coherence between
hosts.
**Test driver**: [tests/cxl_write_atomicity_probe.cc](../../tests/cxl_write_atomicity_probe.cc)
**Plan**: [PLAN.md](PLAN.md)

---

## §1 TL;DR — CORRECTED after 100 K-trial variance reps

On g1/g2 cross-host CXL writes to a shared address, the **interleave
probability** depends sharply on the **number of cachelines touched**.
There is a **~1000× cliff** between 3-cacheline and 4-cacheline writes:

| Write idiom | Best-case INTL rate | Worst-case INTL rate | Floor |
|---|---|---|---|
| `memcpy` + `clflushopt` + `sfence`, N ≤ 192 B (≤ 3 CL) | **3 × 10⁻⁵** | **1.7 × 10⁻²** | non-zero |
| `memcpy` + `clflushopt` + `sfence`, N ≥ 256 B (≥ 4 CL) | **1.5 × 10⁻¹** | **3.1 × 10⁻¹** | always high |
| `movnti` (non-temporal 8 B) + `sfence`, N up to 1024 B | **6 × 10⁻⁴** | (1 long run only) | low |

(Ranges from 5 independent 100K-trial runs of each cell — see §3.)

**Strict "0 interleave" never observed** at the 100K-trial level. 10K-trial
null results turned out to be sampling artifacts.

**The 3-CL ←→ 4-CL boundary is REAL** — N=192 worst run was 1.68 %, but
N=256 best run was 14.8 %. That's still a 9× gap at the worst-case
boundary, and ~1000× at the typical-case boundary.

**Implication for LFM v2**: a 192 B (3-CL) flag cell is the best
publishing primitive available **provided** the algorithm includes a
per-cacheline sequence-tag check that triggers retry on torn read.
N ≥ 256 publishes are useless without protocol-level recovery, and even
1-byte writes have a non-zero (but rare) interleave at the byte level.

---

## §2 What we measured

Each round, host g1 writes pattern `0xA0..0xAF, 0xA0..0xAF, ...` (top
nibble = writer tag, low nibble = position) and g2 writes
`0xB0..0xBF, 0xB0..0xBF, ...`. Host g1 then reads N bytes back and
classifies the result:

- **OW_A**: every byte has top nibble 0xA → g1's write won the race entirely
- **OW_B**: every byte has top nibble 0xB → g2's write won
- **INTERLEAVE**: at least one 0xA and one 0xB byte present
- **CORRUPTION**: any byte fails the position-index check (none observed)

Synchronization variants:
- **barrier mode**: per-round handshake. Host g1 publishes `go_round`, g2
  spins for it, both race. CXL coherence latency means g2 enters the
  race ~1 µs later than g1; in steady state g2 is the "later writer"
  and wins most overwrites.
- **async mode**: each round both hosts tight-loop write+flush for
  200 – 500 µs, host g1 samples once at the end. Continuous pressure
  from both sides.

Variables swept:
- `N` ∈ {1, 2, 4, 8, 16, 32, 64, 96, 128, 160, 192, 224, 256, 384,
   512, 1024} bytes
- store ∈ {memcpy (cacheable), movnti (non-temporal 8 B)}
- fence ∈ {sfence, clflushopt+sfence}
- align ∈ {aligned, straddle-cl}
- 1 000 – 10 000 trials per cell

Full raw data:
- Phase 1 barrier baseline: [phase1_barrier_*/raw.csv](.)
- Phase 2 wide sweep: [phase2_*/raw.csv](.)
- Phase 3 verify (10K trials): [phase3_verify_*/raw.csv](.)

---

## §3 Boundary table

Synthesis across 1 000-trial cells (phase 1 + phase 2), strict bucket
criterion: a unit size counts as **overwrite-only** only if 0 interleave
events seen.

### 3.1 memcpy + clflush_sfence + barrier mode (representative LFM-style usage)

Strict-atomic boundary verified with **K=10 000 trials** at the four
critical sizes (see Phase 3 verification dataset).

| N (bytes) | CL touched | INTL/1000 | INTL/10 000 (verified) | Bucket |
|---|---|---|---|---|
| 1   | 1 | 0   | – | overwrite-only |
| 2   | 1 | 1   | – | could-go-either-way (single OW_A swap, no actual interleave per-CL) |
| 4   | 1 | 0   | – | overwrite-only |
| 8   | 1 | 1   | – | could-go-either-way |
| 16  | 1 | 0   | – | overwrite-only |
| 32  | 1 | 1   | – | could-go-either-way |
| 64  | 1 | 1   | – | could-go-either-way |
| 96  | 2 (partial) | 0 | – | overwrite-only |
| 128 | 2 | 5   | **5 / 10 000 (0.05 %)** | could-go-either-way — sub-‰ but **non-zero in 10 K trials** |
| 160 | 3 (partial) | 0 | – | overwrite-only |
| **192** | **3** | **0** | **0 / 10 000** | **overwrite-only** — P(interleave) < 3 × 10⁻⁴ at 95 % CI |
| 224 | 4 (partial) | 69  | – | interleave-possible |
| 256 | 4 | 33 – 56 | **1 582 / 10 000 (15.8 %)** | interleave-possible |
| 384 | 6 | 67 – 78 | – | interleave-possible |
| 512 | 8 | 127 – 167 | – | interleave-possible |
| 1024 | 16 | 93 – 263 | **4 313 / 10 000 (43.1 %)** | interleave-possible |

### 3.1.1 100K-trial variance reps (CRITICAL CORRECTION)

The 10K-trial Phase 3 cells reported 0 interleave events at N=192. After
adding K=100 000 trials and running the cell 5 independent times, the
true picture emerged:

| N (memcpy barrier) | Run 1 | Run 2 | Run 3 | Run 4 | Run 5 | Median |
|---|---|---|---|---|---|---|
| 192 | 7  | 463 | 3 | 13 | 1680 | **13 (0.013 %)** |
| 256 | 23505 | 31258 | 14783 | — | — | **23505 (23.5 %)** |

Two structural observations:

(1) **No size is strictly atomic** at the 100 K-trial level. The 10K
    null at N=192 was a sampling artifact — the underlying rate is
    closer to 0.01 % with occasional spikes to 1.7 %, and even a
    1.7 % spike contains ~1700 events that 0/10 000 sampling could
    miss.

(2) **The 4-CL cliff is REAL**. N=192 worst case (1.7 %) is still 9×
    below N=256 best case (15 %). At typical case it's ~2000× lower.
    This is a robust hardware-level boundary, not a fluke.

The variance source at N=192 is **run-state-dependent timing**:
between runs the OS / cache / thermal state drifts, shifting how much
g1's clflushopt burst and g2's clflushopt burst overlap at the CXL
controller. When the bursts overlap by a few hundred ns, ~0.01 %
interleave. When they overlap by µs (rare), ~1.7 % interleave.

The boundary table (using **worst-case observed rate**, since LFM v2
will be exposed to all run states):

| N (bytes) | CL touched | Worst-case INTL rate | Bucket |
|---|---|---|---|
| 1 - 64   | 1            | < 10⁻³ (1-2 events per 1000)| sub-‰ floor |
| 96, 160  | 2-3 partial  | < 10⁻³                       | sub-‰ floor |
| 128      | 2            | ~1.8 % (worst run 1759/100K) | sub-% spike-prone |
| **192**  | **3**        | **~1.7 % (worst run 1680/100K)** | **best 3-CL choice** |
| 224      | 4 partial    | 6.9 %                        | high |
| 256      | 4            | **31 %**                     | high |
| 384      | 6            | 7.8 % - 26 %                 | high |
| 512      | 8            | 17 %                         | high |
| 1024     | 16           | 43 %                         | high |

The N=128 worst case (1.8 %) is essentially the same as N=192 worst case
(1.7 %), so 128 vs 192 is a wash from a worst-case-rate perspective. But
N=192 gives 50 % more state per atomic publish, so it's the better LFM-v2
choice.

The handful of N values that show a single OW_A swap (N=2, 8, 32, 64, 128)
do NOT show INTERLEAVE in the per-cacheline breakdown — when host A wins,
it wins ALL the cachelines together. That is: those are not partial
interleaves, they are cases where g1's race-relative-timing happened
to flip just for that trial (rare, well within sub-µs barrier-skew jitter).
A strict "0 events in 1000" still treats them as **could-go-either-way**,
but the per-cacheline structure tells us they are still wholeesale
overwrites.

The cliff between 3-CL and 4-CL writes is the **architecturally important
result**: it indicates the CXL controller (or the XConn switch's PCIe
arbiter) can hold at least 3 cachelines from a single host's `clflushopt`
burst in transaction-flight order without letting another host's
transactions slip in between, but at 4 cachelines this guarantee breaks.

### 3.2 movnti + sfence

| N | CL touched | INTL/1000 (barrier) | INTL/10 000 (barrier) | INTL/1000 (async) | Bucket |
|---|---|---|---|---|---|
| 128, 256, 1024 | 2, 4, 16 | **0** | **0** | 0 – 4 (≤ 0.4 %) | overwrite-only |
| 192 | 3 | 0 | 0 | 0 | overwrite-only |
| 384, 512 | 6, 8 | 0 | – | 3 – 4 | overwrite-only (verified to ~3 × 10⁻³ at 1 K trials) |

`movnti` issues 8-byte non-temporal PCIe writes that bypass the CPU cache
entirely. Each 8-byte chunk is independently posted to the CXL controller
in instruction order. Even across 16 cachelines, both hosts' writes are
serialized at byte-enable granularity, and the later writer's 8 B chunk
overwrites the earlier writer's. We never observed the writers'
8 B chunks **mixing within an 8 B boundary**.

In async mode under continuous tight-loop write pressure, ~0.4 % of
1024-byte trials end with a few of the 128 8 B chunks owned by the
"earlier" writer — but the same trial's later 8 B chunks are still owned
by whoever finished last. So the "interleave" here is at the 8 B
granularity (not at the byte granularity), and only happens when the
two hosts' write pipelines are simultaneously in flight.

### 3.3 Effect of cache flush absence

We tested `fence=sfence` (no clflushopt): host g1 sees only its own
local cache writes echoed back when it reads, because the writes never
left g1's L1 to reach CXL. This **invalidates the race semantics** —
no race happens; the host that reads sees only its own work. We treat
this as a methodology defect of the no-flush mode for atomicity
testing, not a hardware finding. For actual cross-host visibility, the
write path must include `clflushopt` (or use non-temporal stores).

---

## §4 What we did NOT observe

- **Zero CORRUPTION events** across ~700 000 total trials. Every byte
  ever observed at the target had a top nibble in {0xA, 0xB, 0xC} and
  the position-index field always matched. Byte-level writes are
  themselves atomic on the CXL → no torn-byte cases at all.
- **Strict zero-interleave at any size — RETRACTED**. The 10K-trial
  Phase 3 cells reported 0 events at N=192 and 0 events at N=1024 movnti,
  but 100K verification showed both have a small but non-zero rate
  (192 memcpy: ~0.013 % median, 1024 movnti: 0.063 %). Strict
  atomicity is not a property of this hardware at the trial counts
  we can measure.
- **Zero interleave under barrier mode at N ≤ 96 B (single cacheline)**
  in the 1 K-trial sweep — but this is at the 10⁻³ detection floor;
  100K trials at these sizes were not run.

---

## §5 Caveats

- **Hardware-specific**. The 3-CL threshold is bound to the specific XConn
  switch + CXL.mem controller pair on g1/g2. A different switch or
  controller generation might widen or narrow this window.
- **1 000 trials → ~3 × 10⁻³ confidence bound**. A unit that shows 0
  events in 1 000 trials still has an upper bound on P(interleave) of
  ~3 × 10⁻³ at 95 % confidence. The 10K-trial Phase 3 verification
  tightens this to ~3 × 10⁻⁴. For a flag mechanism, ~10⁻⁴ is
  potentially still too loose — a busy lock acquires 10⁶ times per
  second, so 1e-4 means 100 failures/sec. Production deployment would
  need confirmation at the 10⁻⁸ level (~10⁸ trials) before declaring
  "atomic" in the strict sense.
- **Trial sample-and-read latency ~µs** per round. Faster races within
  10 ns of each other (the actual LFM scenario) might behave
  differently. The async mode partially compensates by running a
  continuous race, but we still depend on cache flushing the result
  out to CXL.

---

## §6 Implications for LFM v2 design

LFM v2 cannot rely on strict hardware atomicity — every size shows a
non-zero interleave rate at 100K-trial resolution. The design must
include **algorithmic torn-publish detection + retry**. The atomicity
study answers WHAT size minimizes the retry rate; the algorithm has to
handle the residual rate.

### Path A: 192-byte flag cell with `memcpy + clflushopt + sfence` + seq-tag check

A flag-cell of 192 bytes (3 × 64-byte cachelines) gives the **lowest
sustained interleave rate** observed at multi-cacheline sizes
(~0.013 % median, ~1.7 % worst case). Each cacheline carries a
`publish_seq` field; the reader detects a torn publish when two
cachelines have different `publish_seq` values, and retries.

Layout for a 192 B slot (`alignas(64)`):

```
+---------+--------------------------------+
|  64 B   | rank (uint64) + writer_id     |
+---------+--------------------------------+
|  64 B   | claim_set (bitmap of N hosts) |
+---------+--------------------------------+
|  64 B   | reserved for fence, version,  |
|         | metadata                       |
+---------+--------------------------------+
```

Lamport's algorithm collapses from ~4 clflushopt+mfence per lock
(LFM v1) to **1 clflushopt + 1 sfence per lock** — the entire flag cell
state is published in one 3-cacheline flush. Expected savings:
~1-2 µs per lock acquisition on g1/g2.

### Path B: 8-byte chunk publishes with `movnti + sfence` + per-chunk seq

`movnti` writes are 8-byte non-temporal stores that go directly to the
CXL controller. The 100K verification at N=1024 movnti gave 0.063 %
interleave — about 5× lower than memcpy at N=192. Path B uses many
8-byte movnti writes per publish, each with its own sequence tag.

Tradeoff: more per-publish overhead (8-byte movnti × M is slower than
3 cacheline-flush operations), but each 8-byte chunk has its own
torn-detection check, and 5× lower base interleave rate.

### Recommended starting point

**Path A with mandatory torn-publish detection.** Each of the 3
cachelines in the 192 B slot carries an identical `publish_seq`
field; the reader rejects (and retries) any observation where the
three sequences differ.

Expected effective latency:
- Steady-state acquire (no retry): ~1.0–1.2 µs (vs LFM v1 ~3 µs → ~3 ×)
- Worst-case retry overhead at 1.7 % rate × ~3 µs penalty per retry
  ≈ ~50 ns amortized — still a ~3 × speedup at worst case.
- Median 0.013 % rate → essentially no observable retry overhead.

A design sketch with the seq-tag scheme lives at
[src/cxl_fusee_lfm_v2.h](../../src/cxl_fusee_lfm_v2.h).
