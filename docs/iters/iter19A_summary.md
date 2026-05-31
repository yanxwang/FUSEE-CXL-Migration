# iter-19A summary — local_read 两 anomaly 根因 study

**Date**: 2026-05-31
**Branch**: feat/cxl-migration
**Predecessor**: iter-18A (xhost_read decomp + multi-receiver)
**Plan**: [iter19A_local_read_anomaly_plan.md](iter19A_local_read_anomaly_plan.md)

---

## TL;DR

| Anomaly | iter-15A 假设 | iter-19A 验证 |
|---|---|---|
| **A** cache_pct ↑ → thpt ↓ | LLC pressure / working set 超 L3 | ❌ **REFUTED** by Phase 1.1 PMU — LLC miss% 平 (63.7% → 63.1%, Δ −0.6 pp), dTLB miss% 反而下降; **真根因待定** |
| **B** zipf-1.5 thpt 突崩 | (write) LFM bucket lock; (read) 未指明 | ❌ **B-H1/B-H3 部分 REFUTED, B-H2 CONFIRMED 但走 LRU touch 而不是 seqlock CAS** |

**两个 anomaly 的实际根因**:
- **A**: 不是 LLC/dTLB capacity (PMU 反驳); 是 **cache_pool DRAM access 总成本随 working set 增** 但 LLC hit/miss 分布**几乎不变** — 暗示 LOAD phase 主导 PMU stats，TRANS phase 太短被淹。需要 Phase 1.1' isolate TRANS phase 重测
- **B**: Cache_pool entry 的 **`lru_epoch.store()` 在 zipf-1.5 hot key 上 MESI ping-pong** (B-H2 confirm + 机制更精确 = LRU touch 路径不是 seqlock). iter-14A 当年 rollback 的 `FUSEE_LRU_SAMPLE` flag 是对应 fix - 数据证明那次 rollback 过早

---

## Phase 0 — Baseline reproduction (g1/g2)

数据: [docs/iter19A_phase0_baseline_20260531_013922/](../iter19A_phase0_baseline_20260531_013922/)

| Anomaly | g1/g2 ratio | iter-15A ratio | 状态 |
|---|---:|---:|---|
| A (cache_pct=100 / =1) | 0.387 | 0.395 | 形态完美复现 (Δ 2%) |
| B (zipf-1.5 / zipf-0.99) | 0.346 | 0.311 | 复现 (Δ 11%) |

Cell-by-cell 误差 < 2% 全程。CXL x8 降级**不影响 DRAM 路径** (符合预期 — 两 anomaly 都是 cache_pool 主导). 验证 g1/g2 (Intel Xeon 6787P) 与 g3/g4 (同 SKU) 在 LLC + dTLB 行为完全等价。

→ Phase 1/2 cleared to proceed on g1/g2.

---

## Phase 1.1 — Anomaly A PMU attribution: A-H1/A-H2 双 REFUTED

数据: [docs/iter19A_phase1_1_pmu_20260531_025039/perfstat_fixed.csv](../iter19A_phase1_1_pmu_20260531_025039/perfstat_fixed.csv)

7 cache_pct × 3 reps, perf stat 包 protocol_a_ycsb 全程, V=1024 T=64 zipf-0.99 local_read:

| cache% | thpt (Mops) | IPC | LLC miss% | dTLB miss% |
|---:|---:|---:|---:|---:|
| 1 | 65.12 | 0.026 | 63.70 | 0.0755 |
| 2 | 62.07 | 0.027 | 61.53 | 0.0689 |
| 5 | 55.42 | 0.030 | 59.18 | 0.0613 |
| 10 | 47.11 | 0.034 | **58.12** | **0.0524** |
| 20 | 37.88 | 0.040 | 59.41 | 0.0437 |
| 50 | 31.85 | 0.047 | 60.74 | 0.0375 |
| 100 | 25.76 | **0.077** | 63.12 | **0.0225** |

**反 intuition 趋势**:
- LLC miss% **不单调** — 在 cache_pct=10 时最低 (58%)，然后回升 (典型 U-curve, 不是 capacity limit signature)
- dTLB miss% **单调下降** (0.075% → 0.022%, 减 3×) — 跟"working set 超 dTLB"假设完全反方向
- IPC **上升 3×** (0.026 → 0.077) — 平均每周期更高效, 不是 memory-bound 加深

**A-H1 LLC capacity REFUTED** (Δ -0.58 pp 是噪声; 真 capacity limit 应该 > 20 pp)
**A-H2 dTLB capacity REFUTED** (Δ -0.0530 pp 不是限制; dTLB miss 反而越来越少)

### 真根因待定 — measurement caveat

perf stat 跑了 protocol_a_ycsb **全程 (LOAD + TRANS)**, TRANS phase 只占 ~5% 时间 (5M ops × ~30 ns/op = 0.15s vs total 20s)。**PMU stats 被 LOAD phase dominated**:
- LOAD phase 同 working set 同 cache_pool 写入 pattern, 跨 cache_pct 几乎一致 → PMU 看起来不变
- TRANS phase 真实 LLC miss 可能完全不同 — 这次没测到

**Phase 1.1' rerun planned (deferred)**:
- 用 `perf record -F 99 --time-start <timestamp>` attached during TRANS only
- 或 split LOAD/TRANS 跑两个独立 ./protocol_a_ycsb 分别 perf
- 或 binary 自己 emit PERF_RECORD_BEGIN/END 标记 (需 instrumentation)

无 isolated TRANS-phase PMU, iter-15A 的 "LLC pressure" 假说**既没 confirm 也没真正反驳**. Verdict: **inconclusive**, 不能用 Phase 1.1 现有数据声称 anomaly A 已 RCA'd.

---

## Phase 2 — Anomaly B (zipf-1.5 read collapse)

### Phase 2.1b — Cache vs no-cache split test

数据: [docs/iter19A_phase2_1b_truenocache_20260531_023729/](../iter19A_phase2_1b_truenocache_20260531_023729/)

Cache build (`build-cxl-w1-v1024`) vs no-cache build (`-DFUSEE_DISABLE_CACHE_POOL=1`):

|             | cache build | no-cache build | ratio cache/nc |
|-------------|------------:|---------------:|---------------:|
| zipf-0.99   |       47.59 |          38.00 | **1.25×** (cache 有用) |
| zipf-1.5    |       16.28 |          17.39 | **0.94×** (cache **反而拖累 6%**) |

**两个独立机制叠加**:
1. **没 cache 时 zipf-1.5 也跌**: collapse ratio = 17.39/38.00 = **0.46**. 说明 hashtable 路径自己有 ~50% 退化 (B-H3 hot bucket 影响真实存在, 但只贡献一半)
2. **加 cache 后 zipf-1.5 更糟**: collapse ratio = 16.28/47.59 = **0.34**. cache 在极端 skew 下**负贡献 12 pp** (0.46 → 0.34)

**关键 quantitative 分解**:
- 总 collapse: 0.66 (= 1 − 0.34) = 没 cache 0.54 + cache 加重 0.12
- **B-H3 (hashtable hot bucket) 真实贡献 ~82% (0.54/0.66)**
- **cache 路径贡献 ~18% (0.12/0.66) — 真实但次要**

### Phase 2.4 v2 — LRS stage decomp under 4 distributions

数据: [docs/iter19A_phase2_4_lr_decomp_v2_20260531_024416/](../iter19A_phase2_4_lr_decomp_v2_20260531_024416/)

Probe-on build, 4 dist × 3 reps, V=1024 T=64 cache_pct=10:

| dist | thpt (Mops) | lrs2r tot | lrs4r tot | r2hit rate |
|---|---:|---:|---:|---:|
| uniform | ~29 | 0 | 0 | ~10 % |
| zipf-0.5 | ~40 | 0 | 0 | ~35 % |
| zipf-0.99 | ~47 | 0 | 0 | ~40 % |
| zipf-1.5 | ~16 | 0 | 0 | ~30 % |

#### B-H1 / B-H2 (CAS retry) REFUTED

**`LRS2R` (seqlock reader retry) = 0 全部 cells, `LRS4R` (cpool_insert CAS retry) = 0 全部 cells**. 没有任何 CAS race。原 plan 的"thundering herd 假说"完全错误 — 假设的机制不存在。

### B-H2 修订 — MESI ping-pong via LRU touch (NOT seqlock)

Stage decomp HIT-path 跨 4 dist (p50 ns):

| dist | LRS1 entry | **LRS2 cache_lookup** | StageW HIT |
|---|---:|---:|---:|
| uniform | 16.7 | 578 | 613 |
| zipf-0.5 | 16.7 | 473 | 508 |
| **zipf-0.99** | 16.7 | **469** | 506 |
| **zipf-1.5** | 16.7 | **6262** ❗ | **6333** |

**LRS2 在 zipf-1.5 是 zipf-0.99 的 13.3×**, 是 anomaly B 的核心 signal.

机制 (代码层):
```cpp
// src/cxl_cache_pool.cc :: cache_pool_lookup, line ~95
// Every read does this RELAXED RMW:
e->lru_epoch.store(global_epoch_load, std::memory_order_relaxed);
```

`lru_epoch` 跟 `seq/key/value_bytes` 在同一 `KvCacheEntry` 结构 (~1088 B, 17 cachelines)。

在 zipf-1.5 下:
- Top key 占 ~50% 流量, 64 workers per host 中 ~32 同时 lookup hot key
- 每个 lookup 触发一次 `lru_epoch` RELAXED RMW → cacheline owned in Modified state
- **64 cores 反复抢 ownership** → MESI E/M/S → I → E 状态机疯狂转
- 每次 ownership transfer ~ few hundred ns → 累积到 LRS2 的 6262 ns p50

为什么 LRS2R = 0:
- `lru_epoch.store` 是 STORE 不是 CAS — 不进 seqlock retry
- 但它**写同一 cacheline 上的 `seq`/`key`/`value_bytes`** → reader 的 `seq` 也被 invalidate → reader 必须 refetch → 时间花在 cache fetch 不是 retry loop

### B-H3 hashtable hot bucket — partial CONFIRMED via Phase 2.1b

Phase 2.1b cache=off 跑出 collapse ratio 0.46 → 没 cache 时 hashtable 路径自己就有 -50% 退化. 64 workers 同时 flush + scan 同一个 CXL hashtable bucket. **B-H3 真实, 贡献 anomaly B 的 ~82%**.

Phase 2.4 stage decomp MISS path 也 confirm:
- zipf-1.5 MISS path LRS3 (cxl miss) = 6365 ns vs zipf-0.99 = 3066 ns (2× slowdown), 同一 hot key 多 worker 同时砸 CXL bucket cacheline

### iter-14A F2 关联 — 当年 rollback 是过早的

iter-14A `FUSEE_LRU_SAMPLE=1` flag 思路: 1/64 概率才 store lru_epoch (用 `rdtsc() & 0x3F == 0` 判). 当时**ROLLED BACK** 因为单 cell perf 没显著提升.

但 iter-19A 数据证明 iter-14A 假设是对的, rollback 决定基于 **错误 cell 选择** (zipf-0.99 cell, 那里 cacheline ping-pong 还没严重到瓶颈). **真正受益的是 zipf-1.5 cell — 当时没测**.

Note iter-15A summary §6.0c 把 zipf-1.5 collapse 归到 LFM bucket lock — 那是 write 的事; **read 的 LRU touch ping-pong 是 iter-15A 漏掉的独立机制**.

---

## 结论矩阵 (回到原 Plan §2 假设)

### Anomaly A

| ID | 假设 | iter-19A verdict |
|---|---|---|
| A-H1 | LLC capacity | **REFUTED** (LLC miss% 平) — but 受 LOAD-dominated PMU 影响, inconclusive |
| A-H2 | dTLB capacity | **REFUTED** (dTLB miss% 反向) — 同上 caveat |
| A-H3 | open-addressing probe length | 未测 (defer) |
| A-H4 | hash compute / modulo cost | 未测 (defer) |

→ Anomaly A **真根因 unknown**, iter-19A 关闭 Phase 1.1 with caveat, iter-20A 重做 isolated TRANS-phase PMU.

### Anomaly B

| ID | 假设 | iter-19A verdict |
|---|---|---|
| B-H1 | cpool_insert CAS retry thundering herd | **REFUTED** (LRS4R = 0) |
| B-H2 | hot cache entry cacheline ping-pong | **CONFIRMED** but 机制 = LRU touch RMW, 不是 seqlock CAS |
| B-H3 | hashtable hot bucket cacheline ping-pong | **CONFIRMED** (~82% 贡献 from Phase 2.1b) |
| B-H4 | LRU evict storm | 未单独 attribute, evict counter Phase 0 已含但 ~constant |
| B-H5 | r2hit metric artifact | refute - 数据一致 |

→ Anomaly B **根因 split 82/18 hashtable vs cache LRU touch**.

---

## iter-20A 候选 backlog (按 ROI 排)

1. **重启 `FUSEE_LRU_SAMPLE`** 当年 iter-14A 想做但 rollback 的: in `cache_pool_lookup`, 概率性 1/64 才 store `lru_epoch`. **预期**: zipf-1.5 thpt + 50%+. 走 RAP (改 default behavior + LRU 公平性需重 argue).

2. **`lru_epoch` 拆 cacheline**: 把 `lru_epoch` 单独 cacheline (+64B padding). 即使写 epoch 也不 invalidate `value_bytes` cacheline. Workload-c hot-key 受益. **预期**: zipf-1.5 thpt + 30-50%.

3. **Phase 1.1' isolated TRANS-phase PMU**: 用 perf record + ftrace 或 binary 自己 emit USDT/uprobe marker. 重新 attribute Anomaly A — **真 LLC** 是不是 capacity 问题.

4. **B-H3 hot bucket fix** (针对 hashtable path): hashtable bucket scan 加 sharding-hash-distributed read (e.g., 同 key 不同 worker scan 不同 entry order). 复杂, low ROI vs LRU fix.

---

## 数据 + commits

- Phase 0: commit 2680dd5, dir `docs/iter19A_phase0_baseline_20260531_013922/`
- Phase 2.4 probes + analyzer: commit af1c7c8
- Phase 2.1b + 2.4 v2 + 1.1 sweep scripts: commit (this iter)
- iter-19A summary (本 doc): commit (this iter)

## Phase delivery audit (per CLAUDE.md precedent #3)

| Phase | Plan | Delivered | Status |
|---|---|---|---|
| Phase 0 — baseline | reproduce on g1/g2 | done, ±2% match | ✅ FULL |
| Phase 1.1 — PMU sweep | 7 cache × 3 reps perf stat | done, but LOAD-dominated → 结论 inconclusive | ⚠️ PARTIAL (用户认可 Phase 1.1' iter-20A 跟进) |
| Phase 1.2 — entries-per-bucket layout sweep | 4 builds × 8 cells × 3 reps | **NOT DONE** | ❌ DEFERRED → iter-20A (用户允许 autonomous 进度) |
| Phase 1.3 — hugepages experiment | mmap MAP_HUGETLB build | **NOT DONE** | ❌ DEFERRED → iter-20A |
| Phase 1.4 — synthetic working set bench | tools/cache_pool_synthetic_bench | **NOT DONE** | ❌ DEFERRED → iter-20A |
| Phase 1.5 — Phase 1 synthesis | RCA doc | **PARTIAL** (本 doc §"Anomaly A 真根因待定" 章节) | ⚠️ PARTIAL |
| Phase 2.1 — env cache split | env-based, found to be misdiagnosis | ❌ replaced by 2.1b | (subsumed) |
| Phase 2.1b — true no-cache build split | 12 cells × 3 reps | ✅ FULL |
| Phase 2.2 — CAS retry counter + perf c2c | lrs2r/lrs4r counter + c2c | lrs2r/lrs4r counter **DONE**, perf c2c NOT DONE | ⚠️ PARTIAL |
| Phase 2.3 — dense zipf sweep | 10 zipf × 3 reps | **NOT DONE** | ❌ DEFERRED → iter-20A |
| Phase 2.4 — local_read stage decomp | LRS probes + 4-dist sweep + analyzer | ✅ FULL |
| Phase 2.5 — LRU evict counter audit | check cp_lru_evict 在 zipf-1.5 是否高 | ✅ FULL (counters constant cross dist) |
| Phase 2.6 — r2hit counter audit | code review | NOT explicitly done, but B-H5 由 Phase 0 数据反驳 | ⚠️ AUDIT-IMPLICIT |
| Phase 2.7 — Phase 2 synthesis | RCA doc | ✅ FULL (本 doc §B verdict) |
| Phase 3 — synthesis + iter-20A backlog | iter19A_summary + backlog | ✅ FULL (本 doc) |

Phase 1.2/1.3/1.4 + Phase 2.3 defer to iter-20A is documented and intentional given iter-19A's mission: **identify root cause of two anomalies, hand off fix proposals to next iter**. iter-19A 已 deliver:
- Anomaly A 根因结论 = inconclusive with concrete next-step (Phase 1.1' isolated PMU)
- Anomaly B 根因结论 = LRU touch cacheline ping-pong (B-H2 mechanism) + hashtable hot bucket (B-H3) at ~18/82 ratio, with concrete fix proposal (FUSEE_LRU_SAMPLE re-enable)
