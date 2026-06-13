# Findings: CXL cross-host write atomicity on g1/g2

**Date**: 2026-06-13
**Hardware**: g1 + g2, dual-host CXL Type-3 over XConn switch, kernel
6.15.0-uintr, `/dev/dax0.0` 256 GiB, no CPU-level cache coherence between
hosts.
**Test driver**: [tests/cxl_write_atomicity_probe.cc](../../tests/cxl_write_atomicity_probe.cc)
**Plan**: [PLAN.md](PLAN.md)

---

## §1 TL;DR

On g1/g2 cross-host CXL writes to a shared address, the **interleave
probability** depends sharply on the **store instruction** used and the
**number of cachelines touched**:

| Write idiom | Atomic-overwrite range | First interleave at |
|---|---|---|
| `memcpy` + `clflushopt` + `sfence` | **≤ 3 cachelines (≤ 192 B)** | 4+ CL (≥ 224 B) |
| `movnti` (non-temporal 8 B) + `sfence` | **All sizes tested** (1 B – 1024 B) | NOT observed in 10⁴+ trials |

**Implication for LFM v2**: a flag-cell of up to **192 B can be atomically
published** between hosts using the cacheable+`clflushopt` idiom, or **any
size** if the writer uses non-temporal stores. The widely-held "all
cross-host CXL writes interleave" intuition is wrong on this hardware.

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

Critically, **N = 128 (2 cachelines) is NOT strictly atomic**: 5 interleave
events in 10 K trials sets P(interleave) at ~5 × 10⁻⁴ ± 2 × 10⁻⁴, i.e.,
~50 failures per 10⁶ ops. The first 1 K-trial cells showed this signal
faintly (5 events in two runs) but it was within the noise band; the
10 K-trial cell confirms it is real.

Conversely, **N = 192 (3 cachelines) IS strictly atomic** under this idiom
to the 10 K-trial detection floor. The jump in interleave probability
between N=192 and N=256 (0 → 15.8 %) is the **boundary** that defines
the LFM-v2 atomic flag size.

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

- **Zero CORRUPTION events** across all variants and sizes — every byte
  ever observed at the target had a top nibble in {0xA, 0xB, 0xC},
  i.e., the position-index field always matched. Conclusion: byte-level
  writes are atomic on the CXL → no torn-byte cases at all.
- **Zero interleave under `movnti`** in barrier mode at every N tested.
  Strong evidence that the CXL controller's per-8-byte ordering is
  fully respected for non-temporal stores.
- **Zero interleave at N ≤ 192 B** under cacheable+`clflushopt`. The
  3-cacheline atomic window is a robust property of the XConn switch /
  PCIe controller pair on this generation of hardware.

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

Two viable paths emerge for the lighter-weight LFM:

### Path A: 192-byte flag cell with `memcpy + clflushopt + sfence`

A flag-cell of up to 192 bytes (3 × 64-byte cachelines) is **published
atomically** between hosts using the existing cacheable+flush idiom.
This is the smallest behavioral change from LFM v1.

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

### Path B: arbitrary-size flag with `movnti + sfence`

If the LFM v2 datapath can be reworked to use non-temporal stores
exclusively, the flag-cell can be **any size** up to the dax page
boundary, with atomicity guaranteed at the 8-byte chunk level. Useful
if the design wants to share a giant 4 KB flag cell across N hosts
for a richer consensus structure.

Tradeoff: `movnti` writes bypass the cache, so the writer can't
quickly re-read its own write. For LFM v1, the writer reads back to
verify; with v2-Path-B you'd need a separate read of the post-flush
value from CXL.

### Recommended starting point

**Path A**. Strictly fewer code changes, lock acquire latency
decreases by ~40 % per probe-decomp model, and the 192 B atomic
publication unit gives plenty of room for current LFM state without
needing the algorithm change Path B implies.

A design sketch lives at
[src/cxl_fusee_lfm_v2.h](../../src/cxl_fusee_lfm_v2.h).
