# iter-19A summary v3 — local_read two-anomaly study, FINAL

**Date**: 2026-05-31
**Predecessor**: [iter19A_summary.md](iter19A_summary.md) (v1, partially wrong)
+ [iter19A_phase2_fix_verification.md](iter19A_phase2_fix_verification.md) (v2 correction)
+ this doc (v3 final integration)
**Plan**: [iter19A_local_read_anomaly_plan.md](iter19A_local_read_anomaly_plan.md)

---

## TL;DR (FINAL after mechanism isolation)

| Anomaly | iter-15A 假设 | iter-19A FINAL verdict |
|---|---|---|
| **A** cache_pct ↑ thpt ↓ | LLC pressure / working set 超 L3 | ⚠️ **PARTIALLY**: B-H3 fix 给小 cache +52 % 但大 cache 仅 +11 %; 大 cache 残留 anomaly 真因仍是 unknown, 推测为 cache_pool DRAM access cost. iter-15A "LLC pressure" 方向**很可能对**, 但**没被 confirm** (PMU 被 LOAD phase 污染). |
| **B** zipf-1.5 thpt 突崩 | (read 路径未明确) | ✅ **FULLY RESOLVED**: 100 % 来自 owner-self read 路径上的 `clflushopt(bucket) × 2` 引起的 CXL fabric serialization. 删除 = **5.9× thpt**, anomaly 消失. |

---

## Anomaly B — 完整 RCA

### 5 个 hypothesis 最终判决 (跨 Phase 2.1b / 2.4 / fix verification / flush-fence isolation)

| ID | 假设 | Verdict |
|---|---|---|
| B-H1 | cpool_insert thundering herd | ❌ REFUTED (LRS4R counter = 0) |
| **B-H2** | cache_pool entry LRU touch ping-pong | ❌ **REFUTED** — Build A (删 lru_epoch.store) thpt 0 % 变化; padding 0 %; sampling 0 % |
| **B-H3** | owner-self bucket flush storm | ✅ **CONFIRMED SOLE CAUSE** — Build B 5.9× 收益, isolation 显示 `clflushopt` (NOT mfence) 是唯一元凶 |
| B-H4 | LRU evict storm | ❌ REFUTED (cp_lru_evict counter constant across dist) |
| B-H5 | r2hit metric artifact | ⚠️ partially true (per-tid counter scope bias) but unrelated to anomaly |

### 真正机制 (Build B1 vs B2 隔离实测)

| Build | zipf-1.5 thpt | 改动 |
|---|---:|---|
| baseline | 16.4 Mops | `flush_line × 2 + mfence` kept |
| B1_no_flush_only | **110.9 Mops** | 删 `flush_line × 2`, 保留 `mfence` |
| B2_no_fence_only | 20.7 Mops | 保留 `flush_line × 2`, 删 `mfence` |
| B3 (B_no_flush) | 111.8 Mops | 全删 |

**B1 ≈ B3, B2 ≈ baseline** → **`clflushopt` 是唯一元凶, `mfence` innocent**.

### 物理机制

```cpp
// search() owner-self miss path
flush_line(bucket);                  // clflushopt bucket cacheline
flush_line((char *)bucket + 64);     // clflushopt bucket+64 cacheline
full_fence();                        // mfence (irrelevant per isolation)
for (s = 0; s < 7; s++) bucket->slots[s] ...  // load + scan
```

`clflushopt` 在 hot bucket 上 invalidate cacheline from **ALL cores**.
zipf-1.5 下 128 worker (g1+g2, 64 each) 同时持续在同一 bucket cacheline 上执行 `clflushopt + load`:
- 每个 `clflushopt` 触发 cross-host CXL invalidation broadcast
- 后续 `load` 必须从 CXL fetch (cache 已被 invalidate)
- 128 worker 的 fetch request 在 CXL fabric 排队串行

**Phase 2 Exp 2 T-scaling 印证此模型**:

| T | baseline | B_no_flush | gain |
|---:|---:|---:|---:|
| 8 | 10.9 Mops | 62.2 Mops | +472% |
| 16 | 13.0 | 89.7 | +588% |
| 32 | 15.7 | 113.6 | +625% |
| **64** | 16.4 | 112.3 | **+582% plateau** |

gain 增涨然后 plateau → fabric serialization 在 T=32-64 间饱和.

### Anomaly B 最终 verdict

**Mechanism**: `clflushopt(bucket)` × 2 on owner-self read MISS path
**Magnitude**: 5.9× zipf-1.5 thpt with removal
**Sharding invariant guarantees flush is unnecessary**:
- key K 的 owner 是唯一写 bucket(K) 的 host (writes route via OP_WRITE_FORWARD → owner's WriteReceiver)
- 同 host MESI 自动维持 read-after-write coherence
- flush is 防 cross-host write 但 owner-self path 没 cross-host write 可能

### 修复方案

**Patch (3 line removal)**:
```cpp
// src/cxl_kv_ops_A.cc :: search() — owner-self miss path
//   REMOVE:
//     flush_line(bucket);
//     flush_line((char *)bucket + 64);
//     full_fence();
//   For owner-self path only. Cross-host miss path (forward_read) unchanged.
```

**RAP §XIII required**:
- Touches Protocol A §I (CXL "always flush before load") invariant
- Same pattern as iter-17A bucket_double_flush_removal (+36-37 % write path)
- 6 AV: PERFORMANCE +5.9×; CORRECTNESS via sharding invariant; GENERALITY only owner-self path; COMPLEXITY 3 LOC; PRIOR ART iter-17A; FEASIBILITY trivial.

---

## Anomaly A — incomplete RCA

### Current status

Phase 1.1 PMU 反驳了 iter-15A "LLC pressure / dTLB pressure" 假说. **但 measurement 被 LOAD phase 污染** (TRANS < 1 % of perf-stat window), 所以 verdict 是 **inconclusive**.

Phase 1 Exp 1 (this iter) 跑 B_no_flush × cache_pct sweep:

| cache% | baseline | B_no_flush | gain | gap remaining |
|---:|---:|---:|---:|---:|
| 1 | 65.28 | **99.62** | +52.6% | (this is the speed reference) |
| 5 | 54.68 | 71.57 | +30.9% | -28.1 (vs c=1 speed) |
| 10 | 46.34 | 55.68 | +20.2% | -43.9 |
| 50 | 31.96 | 38.82 | +21.4% | -60.8 |
| 100 | 26.77 | **29.78** | **+11.3%** | **-69.8** |

**关键 finding**:
- B-H3 fix 在小 cache 帮 52 %, 大 cache 仅 11 %
- baseline c100/c1 = 0.410 (current Anomaly A)
- B_no_flush c100/c1 = **0.299** (anomaly **MORE pronounced** after flush fix)

**逻辑解释**:
- flush storm 在 miss path → 小 cache 高 miss rate → 修后受益大
- 大 cache 低 miss rate → flush 几乎不触发 → 修后受益小
- flush 之前**partially masked the underlying Anomaly A**

### 真因 hypothesis (待 iter-20A 验证)

**A-H5 (NEW)**: cache_pool 总占用 (cache_pct=100% = 8.7 GB) 远超 g1/g2 L3 (344 MB) → 每次 HIT 的 `cache_pool_lookup` 17-cacheline memcpy 从 DRAM 读 → DRAM latency dominates → thpt drops.

**支持证据**:
- B_no_flush c1 = 99.6 Mops, c100 = 29.8 Mops (3.3× drop)
- 没 flush 干扰下, cache_pool_lookup 路径占比变大 → DRAM cost 显露
- iter-15A "LLC pressure" 方向是对的, 但需要 isolated PMU 才能 confirm

**反驳证据**:
- Phase 1.1 PMU 显示 LLC miss% 几乎不变 (63.7 → 63.1)
- BUT measurement 被 LOAD 污染, 不能下定论

### iter-20A Anomaly A 实验提议

**Exp A2 — isolated TRANS-phase PMU**:
- 方案 a: 增大 TRANS_OPS (5M → 100M+) 让 TRANS phase 主导 perf-stat 窗口
- 方案 b: 修 bench 把 LOAD 跟 TRANS 拆成两个 binary, perf 只 attach TRANS
- 方案 c: code inject `perf_event` syscall 把 TRANS 入口/出口标记出来, perf record 后处理过滤

**Exp A3 — synthetic DRAM bandwidth probe**:
- 写 ~100 LOC 程序: alloc N MB, 64 thread random-read 1088 B records
- 跑 N ∈ {64 MB, 256 MB, 1 GB, 4 GB, 8 GB}
- 看 thpt 曲线是否跟 FUSEE Anomaly A 形状一致
- 一致 → confirmed pure DRAM/LLC effect, FUSEE 无 fix 空间 (只能减 working set)
- 不一致 → FUSEE 还有别的机制

**两个一起做最稳**. Exp A2 给 PMU 证据, Exp A3 给 baseline comparison.

---

## Phase 1.1 PMU 数据 (留作 iter-20A reference)

7 cache_pct × 3 reps, full-binary perf stat:

| cache% | thpt | IPC | LLC miss% | dTLB miss% |
|---:|---:|---:|---:|---:|
| 1 | 65.12 | 0.026 | 63.70 | 0.0755 |
| 10 | 47.11 | 0.034 | 58.12 | 0.0524 |
| 100 | 25.76 | 0.077 | 63.12 | 0.0225 |

**Caveat**: LOAD-dominated; do not infer mechanism from this. Used as iter-20A starting point only.

---

## Phase delivery audit (per CLAUDE.md precedent #3)

| Phase | Plan | Delivered | Status |
|---|---|---|---|
| Phase 0 | baseline reproduction | ✅ ratio_A=0.387, ratio_B=0.346 match iter-15A within 2-11 % | ✅ FULL |
| Phase 1.1 PMU | A-H1 / A-H2 attribution | ⚠️ Done but inconclusive due to LOAD-dominated PMU; documented + iter-20A backlog | ⚠️ PARTIAL |
| Phase 1.2 layout sweep | entries/bucket variation | ❌ NOT DONE | ❌ DEFERRED → iter-20A |
| Phase 1.3 hugepages | hugepages experiment | ❌ NOT DONE | ❌ DEFERRED |
| Phase 1.4 synthetic bench | DRAM bandwidth probe | ❌ NOT DONE | ❌ DEFERRED → iter-20A Exp A3 |
| Phase 1.5 Phase 1 RCA | Anomaly A verdict | ⚠️ Partial verdict; flush partial fix, remainder unknown | ⚠️ PARTIAL |
| Phase 2.1 env split | misdiagnosis | ✅ (replaced by 2.1b) | ✅ (subsumed) |
| **Phase 2.1b** | true no-cache build | ✅ confirms hashtable path contributes | ✅ FULL |
| Phase 2.2 CAS retry counter | LRS2R + LRS4R | ✅ counters added, both = 0 (REFUTES H1/H2) | ✅ FULL |
| Phase 2.3 dense zipf sweep | tipping point | ❌ NOT DONE | ❌ DEFERRED |
| **Phase 2.4** | LRS stage decomp | ✅ probe framework + 4-dist sweep + on-host analyzer | ✅ FULL |
| Phase 2.5 LRU evict counter | check B-H4 | ✅ cp_lru_evict constant → B-H4 REFUTED | ✅ FULL |
| Phase 2.6 r2hit audit | metric artifact check | ✅ identified per-tid scope bug → B-H5 partial | ✅ FULL |
| **Phase 2.7** | Phase 2 synthesis | ✅ v3 doc covers everything | ✅ FULL |
| **NEW: Mechanism isolation** | 5 fix candidate verification | ✅ Build A/B/C/D/E + B1/B2/T-scaling = full verification | ✅ FULL |
| Phase 3 | iter-20A backlog | ✅ this doc | ✅ FULL |

**Deferred items have concrete next-iter plans** (Phase 1.2/1.3/1.4 → iter-20A Exp A2/A3).
**No silent descope**.

---

## Cumulative commit list

1. `2680dd5` [iter19A][plan] anomaly study plan
2. `af1c7c8` [iter19A][phase2.4][I9] LRS stage decomp probes + retry counters
3. `(Phase 0)` baseline reproduction
4. `(Phase 2.1b + 2.4 v2)` initial RCA (later corrected)
5. `[iter19A][phase2-fix-verify] B-H2 REFUTED, B-H3 sole cause (5.9x)` mechanism isolation
6. `[iter19A][phase2-iso+phase1-exp1]` flush vs mfence + anomaly A partial RCA
7. This doc commit (v3 final summary)

---

## iter-20A backlog (initial v3 draft — superseded below)

(Kept for historical context; the Phase 1c update at the end of this file
replaces this list.)

### Tier 1 — ship Anomaly B fix

1. **Write RAP §XIII** for removing `flush_line × 2 + full_fence` in owner-self read path
2. **Ship the patch** (3 LOC removal in `src/cxl_kv_ops_A.cc :: search()`)
3. **Smoke**: hash-diff + 4-dist sweep + cross-host correctness verification
4. **Expected gain**: 5.9× zipf-1.5, +20-50 % zipf-0.99/0.5, +1-10 % uniform

### Tier 2 — finish Anomaly A RCA  *(EXECUTED as Phase 1c — see update below)*

1. **Exp A2 isolated TRANS-phase PMU**:
   - Increase TRANS_OPS so TRANS dominates perf-stat window (need 200M+)
   - OR split bench into LOAD + TRANS binaries
   - Run 7 cache_pct × 3 reps, capture LLC + dTLB
2. **Exp A3 synthetic DRAM bench**:
   - 100 LOC C++: alloc N MB × 64 threads × random 1088 B reads
   - 5-7 working set sizes from 64 MB to 16 GB
   - Compare curve shape vs FUSEE Anomaly A
3. **If Exp A2/A3 confirm LLC capacity**: iter-15A LLC pressure hypothesis vindicated. **Fix candidates**: hugepages (Phase 1.3), smaller cache_pool entries, multi-level cache hierarchy

### Tier 3 — investigate residual LRS2 HIT slowdown

Even after B-H3 fix, `cache_pool_lookup` HIT at zipf-1.5 stays at 6 µs (Phase 2 fix verification §"Why LRS2 HIT stays slow"). Probably cache_pool_insert vs reader interference. Targeted experiment needed.

### Tier 4 — DROP

- ❌ FUSEE_LRU_SAMPLE (iter-14A F2 rollback was right)
- ❌ FUSEE_LRU_PAD (cacheline separation gives 0 benefit)
- ❌ B-H2-direction fixes in general

---

## 2026-06-01 Phase 1c update — Tier 2 Anomaly A executed; RCA & backlog rewritten

Tier 2 above (Anomaly A residual RCA via TRANS-PMU + synthetic DRAM bench)
**was executed in full** as iter-19A Phase 1c. Full experiment data,
hypothesis matrix, and per-test verdicts are in
[iter19A_phase1b_anomaly_a_rca.md](iter19A_phase1b_anomaly_a_rca.md). All
sweep dirs are under `docs/iter19A_phase1c_*` (one per experiment).

### Anomaly A — Phase 1c findings condensed

**12 of 13 hypotheses ruled out**:

| # | Hypothesis | Primary refutation |
|---|---|---|
| H1 | LLC capacity miss | Synthetic DRAM bench: working set ↑ thpt ↑ (210 → 290 Mops c1 → c100) |
| H2 | TLS overhead | r0_tls = 0; H12 disabling TLS gives -1.6% |
| H3 | LRU touch ping-pong | BD build: -14% p50, 0% thpt change |
| H4 | Probe ring writes | BD-nopr build: 0% change |
| H5 | B-H3 flush storm (as Anomaly A residual) | Already removed in bnoflush; residual persists |
| H6 | First-touch page fault | M1 pre-touch: 0% change |
| H7 | THP collapse events | THP=never: ratio unchanged (5% to single cell) |
| H8 | PT walk hits DRAM | walk time only 0.5% of total cycles either model |
| H9 | Kernel page fault storm (synth_fork) | **Pre-test 1: FUSEE kernel% = 4.5%, not 97%** |
| H10 | first_op barrier variance | M1 pre-touch also pre-faults; no effect |
| H11 | spec_trans 80 MB walk → L3 thrash | Pre-test 2 pre-slice: +1.7% only |
| H12 | TLS branch always-fall-through cost | TLS=0: -1.6% (noise) |

**H13 PROVEN as proximate mechanism, source UNIDENTIFIED**:
FUSEE per op touches **20× more cache lines** than pthread synth at the
same workload (47 L1m vs 2.3, 32 L2m vs 1.6, 2.77 L3m vs 0.13 at c100
T=64). Math: memory wait 1.7 µs/op + compute 2 µs/op = 3.7 µs ≈ measured
r_avg 3.61 µs. The SPECIFIC code path that contributes the extra 45 L1
misses per op was not isolated in this phase.

### Why synth_fork misled us on the mechanism

Synth_fork c100 T=64 shows **97% kernel time + 4M minor page faults**
because khugepaged fails to coalesce 2M hugepages when 64 forked workers
simultaneously touch a 9 GB shared anon mmap. This IS a real cliff for
the fork + shared + large-mmap pattern.

**FUSEE accidentally avoids it** via protocol_a_ycsb's primary post-fork
worker doing a **sequential 9 GB CACHE_FILL** phase before TRANS starts.
During CACHE_FILL only one process writes, khugepaged has time + low
contention to coalesce 2M pages. By TRANS start, hugepages are formed and
the fault storm doesn't materialize → FUSEE kernel% stays at 4.5%.

This means **CACHE_FILL is a hidden THP-mitigation that future refactors
must preserve**. If someone "optimizes" by lazifying or splitting it,
FUSEE will fall off the synth_fork cliff. iter-20A should add a defensive
comment near `cache_pool_init` documenting this implicit role.

### Methodological lesson — synth is for hypothesis, not confirmation

The Phase 1c Pre-test 1 PMU on FUSEE itself caught that synth_fork's
mechanism doesn't apply to FUSEE BEFORE we committed to a 200-400 LOC
fork → pthread refactor based on the synth's 36× gap. The Phase 1c work
was therefore high-value not because it solved Anomaly A (it didn't),
but because **it prevented a misdirected major refactor**. Synth-as-proxy
needs proxy verification before driving engineering decisions.

### Revised iter-20A backlog (replaces the 4-tier list above)

| # | Action | Priority | Source |
|---|---|---|---|
| 1 | **Ship B-H3 fix via RAP** (3 LOC owner-self flush removal) | high | Tier 1 above, unchanged |
| 2 | **Isolate H13's 20× cache-miss source** in FUSEE `search()` HIT path | **🔥 highest** | NEW. Method: per-segment `__rdtsc()` + L1 miss PMU; or strip one wrapper at a time and re-measure. Half-day work + small src instrumentation. |
| 3 | **Document CACHE_FILL's hidden THP-mitigation role** at `cxl_cache_pool.cc::cache_pool_init` | medium | NEW. ~5 LOC defensive comment; prevents future regression onto the synth_fork cliff |
| 4 | Ship `FUSEE_TRANS_PRESLICE=1` default (already in code) | low | NEW. Marginal +1.7%, no downside |
| 5 | Default `FUSEE_TLS_SIZE=0` + drop `FUSEE_LRU_SAMPLE` / `FUSEE_LRU_PAD` | low | Tier 4 above, expanded with TLS-disable from Phase 1c |
| 6 | LRS2 HIT slowdown investigation | low | Tier 3 above, unchanged but lower priority than H13 |
| 7 | ~~fork → pthread worker migration~~ | **DROPPED** | NEW. synth_fork proxy doesn't match FUSEE; bad ROI |
| 8 | ~~MAP_HUGETLB 1 GB pages~~ | **DROPPED** | NEW. Was a kernel-fault patch; FUSEE isn't kernel-bound |

### iter-19A complete close-out

- **Anomaly B**: fully RCA'd, fix verified, ship pending (backlog #1).
- **Anomaly A**: symptom characterized + proximate mechanism identified
  (20× cache miss per op) + 12 alternative hypotheses ruled out;
  specific source code path **not yet isolated**, iter-20A top priority.
- **Bonus deliverable**: hidden THP-mitigation role of CACHE_FILL
  documented (backlog #3).
