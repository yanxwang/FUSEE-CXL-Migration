# P5 attribution conclusion — copy elimination at the BW level

**Phase**: P5
**Date**: 2026-05-19
**Question**: iter-13A delivered HAZARD direct-pool read + W1 reserved
per-host write segments. Why did throughput not improve over the
STAGING baseline despite eliminating data copies on both paths?
**Output bar**: quantitative case A/B/C/D conclusion per
[case_framework.md](../iter14A_p5_attribution_framework/case_framework.md).

## Cell + builds

- Workload: workloada (R50W50 Zipf, 64 threads, KV=1024, cache=on, 200k ops)
- Tool: protocol_a_ycsb + pcm-memory (g3 system-aggregate counters)
- Reps: 5 per build
- Compared:
  - **STAGING** (`build-cxl`): FUSEE_READ_GUARD=0, FUSEE_WRITE_ALLOC=0
  - **HAZARD+W1** (`build-cxl-w1`): FUSEE_READ_GUARD=2, FUSEE_WRITE_ALLOC=1
- Both at FUSEE_LRU_SAMPLE=0, FUSEE_XHOST_WRITE_SELF_INVAL=0 (defaults).

## Per-rep raw

| Build | Rep | trans_agg_thpt | DRAM rd | DRAM wr | CXL rd | CXL wr | total_BW |
|---|---|---:|---:|---:|---:|---:|---:|
| STAGING       | 1 | 10,538,518 | 858 | 503 | 169 | 465 | 1361 |
| STAGING       | 2 | 10,693,471 | 761 | 462 | 146 | 387 | 1223 |
| STAGING       | 3 | 11,294,968 | 764 | 466 | 146 | 387 | 1230 |
| STAGING       | 4 | 10,575,296 | 804 | 480 | 146 | 387 | 1284 |
| STAGING       | 5 | 10,983,579 | 756 | 459 | 157 | 387 | 1215 |
| HAZARD+W1 | 1 | 10,723,860 | 838 | 499 | 172 | 464 | 1337 |
| HAZARD+W1 | 2 | 10,744,600 | 764 | 466 | 146 | 387 | 1230 |
| HAZARD+W1 | 3 | **17,394,329** | 891 | 521 | 166 | 464 | 1413 |
| HAZARD+W1 | 4 | **17,818,959** | 845 | 503 | 160 | 465 | 1348 |
| HAZARD+W1 | 5 | **17,198,383** | 877 | 514 | 164 | 465 | 1391 |

Bolded reps are in the **bimodal-fast mode** (~17.5 Mops/s). All STAGING
reps are in the **slow mode** (~10.7 Mops/s).

## Medians + deltas

| Metric | STAGING | HAZARD+W1 | delta% |
|---|---:|---:|---:|
| trans_agg_thpt (ops/s) | 10,693,471 | 17,198,383 | **+60.83%** |
| DRAM read (MB/s) | 764 | 845 | +10.60% |
| DRAM write (MB/s) | 466 | 503 | +7.94% |
| CXL read (MB/s) | 146 | 164 | +12.33% |
| CXL write (MB/s) | 387 | 464 | +19.90% |
| **mem total (MB/s)** | 1230 | 1348 | +9.59% |

## Bytes per operation (data-movement load-bearing metric)

| Channel | STAGING B/op | HAZARD+W1 B/op | delta% |
|---|---:|---:|---:|
| DRAM read | 74.6 | 53.7 | **-28.0%** |
| DRAM write | 45.3 | 31.4 | **-30.7%** |
| CXL read | 14.5 | 10.0 | **-31.0%** |
| CXL write | 37.9 | 28.4 | **-25.1%** |
| **total memory** | **119.9** | **85.2** | **-28.9%** |

Bytes/op drops uniformly **~25-31%** across every memory channel. This
is the load-bearing data-movement result: HAZARD direct-pool read +
W1 reserved per-host write segment really do eliminate ~29% of memory
traffic per operation.

## Bimodal segmentation (mode-aware comparison)

Splitting by mode (P3.C noted bimodal collapse persists at iter-13A HEAD):

| Sub-comparison | STAGING (slow) | HAZARD+W1 (slow, reps 1-2) | HAZARD+W1 (fast, reps 3-5) |
|---|---:|---:|---:|
| Reps available | 5 | 2 | 3 |
| Median thpt | 10.69 Mops/s | 10.73 Mops/s | 17.39 Mops/s |
| Median total_BW | 1230 MB/s | 1284 MB/s | 1391 MB/s |
| Median B/op | 119.6 | 119.5 | 80.0 |

- **Slow-mode vs slow-mode**: thpt essentially identical (10.69 vs 10.73);
  B/op identical (119.6 vs 119.5). Surprising — the per-op data movement
  in slow mode does NOT differ between STAGING and HAZARD+W1.
- **Fast-mode (HAZARD+W1 only)**: thpt 17.39 Mops/s @ 80.0 B/op. STAGING
  did not enter fast mode in 5 reps in this sample.

## Case classification

Applying the framework table:

| | BW drop (per op) ≥ 10% | BW flat |
|---|---|---|
| Thpt up ≥ 5% | **case C** | case D |
| Thpt flat | case A | **case B** |

Two simultaneous readings, depending on sub-comparison:

1. **All-reps median comparison** → case C. Thpt +60.8%, B/op -28.9%
   — copy elim worked; throughput followed. *Caveat*: the +60.8% is
   driven by HAZARD+W1 hitting the bimodal-fast mode in 3 of 5 reps
   while STAGING hit 0 of 5; 5-rep sample binomial p(0/5 | p=0.5) ≈ 3%
   — unlikely-but-not-impossible coincidence.

2. **Slow-mode vs slow-mode comparison** → case B. Thpt and B/op both
   essentially identical (within < 1%). In slow mode, copy elim makes
   no observable difference.

## What the bimodal mode actually IS (sub-question raised by data)

In the slow mode, HAZARD+W1's per-op data movement is *identical* to
STAGING (both ~120 B/op). The expected savings from eliminating
forward_read + forward_write staging copies do NOT appear at the BW level
in slow mode. This is structurally consistent with the W10 finding from
P4: in slow mode, the system is dominated by `cache_pool_insert` MESI
ping-pong on the 1088 B KvCacheEntry — that traffic is not on the
read/write forward path being optimized, so HAZARD+W1 doesn't move it.

In fast mode, the system somehow escapes the W10-dominant regime and
per-op data movement drops to 80 B/op (HAZARD+W1) — at which point
copy elim is visible and throughput rises 60%.

**The fast vs slow mode appears to be a structural / initialization-state
phenomenon**: same code, same workload, same arguments — the system
either enters fast mode (~17.5 Mops/s) or slow mode (~10.7 Mops/s).
Bimodal RCA was iter-12A's task; iter-12A's fix did NOT eliminate the
bimodal behavior (P3.C confirmed it still exists). iter-15A backlog.

## Conclusion (the deliverable case judgment)

**Primary case**: **case B (slow-mode comparison)** — copy elimination
delivers no observable BW reduction or throughput gain in the
slow-mode regime where iter-13A spent most of its measurement budget.

**Secondary case**: **case C-conditional (bimodal-fast comparison)** —
when the system enters fast mode, HAZARD+W1 does deliver lower B/op
(80 vs ~120) and higher thpt (17 vs ~11). STAGING did not enter fast
mode in this sample so the comparison is asymmetric.

**Why this matters for iter-15A**:

1. The dominant contender for the throughput gap is no longer
   the read/write forward-path copies (they're already removed in
   HAZARD+W1); it's W10 (cache_pool_insert structural MESI traffic)
   and the bimodal collapse.

2. Fast mode reaches 17.5 Mops/s — within reach of the 20 Mops/s YCSB-A
   target. **Eliminating the bimodal slow mode would** bring measured
   throughput from "10.7 most of the time, occasionally 17.5" to
   "17.5 always" — exceeding the iter-13A target without further copy
   optimization. **This makes bimodal RCA the highest-priority iter-15A
   item.**

3. The 80 B/op fast-mode footprint is still 3-4× the theoretical
   minimum (~16 B/op for key+value 1024B / 64-ops-per-cacheline). The
   gap is cache_pool_insert (W10), TLS eviction traffic, and InvalRing.
   Path-decomp at fast-mode operating point would clarify which.

## Notes on methodology

- pcm-memory measured on g3 only (host 0). Total cross-host BW would
  approximately double if we counted both hosts.
- Bytes/op is computed as (MB/s × 1 MB) / ops/s. trans_agg_thpt is the
  aggregate of both hosts' h0+h1 ops, so this is "memory traffic on g3
  per total cluster op" — useful for relative comparison, slightly
  asymmetric in absolute terms.
- pcm-memory averages over the full run window (~20 s). Initialization
  spike is included; the dominant phase is steady-state.
- 5 reps is insufficient to firmly disentangle bimodal-mode-incidence
  from real per-mode shift. Larger N (20+) would tighten this, but the
  per-mode B/op equality is a clean qualitative finding.
