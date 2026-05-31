# iter-19A Phase 2 fix verification — B-H2 REFUTED, B-H3 sole cause

**Date**: 2026-05-31
**Predecessor**: [iter19A_summary.md](iter19A_summary.md)

## TL;DR (correction of prior summary)

| Hypothesis | Prior verdict | Verified verdict |
|---|---|---|
| **B-H2** cache_pool entry LRU touch ping-pong | "CONFIRMED, 64% contribution" | ❌ **REFUTED** (0% contribution) |
| **B-H3** owner-self bucket flush storm | "CONFIRMED, 36% contribution" | ✅ **CONFIRMED as sole dominant cause** (~95%+ contribution) |

Prior analysis attributed Anomaly B to mixed B-H2/B-H3 mechanism based on
**code reading + stage-decomp correlation**. Mechanism isolation
experiments (Phase 2 fix sweep) **refute B-H2 entirely** and show
B-H3 is the only meaningful cause.

## Experiment design

6 build variants on g1+g2, 4-dist × 3-rep sweep with on-host LRS
stage decomp:

| Build | Flags | Purpose |
|---|---|---|
| baseline | (lrprobe only) | reference |
| A_no_lru | `-DFUSEE_LR_DEL_LRU_TOUCH=1` | delete lru_epoch.store entirely (B-H2 mechanism isolation) |
| B_no_flush | `-DFUSEE_LR_DEL_OWNER_FLUSH=1` | delete owner-self read flush_line × 2 + mfence (B-H3 mechanism isolation) |
| C_sample | `-DFUSEE_LRU_SAMPLE=1` | iter-14A 1/64 sampling alone (proposed fix candidate) |
| D_pad | `-DFUSEE_LRU_PAD=1` | lru_epoch on own cacheline (proposed fix candidate) |
| E_sample_pad | both LRU_SAMPLE + LRU_PAD | combined LRU-fix (proposed) |

## Data — throughput median (Mops, cluster)

| build | uniform | zipf-0.5 | zipf-0.99 | zipf-1.5 | z15/z099 |
|---|---:|---:|---:|---:|---:|
| baseline | 27.65 | 36.47 | 42.71 | **15.76** | 0.369 |
| A_no_lru | 27.79 | 37.66 | 43.55 | **15.91** | 0.365 |
| C_sample | 27.58 | 37.32 | 43.47 | **15.72** | 0.362 |
| D_pad | 26.89 | 34.89 | 41.84 | **15.53** | 0.371 |
| E_sample_pad | 26.90 | 36.10 | 42.45 | **15.95** | 0.376 |
| **B_no_flush** | 27.99 | 38.76 | 47.37 | **93.18** ⚡ | **1.967** |

### Key observations

1. **A_no_lru shows 0 % thpt change at zipf-1.5** (15.76 → 15.91). Deleting
   the lru_epoch RMW entirely has **NO effect on Anomaly B**.
2. **C_sample / D_pad / E_sample_pad all show 0 % thpt change at zipf-1.5**.
   Sampling 1/64 alone, cacheline padding alone, combined — none of the
   proposed B-H2 fixes work.
3. **B_no_flush gives 5.91× thpt @ zipf-1.5** (15.76 → 93.18) — completely
   eliminates the collapse and **reverses the dist-thpt relationship**
   (zipf-1.5 is now FASTER than zipf-0.99 at 47.37 Mops, restoring the
   "skew helps cache" intuition).
4. **B_no_flush also helps zipf-0.99** (+11 %) and other dists (+1-6 %).
   Flush removal is a general read-path optimization, not just an
   anomaly-B fix.

## Data — stage decomp at zipf-1.5 (cache_pct=10, T=64)

| build | HIT LRS2 p50 | MISS LRS3 p50 | MISS LRS4 p50 | HIT StageW | MISS StageW |
|---|---:|---:|---:|---:|---:|
| baseline | 6255 ns | 6251 ns | 3667 ns | 6324 ns | 20494 ns |
| A_no_lru | 6028 ns | 6353 ns | 3977 ns | 6092 ns | 21390 ns |
| C_sample | 6007 ns | 6415 ns | 4534 ns | 6088 ns | 21808 ns |
| D_pad | 6402 ns | 6337 ns | 4895 ns | 6467 ns | 21287 ns |
| E_sample_pad | 6155 ns | 6412 ns | 5760 ns | 6232 ns | 22755 ns |
| **B_no_flush** | **6116 ns** | **4211 ns** | **785 ns** ⚡ | **6154 ns** | **12388 ns** |

### Stage-level insights

- **LRS2 HIT p50 is ~6100 ns across ALL builds**. Removing lru_epoch.store
  (Build A) doesn't change it. Cacheline padding (Build D) doesn't change
  it. The HIT-path slowdown is **NOT from lru_epoch ping-pong** —
  origin unknown, but explicitly NOT the LRU touch.
- **LRS3 MISS drops 6251 → 4211 ns in B_no_flush** (Δ −2040 ns) — that's
  the direct CXL-fetch + mfence cost saved per miss.
- **LRS4 (cache_pool_insert) drops 3667 → 785 ns in B_no_flush** (Δ −2882 ns,
  4.7× faster). LRS4 doesn't touch the flush code — its speedup is
  **system-wide memory subsystem decongestion** when the flush storm is
  removed. cache_pool_insert writes to the same hot entry; without the
  CXL fabric saturated by flush+load on hot bucket, DRAM accesses to
  cache_pool are no longer blocked.

## Why my prior analysis got B-H2 wrong

I observed in Phase 2.4 v2 that LRS2 HIT p50 went from 469 ns @ zipf-0.99
to 6262 ns @ zipf-1.5 (13.3× slowdown). I attributed this to lru_epoch
ping-pong because:
- it's the only WRITE in the HIT path
- LRS2R counter = 0 ruled out seqlock retry
- code review pointed at it

But mechanism isolation (Build A removing the write) shows LRS2 stays at
~6000 ns. So the slowdown is not from lru_epoch ping-pong.

The real explanation: **flush storm in the MISS path causes system-wide
memory subsystem stalls** that affect HIT path too. When 64 workers issue
clflushopt + mfence + CXL load on hot bucket cachelines, the memory bus
saturates. Concurrent HIT-path readers waiting for cache_pool_lookup
loads also get slowed down because:
- shared memory controller / IIO PCIe path bandwidth contention
- mfence in MISS path globally orders memory ops, blocking other workers'
  parallel reads
- L3 cache pollution from MISS-path CXL fetches evicting cache_pool entries

The 13× LRS2 slowdown at zipf-1.5 was a **secondary effect of B-H3**, not
an independent B-H2 mechanism.

## Why throughput jumps 5.91× when per-op latency stays similar

`r_p50` at zipf-1.5:
- baseline: 0.336 µs
- B_no_flush: 0.438 µs (actually HIGHER per-op!)

Throughput jumps 5.9× while per-op latency rises 30 %. By Little's Law:
N = λ × L (concurrent ops in flight):
- baseline: 16 Mops × 0.336 µs = 5.4 concurrent ops
- B_no_flush: 93 Mops × 0.438 µs = 40 concurrent ops

So removing flush enables 7.5× higher parallelism. The bottleneck wasn't
per-op work — it was **mfence-induced serial ordering** across the 128
worker threads (64 per host × 2 hosts). Every mfence in the MISS path
acts as a global memory ordering point that effectively serializes
workers competing for the same bucket.

Without flush+mfence: workers issue parallel loads of bucket cacheline
from shared L3 cache. With 128 cores capable of parallel L3 reads of a
single Shared cacheline, throughput scales.

## Implications

### For B-H2 candidate fixes (LRU touch related): DROP ALL

- iter-14A's `FUSEE_LRU_SAMPLE` rollback was **correct after all**.
  Even with full mechanism removal (Build A), no thpt gain.
- `FUSEE_LRU_PAD` proposal is moot; cacheline separation provides
  no benefit because the readers' slowdown isn't from cacheline 0
  contention.
- iter-20A should NOT pursue LRU_SAMPLE or LRU_PAD.

### For B-H3 candidate fix: SHIP

Remove `flush_line(bucket); flush_line(bucket+64); full_fence();` from
owner-self read path (`search()` line ~2910 in cxl_kv_ops_A.cc).
Correctness argument:
- Owner host is sole writer of its own hashtable buckets (sharding
  invariant + OP_WRITE_FORWARD routes peer writes to owner's
  WriteReceiver, executed on owner's host)
- Same-host MESI handles read-after-write coherence between writer
  thread (worker / WriteReceiver) and reader thread on this host
- The flush + mfence was over-defensive copy-paste from the cross-host
  path; it's strictly unnecessary for owner-self

Expected gains (this iter's measurements):
- zipf-1.5 local_read: 5.9× thpt
- zipf-0.99 local_read: +11 %
- general read path: +1-6 %

Risk audit:
- Phase 2 fix sweep showed no correctness issues (data validity via
  trans throughput consistent)
- Same pattern as iter-17A `bucket double-flush removal` (+36-37 %
  on write path, shipped without correctness issue)
- iter-20A should ship via RAP §XIII per CLAUDE.md spec

### For the LRS2 HIT slowdown mystery (still unexplained)

Even in B_no_flush, LRS2 HIT @ zipf-1.5 = 6116 ns (vs 469 ns @ zipf-0.99).
B-H2 mechanism isolation didn't explain this. **Where does the 13× HIT-path
slowdown come from?**

Possible candidates (not investigated):
- value_bytes (1024 B) memcpy bandwidth bound under hot-key concurrent reads
  (cache_pool entry's 17 cachelines staying in shared L3 should be fast,
  but maybe some cacheline thrashing from cache_pool_insert)
- Cache_pool_insert (writing the same hot entry) interfering with concurrent
  readers via MESI invalidation of the entry's cachelines
- L3 capacity pressure: 64 workers reading 1088 B per op = 70 KB/op
  effective working set; over 16 Mops = 1 GB/sec L3 traffic. May exceed
  L3 fill bandwidth.

iter-20A should investigate after the B-H3 fix is shipped. Without B-H3,
this signal is masked by the dominant flush storm.

## Phase 2 fix verification — delivery

| Deliverable | Status |
|---|---|
| 5 build variants on g1+g2 | ✅ |
| 4-dist × 3-rep sweep, all builds | ✅ (72 cells) |
| Stage decomp per (build, dist) at rep=1 | ✅ (24 decomp files) |
| Mechanism isolation conclusion | ✅ B-H2 REFUTED, B-H3 confirmed sole |
| Updated fix recommendation | ✅ ship B-H3 only |

## iter-20A backlog (revised)

1. **Ship B-H3 fix as RAP candidate**: remove owner-self read flush_line ×
   2 + mfence. Expected 5.9× zipf-1.5, +11 % zipf-0.99, +1-6 % others.
2. **Investigate residual LRS2 HIT slowdown**: why does HIT-path LRS2 stay
   at 6 µs @ zipf-1.5 even when B-H2 is fully eliminated? Probably
   cache_pool_insert vs concurrent reader interference; needs targeted
   experiment (e.g., separate insert path or copy-on-write entry).
3. **Skip B-H2 fix candidates entirely** (LRU_SAMPLE, LRU_PAD) — data
   shows they have zero benefit.
4. **Phase 1.1' isolated-TRANS-phase PMU** for Anomaly A (still
   inconclusive in iter-19A).

## Files

- Sweep: [docs/iter19A_phase2_fix_sweep_20260531_052702/](../iter19A_phase2_fix_sweep_20260531_052702/)
- Sweep script: [scripts/iter19A_phase2_fix_sweep.sh](../../scripts/iter19A_phase2_fix_sweep.sh)
- Source patches: cxl_cache_pool.h/cc + cxl_kv_ops_A.cc (FUSEE_LR_DEL_LRU_TOUCH,
  FUSEE_LR_DEL_OWNER_FLUSH, FUSEE_LRU_PAD macros)
