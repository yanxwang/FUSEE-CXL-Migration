# iter-8A Phase 1.A — CXL/DRAM primitive µbench baseline

**Tool**: `tests/cxl_primitive_bench.cc`
**Host**: g3 (single-host benchmark; cross-host PINGPONG TBD)
**Date**: 2026-05-04
**TSC**: 2.0 GHz (calibrated)

## Per-primitive cost (RDTSCP-measured, single-host)

| Primitive | p50 ns | p99 ns | max ns | Notes |
|---|---|---|---|---|
| mfence (alone) | 23 | 24 | 24 | full barrier |
| sfence (alone) | 13 | 14 | 15 | store barrier |
| lfence (alone) | 16 | 17 | 18 | load barrier |
| clflushopt + sfence (CXL line) | 66 | 69 | 70 | flush COMPLETE-to-CXL |
| LD-CXL post-flush+mfence | 630 | 947 | 14 000 | matches mlc baseline ~600ns |
| ST-CXL + flush + sfence | 14 | 15 | 208 | store itself fast; flush is async |
| CXL atomic fetch_add (NO flush) | 17 | 17 | 1 347 | cached locally; not visible cross-host |
| CXL atomic fetch_add + flush + sfence | **1 435** | 1 699 | 12 620 | **84× slower than no-flush** (CXL roundtrip) |
| Spinlock uncontested (lock+unlock) | 29 | 30 | 1 317 | DRAM atomic CAS |

## Spinlock contention scaling (CRITICAL FINDING)

T = number of threads contending on ONE pthread_spinlock.

| T | p50 ns | p99 ns | max ns |
|---|---|---|---|
| 1 (uncontested) | 29 | 30 | 1 317 |
| 2 | 90 | 1 726 | 3 866 |
| 4 | 1 405 | 11 724 | 92 390 |
| 8 | 1 201 | 55 926 | 908 611 |
| 16 | 2 442 | **181 736** | 803 075 |
| 32 | 5 639 | **406 534** | 1 793 902 |
| 64 | 10 139 | **577 774** | **24 994 375** (= 25 ms!) |

## Implications for Protocol A bottleneck attribution

1. **SlotDirectory spinlock at T=64 hot-key contention** = **p99 578 µs, max 25 ms per acquire**.
   This **alone** explains:
   - iter-6A/7A worker write p99 of hundreds of µs
   - The "max 913 µs" in iter-7A healthy probe of W1→W12
   - Likely the dominant contributor to collapsed cells (workers piling on hot Zipf key)
   - **Not OS preemption** — direct primitive-level cost

2. **Pool's `bump.fetch_add` CXL atomic without flush_line** (AP16 hazard) is currently **17 ns** (cached locally). With flush it would be **1.4 µs** (84× cost) — explaining why iter-5A/6A didn't see this as a bottleneck (probe data wouldn't show it either since no peer reads cursor today).

3. **flush_line+sfence cost is 66 ns** — much lower than my blueprint baseline assumed (~600 ns). The 600 ns was confused with LD-CXL which IS ~630 ns. **flush_line is fire-and-forget; only the SFENCE waits**, and SFENCE itself is just 13 ns when nothing pending.

4. **Spinlock DRAM atomic CAS uncontested = 29 ns** — matches blueprint baseline.

## Blueprint Part III.1 update needed (Phase 7 task)

Replace the cheat-sheet column in blueprint Part III.1 with these measured values. Particularly:
- "clflushopt complete to CXL ~600 ns" → **66 ns alone; the 600 ns was LD-CXL post-flush**
- "CXL atomic RMW ~200 ns" → **17 ns no-flush, 1435 ns with flush+sfence**
- Add new row: "spinlock contention scaling T → p99 ns" table

## Cross-validation TBD (Phase 1.A.2 — short experiment)

- 2-host CXL atomic ping-pong (deferred unless Phase 3 needs it)
- LD-CXL while peer host writing same line
