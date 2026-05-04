# iter-8A Phase 3 (proper) — per-stage Expected from µbench primitives (Sol-4)

**Author**: Claude (iter-8A spare-time mandate completion)
**Date**: 2026-05-04
**Source primitives**: `docs/iter8A_phase1_ubench/baseline.md`
**Stage definitions**: `docs/protocol_a_architecture_blueprint.md` Part II

This table converts each blueprint stage's primitive list into an
"Expected typical cost" using the µbench primitive measurements,
replacing the **estimated** baselines that appeared in the blueprint
text with **measured-derived** values.

## Primitive cost reference (median ns from µbench)

| Primitive | ns |
|---|---|
| LD-CXL (post-flush+mfence) | 630 |
| ST-CXL + flush + sfence | 14 + 66 + 13 = ~93 |
| FLUSH (clflushopt) + sfence | 66 |
| MFENCE (alone) | 23 |
| SFENCE (alone) | 13 |
| LFENCE (alone) | 16 |
| RMW-CXL fetch_add NO flush | 17 |
| RMW-CXL fetch_add + flush + sfence | 1435 |
| LD-DRAM (cached) | ~5 |
| ST-DRAM (cached) | ~5 |
| Spinlock uncontested | 29 |
| Spinlock T=64 contended p99 | **577 774** (= 578 µs!) |
| Spinlock T=64 contended max | 24 994 375 (= 25 ms!) |

## Per-stage Expected (uncontested / typical)

### Worker WRITE path (W1..W12) — UPDATE on owner-self, KV=1024, no peer sharers

| Stage | Primitives breakdown | Expected ns | Expected µs |
|---|---|---|---|
| W1 entry+hash | FNV1a+ptr | 10 | 0.01 |
| W2 bucket+lock | 2×FLUSH(66) + MFENCE(23) + 7×LD-CXL(630) + LOCK(29) | 4 594 | 4.59 |
| W3 sharer_bitmap | FLUSH(66) + MFENCE(23) + LD-DRAM(5) | 94 | 0.09 |
| W4 inval-loop entry | branch+loop bookkeeping | 50 | 0.05 |
| W5..W6 inval ACKs | 1× send_invalidate I1..I8 = ~5 µs typical | 5 000 | 5.00 |
| W7 pool alloc | 1× RMW-CXL no-flush (AP16) | 17 | 0.02 |
| W8 pool write KV=1024 | memcpy + 16×FLUSH(66) + SFENCE(13) | 1 069 | 1.07 |
| W9 publish slot CoW | 2×ST-CXL(14) + 2×FLUSH(66) + 2×SFENCE(13) | 186 | 0.19 |
| W10 dir state | 3×ST-DRAM(5) + LOCK-rel(5) | 20 | 0.02 |
| W11 local cache | DRAM hashmap insert + per-bucket lock | ~3 000 | 3.00 |
| W12 return | trivial | 5 | 0.01 |
| **Total W1→W12 typical** | | **14 045** | **14.05** |

### Worker READ path (R1..R6) — cross-host miss → forward_cache_register, KV=1024

| Stage | Primitives breakdown | Expected ns | Expected µs |
|---|---|---|---|
| R1 entry | trivial | 5 | 0.01 |
| R2 cache lookup | DRAM hashmap lookup | 300 | 0.30 |
| R3 forward_cache_register | F1..F7 round-trip typical | 5 000 | 5.00 |
| R4 cache insert post-ACK | DRAM hashmap insert | 3 000 | 3.00 |
| R5 owner-self miss | 2×FLUSH(66) + MFENCE(23) + 7×LD-CXL(630) + 16×LD-CXL(630)/pool + cache insert | ~17 000 | 17.00 |
| R6 return | trivial | 5 | 0.01 |
| **Total R1→R6 cross-miss** | R1+R2+R3+R4+R6 (R5 alternate path) | **8 311** | **8.31** |

### Send-invalidate sub-stages (I1..I8) — single peer

| Stage | Primitives breakdown | Expected ns | Expected µs |
|---|---|---|---|
| I1 reserve slot | RMW-CXL fetch_add+flush+sfence | 1 435 | 1.44 |
| I2 write entry | (slot-free wait=0 healthy) + 4×ST-DRAM + flush+sfence | 95 | 0.10 |
| I3 (consumer) sees tail | covered in D2 | (D2) | (D2) |
| I4 (consumer) loads entry | covered in D3 | (D3) | (D3) |
| I5 (consumer) sets stale | covered in D4 | (D4) | (D4) |
| I6 (consumer) writes ACK | covered in D5 | (D5) | (D5) |
| I7 spin on resp | 7 spin iters × (FLUSH+MFENCE+LD-CXL = 719) | 5 033 | 5.03 |
| I8 free slot | ST-CXL+flush+sfence | 93 | 0.09 |
| **Total I1+I2+I7+I8 (producer side)** | | **6 656** | **6.66** |

Note: I7 typical cost dominates; capped at 5 ms (`kBudgetUs = 5000`)
since iter-6A. Pre-iter-6A was 200 ms.

### Forward-to-owner / cache_register (F1..F7) — single round-trip

| Stage | Primitives breakdown | Expected ns | Expected µs |
|---|---|---|---|
| F1 enter | trivial | 5 | 0.01 |
| F2 reserve slot | RMW-CXL fetch_add+flush+sfence | 1 435 | 1.44 |
| F3 wait-free + write | (wait=0 healthy) + 5×ST-DRAM + flush+sfence | 110 | 0.11 |
| F4..F6 (consumer) | responder_handle = directory lookup + bitmap upd + maybe bucket flush+scan | ~2 000 | 2.00 |
| F7 spin on resp | 7 spin iters × 719 | 5 033 | 5.03 |
| **Total F1+F2+F3+F7 (producer)** | | **6 583** | **6.58** |

Note: F7 capped at 5 ms (`kBudgetUs = 5000`) since iter-8A Phase 5
fix. Pre-iter-8A was 200 ms.

### CacheDispatcher loop (D1..D5) — per inval consumed

| Stage | Primitives breakdown | Expected ns | Expected µs |
|---|---|---|---|
| D1 outer loop | trivial + PAUSE | 5 | 0.01 |
| D2 per-src poll | FLUSH+MFENCE+LD-CXL | 719 | 0.72 |
| D3 load entry | FLUSH+MFENCE+LD-CXL | 719 | 0.72 |
| D4 set_stale | DRAM hashmap lookup + 1B store | 200 | 0.20 |
| D5 write ACK | ST-CXL+flush+sfence | 93 | 0.09 |
| **Total D2+D3+D4+D5 per-inval** | | **1 731** | **1.73** |

Single-thread dispatcher max throughput: 1 / 1.73 µs = **578k inval/sec**.

## Key contention multipliers (CRITICAL — under hot-key Zipf)

When workers contend on the SAME slot directory entry (W2 lock), the
spinlock cost balloons per the µbench scaling table:

| T concurrently on same lock | W2 added latency p99 |
|---|---|
| 1 | 0 µs (uncontested baseline 4.6 µs) |
| 4 | +12 µs |
| 16 | +182 µs |
| 32 | +407 µs |
| 64 | **+578 µs** (worst-case max +25 ms!) |

This means **W2 expected at T=64 hot-key**: 4.6 µs (uncontested) + up
to 578 µs (p99 spinlock) = **583 µs p99** under worst-case contention.

Workload-d Zipf θ ≈ 0.99 → ~5% of keys see ≥10 concurrent threads.
At T=64 ~3-5 hot keys see 30-60 threads simultaneously → W2 p99 in
those cells genuinely is hundreds of µs.

## What this table replaces

The blueprint Part II had per-stage "Healthy baseline" estimates
written PRE-µbench. They assumed `clflushopt = ~600 ns` (wrong; it
is 66 ns; the 600 ns was LD-CXL post-flush). Affected stages:

| Stage | Blueprint pre-µbench | Re-derived from µbench | Ratio |
|---|---|---|---|
| W2 | "5.5 µs uncontested" | 4.59 µs | OK (close) |
| W3 | "610 ns" | 94 ns | **6.5× too high** |
| W8 (KV=1024) | "10 µs" | 1.07 µs | **9.3× too high** |
| W9 | "1.2 µs" | 0.19 µs | **6.3× too high** |
| I1 | "700 ns" | 1.44 µs | **2× too low** (forgot fetch_add+flush cost) |
| I7 per-iter | "(implied 600 ns)" | 719 ns | OK |
| F2/F3 | (not stated) | 1.55 µs | (new) |

The blueprint Part III.1 cheat-sheet was already corrected at iter-8A
Phase 7. This file is the **per-stage** correction Phase 3 owed.
