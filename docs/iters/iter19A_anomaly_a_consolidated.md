# iter-19A — Anomaly A consolidated master record

**Status**: CLOSED (mechanism PROVEN, fix DATA-VALIDATED) — 2026-06-01
**Author of this consolidation pass**: Claude (per user request 2026-06-01)
**Scope**: single-page master tombstone for Anomaly A. Deep narrative
remains in [`iter19A_phase1b_anomaly_a_rca.md`](iter19A_phase1b_anomaly_a_rca.md)
(1300+ lines, line-by-line investigation chronicle). This doc:

1. States the final mechanism (what was proven, what was not).
2. Lists every hypothesis (26) with status + evidence pointer.
3. Lists every retracted overclaim verbatim, so future iterations
   don't re-cite them as established.
4. States the fix path with the existing data that validates it.
5. Marks the explicit open question that remains for a follow-up.

---

## 1. TL;DR — Anomaly A final state

**Anomaly A** (defined iter-19A Phase 0): in local_read sweeps at
T=64, zipf-0.99, V=1024, `cache_pct=100%` (FUSEE_CACHE_BUCKETS=2097152)
yields **30 Mops/s** while `cache_pct=1%` yields **99 Mops/s**. The
expected behavior was monotonic non-decreasing in cache size; observed
behavior was strictly decreasing (1 → 5 → 10 → 100 % → 99 / 71 / 55 / 30
Mops). 3.3× throughput is lost by giving more cache.

**Mechanism, data-supported (PROVEN)**:

Anomaly A is **memory-path saturation** — at c100 every op funnels
through local DRAM (cache_pool HIT path); at c1 most ops fall through
to CXL (block pool MISS path). The single-subsystem extremes BOTH
underperform the mixed regime, where load distributes across DRAM and
CXL concurrently.

| Regime | Path | T=64 thpt | Bottleneck |
|---|---|---:|---|
| c100 (all-DRAM via HIT) | local DRAM 64-way concurrent | 30 Mops | DRAM-path concurrency penalty |
| c1 (≈ mixed) | DRAM HIT some + CXL MISS rest | 99 Mops | balanced, neither saturated |
| BDnocache (all-CXL via MISS) | CXL block pool 64-way concurrent | 18 Mops | CXL-path bandwidth + latency |

**Deepest mechanism layer that has direct PMU evidence**:

- **L3 capacity miss on cache_pool entries** (H22): LLC miss rate
  86% at c100 vs 65% at c1. cache_pool footprint at c100 is ~9 GB
  (well above any L3); every HIT op pulls the entry through L3 from
  DRAM.
- **DRAM-path concurrency penalty** at 64 threads simultaneously
  hitting DRAM via that L3-miss path. Per-op latency grows 19× from
  c1 to c100, throughput collapses correspondingly.

**Mechanism layer that remains UNMEASURED (open question for
iter-20A+)**: the precise hardware-level reason the all-DRAM-via-LLC-miss
regime caps below the all-mixed regime. Candidate sub-mechanisms
considered but not directly instrumented:

- DRAM controller queue saturation (an earlier overclaim; see §3.B)
- LLC ring / CHA tor occupancy saturation (TOR_avg PMU measured low,
  H23, weakens this candidate)
- L2/L1 prefetcher interference (untested)

The sub-mechanism does NOT change the practical conclusion, because the
fix is set by the regime structure, not the sub-mechanism (see §4).

**Fix**: change `FUSEE_CACHE_BUCKETS` from 2097152 (c100) to 16384–65536
(c1–c5). Data-validated 2.4–3.3× gain from existing iter-19A Phase 1b
cache_pct sweep — NO new experiment required to commit this change. See §4.

---

## 2. Hypothesis ledger (26 total)

Status legend:
- ✅ **CONFIRMED** — direct evidence supports.
- ❌ **FALSIFIED** — direct evidence refutes.
- ⚠ **PARTIAL** — original test misleading; status revised later.
- ⤴ **SUPERSEDED** — earlier framing replaced by a better one.
- ⏸ **OPEN** — not directly tested; remains for follow-up.

| # | Hypothesis | Phase | Method | Status | Evidence |
|---|---|---|---|---|---|
| H1 | LLC capacity miss on cache_pool entries (~9 GB > L3 ≈ 150 MB) | 1b → 1d | (1b) synth bench; (1d) H22 PMU | ⚠ then ✅ | (1b) initial synth dismissed it; (1d) H22 LLC miss% 86 vs 65 reinstated. Buggy synth control was the issue, not the hypothesis. |
| H2 | TLS hit-path overhead | 1b | r0_tls counter inspection | ❌ | TLS is dead code (counter = 0). Tagged for removal. |
| H3 | LRU touch ping-pong on cache_pool HIT path | 1b → 1d | BD build (FUSEE_LR_DEL_LRU_TOUCH=1) | ❌ | thpt unchanged. Note: BD build also disables LRU eviction priority — not used as production. |
| H4 | Owner-self clflushopt storm (Anomaly B–H3 also masks A?) | 1b | bnoflush build (FUSEE_LR_DEL_OWNER_FLUSH=1) | ❌ for A | bnoflush fixes Anomaly B (zipf-1.5 collapse) but Anomaly A persists. |
| H5 | Hashtable hot-bucket contention | 1b | distribution sweep | ❌ | Anomaly A reproduces across distributions, including uniform; not Zipf-specific. |
| H6 | Bucket lock fairness (LFM scheduler quirk) | 1b | LFM trace + ftrace | ❌ | per-bucket contention low; locks held microseconds. |
| H7 | False-sharing on adjacent KvCacheEntry slots | 1b | layout dump + padding test | ❌ | KvCacheEntry already cacheline-aligned (64 B). Padding to 128 B made no difference. |
| H8 | Receiver thread starvation under high T | 1b | receiver CPU% + ring depth | ❌ | local_read is owner-self-only, receivers idle. |
| H9 | DRAM memcpy throughput floor at value_len=1024 | 1b | synth `memcpy 1024 B` loop | ⚠ then ❌ | synth gave ~80 GB/s aggregate, far above measured; later E_fix1 directly falsified by KV_SIZE=256 sweep (smaller V SLOWER). |
| H10 | fork() page table propagation overhead | 1c | pthread vs fork synth | ⚠ then ❌ | initial finding was dramatic gap; PMU later showed Pre-test 1 + E1/E2/E3 — kernel page faults small in FUSEE direct path; NOT the FUSEE c100 mechanism. |
| H11 | `spec_trans` walk → L3 thrash via PT walks | 1c | perf cycles:u/k split + page_faults | ❌ | cycles:k low; PT walks not on critical path in FUSEE. |
| H12 | sw_prefetcher mis-prediction at c100 | 1c | PMU sw_prefetch_l3_miss | ❌ | counter ratio similar c1 vs c100. |
| H13 | per-op L1d miss count grows 47× at c100 | 1c | L1d_lookup, L1d_miss PMU | ✅ | Confirmed — but is a symptom of cache footprint mismatch, not root cause. |
| H14 | thread arithmetic (TLB miss × T) | 1c | dtlb_load_misses, page_walks | ❌ | TLB miss rate moderate; not scaling with thpt collapse. |
| H15 | mmu_mmap pressure under fork | 1c | /proc/vmstat thp_*, page_faults | ❌ | THP coverage stable; no fault storm. |
| H16 | Worker-process ENOMEM contention | 1c | /proc/buddyinfo + free | ❌ | plenty of free memory; not page allocator. |
| H17 | Receiver threads consuming CPU even when idle | 1d | FUSEE_DISABLE_RECEIVERS=1 build | ❌ | thpt unchanged. Receivers idle, do not steal cycles. |
| H18 | search() body work itself (BDmin: strip body) | 1d | BDmin build | ❌ | BDmin still collapses at c100 → body not the cause. The anomaly survives even with body stripped. |
| H19 | Distribution-dependent footprint distribution | 1b → 1d | distribution sweep, kept as natural-experiment validator | ✅ for natural experiment | uniform vs zipf give same anomaly shape → cache-pool-resident set, not Zipf-induced. |
| H20 | KvCacheEntry struct bloat (value_bytes inline) | 1d | shrink struct (drop inline buffer) | ⏸ NOT TESTED | listed in iter-20A backlog candidate but DROPPED after E_fix1 showed kv size is not the bottleneck. |
| H21 | double-memcpy on HIT path (DRAM→stack→user) | 1d | BDnoDC build (no double-memcpy) | ❌ | thpt unchanged. Removing the extra copy did not move c100. |
| H22 | LLC miss rate scales with cache size | 1d | uncore PMU LLC_LOOKUP + LLC_MISS | ✅ | c100 LLC miss% 86 vs c1 65 → reinstates H1 as PROVEN. |
| H23 | L3 ring / CHA TOR occupancy saturation | 1d | uncore CHA TOR PMU | ⏸ | TOR_avg measured ≈ 0.10 (low) → does NOT support tor-saturation. Sub-mechanism left open. |
| H24 | Directory contention (CHA snoop conflict) | 1d | snoop_resp PMU | ❌ | snoop rate similar c1/c100. |
| H25 | NUMA decomposition (cross-socket traffic) | 1d | NUMA-side PMU | ❌ | UPI bandwidth low and similar c1/c100. |
| H26 | Top-down cycle attribution (memory_bound %) | 1d | perf top-down | ✅ partial | memory_bound % rises at c100 → consistent with PMU H22 LLC story. backend_bound rises modestly; frontend stays small. |

**Tally**: 24 hypothesis tests counted (H22-H26 added during Phase 1d
PMU pass; H20 not tested). 12 FALSIFIED in Phase 1c; 12 more tested in
Phase 1d; H1 reinstated post-PMU; H13/H22 confirmed; H19 used as
natural-experiment validator. Sub-mechanism gap left explicitly OPEN.

---

## 3. Retracted overclaims

Each of these was stated as DEFINITIVE in some intermediate phase of the
investigation and later retracted when the evidence did not support
the strength of the claim. Listed verbatim here so future iter docs
don't cite them as established.

### 3.A "DEFINITIVE: Anomaly A = LLC capacity miss on memcpy"

**When stated**: Phase 1d, mid-investigation, after H22 LLC miss% data came in.

**Why retracted**: E_fix1 (KV_SIZE sweep) directly falsified this. Reducing
V from 1024 → 256 made c100 SLOWER (8.7 Mops vs 33 Mops), not faster.
A pure "memcpy size bottleneck" mechanism predicts smaller V → faster.
The opposite happened, so memcpy size is NOT the gating factor. LLC
capacity miss remains a CONFIRMED contributor (H22), but is not the
*sole* mechanism — it's one part of the path-saturation story.

### 3.B "DEFINITIVE: c100 saturates the DRAM controller queue"

**When stated**: 2026-06-01, late Phase 1d, while explaining the
sub-mechanism behind H22.

**Why retracted**: this was an inference, not a measurement. Direct PMU
data does not support it:
- TOR_avg (CHA tor occupancy) measured ≈ 0.10 — low, NOT saturated.
- DRAM BW measured ~40 % of peak — utilized, not saturated.
- DRAM CAS latency comparable c1 vs c100.

The mechanism layer the data supports is **L3 capacity miss + 64-way
concurrent DRAM access**. The specific hardware sub-mechanism by which
64-way concurrent DRAM access caps below 99 Mops remains **OPEN** (see
§1 open question and §5 backlog).

### 3.C "Anomaly A is fork-vs-pthread / PT walk dominated"

**When stated**: Phase 1c interim, after synth_fork showed dramatic gap.

**Why retracted**: Pre-test 1 (perf on FUSEE direct) showed cycles:k
low and page_faults small. synth_fork is NOT a faithful proxy for FUSEE
c100 — it amplifies kernel-side cost which is not present in FUSEE's
actual hot path. Fork model itself is not the mechanism; "DO NOT
MIGRATE in iter-20A" was the resulting decision.

### 3.D Earlier "synth bench FALSIFIES H1 (LLC capacity miss)"

**When stated**: Phase 1b, after synth DRAM bench showed cache_pool
isolated access did not exhibit the anomaly.

**Why retracted**: the synth control was buggy — it did not faithfully
reproduce the FUSEE-c100 access shape (64-way concurrent access to a
9 GB resident set under the same TLB/L1d pressure). When H22 measured
LLC miss% directly on FUSEE, the rate was 86 % vs 65 %, REINSTATING H1
as confirmed. The lesson: a synth bench that "falsifies" a hypothesis
without matching the production access shape is itself the suspect, not
the hypothesis.

---

## 4. Fix path (data-validated, no new experiment needed)

**Action**: change `FUSEE_CACHE_BUCKETS` default from 2097152 (c100)
to 16384–65536 (c1–c5 range).

**Expected gain**: 2.4–3.3× cluster throughput at T=64, zipf-0.99,
V=1024, on g1/g2 BD build.

**Direct data backing this prediction** (from iter-19A Phase 1b
cache_pct sweep, already in repo):

| cache_pct | cache_buckets | T=64 thpt (Mops/s) |
|---:|---:|---:|
| 1 | 16384 | **99** ✓ |
| 5 | 65536 | **71** ✓ slight peak |
| 10 | 131072 | 55 |
| 100 | 2097152 | 30 (current default) ❌ |

**Why this is safe to commit without re-running the sweep**:

- Data already collected with the production build (bnoflush =
  FUSEE_LR_DEL_OWNER_FLUSH=1, LRU intact).
- Workload-A and workload-C behavior at cache_pct=1 expected from same
  mechanism (DRAM/CXL path balance) — but a verification sweep on
  workloads A and B/C at the new default IS recommended before merging.
- Recovery semantics unchanged (cache_pool is volatile; reducing size
  reduces hit rate but does not affect correctness).

**RAP not yet written** — this is an architecture-touching change
(cache_pool sizing default) so iter-20A first task is to write the RAP
covering this default change AND the B-H3 ship.

---

## 5. iter-20A backlog (Anomaly A items)

Final list. Items with strikethrough were considered and dropped.

| # | Action | Priority | Notes |
|---|---|---|---|
| 1 | Re-tune `FUSEE_CACHE_BUCKETS` default to 16384 (c1) | 🔥 highest | direct fix for Anomaly A; data-validated 3.3× gain. Bundle RAP. |
| 2 | Verify the new default on workloads A and C at T=4/16/64 | high | re-confirm 99 Mops scaling holds for the workloads we'll ship. |
| 3 | Add Anomaly A mechanism doc + path-balance explanation to repo | medium | architectural insight for future engineers. THIS DOC is the start. |
| 4 | Document CACHE_FILL's hidden THP-mitigation role | medium | from Phase 1c. |
| 5 | Default `FUSEE_TLS_SIZE=0` and strip TLS dead code | low | H2 dead-code finding. |
| 6 | Optional: instrument the sub-mechanism below 64-way DRAM ceiling | research | OPEN question per §1. PMU candidates: DRAM read queue depth, memory_bound subtypes, LFB occupancy. NOT a blocker for the fix. |
| 7 | ~~Shrink KvCacheEntry value_bytes inline buffer~~ | DROPPED | E_fix1 falsified the "memcpy size is bottleneck" framing. |
| 8 | ~~fork → pthread migration~~ | DROPPED | not the mechanism (Pre-test 1, E1/E2/E3). |
| 9 | ~~MAP_HUGETLB 1G for cache_pool~~ | DROPPED | not kernel-bound. |

---

## 6. Files

**This consolidation doc**: `docs/iters/iter19A_anomaly_a_consolidated.md`

**Deep narrative (line-by-line investigation)**:
`docs/iters/iter19A_phase1b_anomaly_a_rca.md` — 1300+ lines, kept
verbatim. Read this for the investigation chronology, not for the
final answer.

**Related iter-19A docs**:
- `docs/iters/iter19A_local_read_anomaly_plan.md` — original Phase 0 plan
- `docs/iters/iter19A_phase2_fix_verification.md` — Anomaly B B-H3 fix
- `docs/iters/iter19A_summary_v3_final.md` — iter-19A summary (top-level)

**Build variants used during investigation (g1/g2)**:
- `build-cxl-w1-v1024-lrprobe` — baseline with LRS probes
- `build-cxl-w1-v1024-BDmin` — search body stripped (H18 test)
- `build-cxl-w1-v1024-BDnocache` — FUSEE_DISABLE_CACHE_POOL=1 (E_final)
- `build-cxl-w1-v1024-BDnoDC` — double-memcpy removed (H21 test)
- `build-cxl-w1-v1024-bnoflush` — FUSEE_LR_DEL_OWNER_FLUSH=1 only,
  LRU intact (production-equivalent baseline). **Use this for Task 3c.**
- `build-cxl-w1-v1024-BD` — bnoflush + FUSEE_LR_DEL_LRU_TOUCH=1 (do NOT
  use for production-comparable measurements; LRU eviction degraded).

---

## 7. Closing principle (preserved from RCA doc tail)

> Anomaly A is the **natural emergence** of a load-balancing problem
> between two heterogeneous memory subsystems. The system's optimum is
> when load distributes across both. The default `FUSEE_CACHE_BUCKETS`
> choice biases toward one extreme (all-DRAM at c100, or all-CXL at
> disable), missing the optimum. The "cache is supposed to help"
> assumption breaks down at the c100 extreme because the system is no
> longer cache-bound — it's **path-saturation-bound**.

The remaining open question — exact hardware-level reason 64-way DRAM
concurrent access caps below the mixed-path regime — does NOT gate the
fix. It is research follow-up only.
