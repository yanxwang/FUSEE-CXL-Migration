# iter-19A — local_read 两大 anomaly 根因 study

**草稿日期**: 2026-05-31
**规划方法**: 聚焦 RCA — 不做新 path optimization、不动 multi-receiver 架构。复用 iter-15A 已 collect 的 anomaly 数据为起点，加 perf PMU + 路径 probe + targeted experiment 把两个未结案现象坐实。
**Predecessor**: iter-15A Phase 6 (partial RCA), iter-18A (xhost_read decomp framework — 工具复用)

---

## 0. Background — 为什么 iter-15A 没结案

iter-15A microbench 出现两个 local_read 反直觉表现：

### Anomaly A — local_read thpt 随 cache_pct 增大单调下降

`docs/iter15A_microbench_phase3_20260520_071115/grid.csv` 实测 (V=1024, T=64, zipf-0.99):

| cache_pct | cache_buckets | thpt (Mops) | r_p50 (µs) | r2hit | r2miss | cache_fill % |
|---:|---:|---:|---:|---:|---:|---:|
| 1   |  16 384 | **65.73** | 0.301 | 25 951 | 52 174 | 100.0 |
| 2   |  32 768 | 62.44 | 0.297 | 28 052 | 50 073 | 100.0 |
| 5   |  65 536 | 55.88 | 0.294 | 29 927 | 48 198 | 100.0 |
| 10  | 131 072 | 47.55 | 0.295 | 31 398 | 46 727 | 100.0 |
| 20  | 262 144 | 38.52 | 0.350 | 32 765 | 45 360 |  99.5 |
| 50  | 524 288 | 32.06 | 0.478 | 34 271 | 43 854 |  87.4 |
| 100 | 2 097 152 | **25.98** | 0.534 | 35 524 | 42 601 |  29.6 |

**反直觉点**: cache 变大→thpt 反而掉 2.5×。r2hit 还小幅 RISE (26K→36K)，但每 op 成本上去得更快。

**iter-15A Phase 6a verdict (未端到端验证)**: 用 `perf c2c` 排除了 MESI ping-pong (HITM count 恒定)，归到 "LLC pressure / working set exceeds L3"。但**没用 PMU 正向证明** LLC-load-miss / dTLB-load-miss 在大 cache 下确实爆涨；也没区分**LLC 容量限制**与 **dTLB 容量限制**两种独立的退化机制。

### Anomaly B — local_read 在 zipf-1.5 突然崩

`docs/iter15A_microbench_phase4_20260520_080309/grid.csv` 实测 (V=1024, T=64, cache=10 %):

| dist | thpt (Mops) | r_p50 (µs) | r_p99 (µs) | r2hit | r2miss |
|---|---:|---:|---:|---:|---:|
| uniform   | 29.52 | 3.93 | 16.99 |  8 138 | 69 987 |
| zipf-0.5  | 40.52 | 0.62 | (n/a) | 27 696 | 50 429 |
| zipf-0.99 | **47.67** | 0.29 | 12.48 | 31 359 | 46 766 |
| zipf-1.5  | **14.81** | 0.28 | 16.99 | 17 278 | 60 847 |

**反直觉点**: 分布更倾斜（zipf-1.5 比 zipf-0.99 更集中到少数 hot key）应该 cache 命中率 UP、thpt UP — 实测**反向**, hit count 跌 45%, thpt 跌 69%。

**iter-15A Phase 6.0/6.0c verdict (部分解释)**: `perf c2c` 排除 MESI；p99 latency 分析归到 **LFM bucket lock contention**。但这条**只解释了 local_write 在 zipf-1.5 的崩**（write 走 LFM bucket lock）。**Local_read 不走 LFM bucket lock**（reader 走 seqlock CAS 路径），所以 6.0c 的结论不适用 read，但 iter-15A 没专门给 read 路径找新假设。

### iter-19A 的目标

把这两个 anomaly 用 **正向 PMU 数据 + 路径 probe + targeted experiment** 端到端定位到一个或多个具体机制。结论无论 confirm / refute iter-15A 假设，都明确写下来。

---

## 1. 范围与不在范围

### 1.1 In-scope

- 仅 **local_read** 路径（host 自己的 key）— iter-15A 4-path microbench 框架已支持
- Protocol A only
- 工具用 `perf stat / perf record / perf c2c` (PMU) + iter-16A `cxl_probe.h` 框架（probe ring）+ iter-15A 现有 microbench runner
- 验证 fix 方向（hugepages / cpool_insert 改造）但**只做最小 prototype + smoke 不端到端 ship**

### 1.2 Out-of-scope (defer)

- **xhost_read 路径** — 已在 iter-18A 系统覆盖（XR stage 框架 / Phase 4 multi-shard fix）
- **Local_write 任何修改** — iter-15A Phase 6.0c 已结案，不重新审视
- **Multi-receiver / sharding 改动** — 与本研究正交
- **YCSB workload sweep** — local_read 只是路径之一，整 YCSB 在 iter-20A backlog
- **Protocol B / C** — 完全不动

### 1.3 平台

g1 + g2 (per 2026-05-29 default 切换)。**注意**: 测试床目前 CXL 在 x8 状态 (iter-18A 后 5/27 维护引起，h3 端 LnkSta 已确认)。**本 iter 数据 NOT 跟 iter-15A 原始数据直接横向比较 absolute Mops**，只比较**形态趋势**（cache_pct ↑ thpt 是否下降 / zipf-1.5 是否塌陷）。Phase 0 重跑确认形态在新平台仍然成立。

---

## 2. 假设清单（待验证）

### Anomaly A 假设

| ID | 假设 | 预期 PMU/probe 证据 |
|---|---|---|
| **A-H1** | cache_pool 总占用超 L3 → DRAM access dominate per-op cost | LLC-load-miss rate 跟 cache_pct 单调 + correlate with thpt drop; 70% L3 → 90%+ at large cache |
| **A-H2** | cache_pool spans 多页 → dTLB miss dominate (working set 跨 page 数 ≫ dTLB capacity) | dTLB-load-miss rate 跟 cache_pct 单调; hugepages 实验消除大半 thpt drop |
| **A-H3** | Open-addressing linear probe (kTlsProbeMax=8) 在更稀疏 bucket 下扫更多 entry | 平均 probe length 跟 cache_pct 单调；同 cache_pct 但变 kCacheEntriesPerBucket 应反向变化 |
| **A-H4** | 大 bucket 数 → 计算 bucket index 的 hash 受影响（fnv1a 不应该有此问题，但 modulo 大数可能触发 hardware divider 卡顿） | T=1 单线程下 cache_pct 仍呈同样降势 → 排除并发因素，cpu cycle/op 单调上升 |

四个假设**可能叠加**。需要 PMU 把各自贡献度拆出来。

### Anomaly B 假设

| ID | 假设 | 预期 PMU/probe 证据 |
|---|---|---|
| **B-H1** | zipf-1.5 下 cache_pool_insert 在 hot key bucket 上 thundering herd: 64 worker 同时 miss → 同时 CAS seq even→odd → 大量 CAS retry → 隐性串行化 | CAS retry counter 在 zipf-1.5 比 zipf-0.99 涨 ≥10×; perf c2c 看 hot key bucket cacheline HITM 涨 |
| **B-H2** | hot key bucket cacheline ping-pong: 即使纯 read，seqlock load 在 64 核间也 ping-pong 同一根 cacheline | perf c2c 看 hot bucket cacheline 是 top HITM source; 跑 cache=0 实验消除 cache_pool → 若 thpt 不再崩 → 锁定 cache 路径 |
| **B-H3** | r2miss 路径上 hashtable bucket scan 也在 hot key 上 ping-pong（cache_pool miss 后落 hashtable, 64 个 worker 同时扫同 bucket） | cache=0 时 zipf-1.5 仍然崩 → 反 H2 + 强化 H3 |
| **B-H4** | hot key 的 cache_pool entry 因 LRU evict 被反复 evict-then-insert（受 LRU policy 影响） | cp_lru_evict counter 在 zipf-1.5 显著高; 改 LRU policy 或关闭 evict 验证 |
| **B-H5** | r2hit metric artifact — zipf-1.5 实际 hit 率高但 r2hit counter 漏数（e.g. CAS retry path 不计 hit） | code review r2hit emit point; 若 r2hit 只在 first-try-success 计数, 高 retry 下数会被低估 |

H1/H2/H3 互斥 (它们解释不同细分场景); H4/H5 是 confounders 要先 排除。

---

## 3. Phase 列表（顺序执行）

### Phase 0 — Baseline reproduction on g1/g2

**目标**: 在 current 平台（g1/g2，CXL x8 状态）确认两个 anomaly 仍然成立 + 拿到 iter-19A 自己的 baseline。

| 任务 | 交付物 |
|---|---|
| 0.1 | 在 g1/g2 上 bootstrap + 装 perf + 装 cxl-cli（已有）; 编译 cxl_kv_bench_mp 当前 head |
| 0.2 | 跑 iter-15A Phase 3 grid（cache_pct ∈ {1,2,5,10,20,50,100}, V=1024, T=64, zipf-0.99, local_read only）3 reps |
| 0.3 | 跑 iter-15A Phase 4 grid（dist ∈ {uniform,zipf-0.5,zipf-0.99,zipf-1.5}, V=1024, T=64, cache=10 %, local_read only）3 reps |
| 0.4 | 对比 iter-15A 原始数字：形态保持 (>1 µs 单调降 / zipf-1.5 显著塌陷) → 继续; 不保持 → 先 RCA 平台差异 |

**HARD delivery**: `docs/iter19A_phase0_baseline_<ts>/` 含 grid.csv + p3_form.png + p4_form.png + reproduce_check.md (cache_pct=1 vs cache_pct=100 thpt ratio + zipf-0.99 vs zipf-1.5 thpt ratio，两个 ratio 与 iter-15A 数据差 ≤ ±20 % 才算复现成立)

---

### Phase 1 — Cache-size anomaly RCA

#### 1.1 PMU-based attribution (Anomaly A 主战场)

**目标**: 跨 cache_pct grid 同时收 LLC + dTLB PMU，attribute thpt drop 到 LLC capacity 还是 dTLB capacity。

| 任务 | 交付物 |
|---|---|
| 1.1.1 | 在 cxl_kv_bench_mp 加 `--perf-stat-tids` 类似 hook，attach `perf stat -e cycles,instructions,LLC-loads,LLC-load-misses,dTLB-loads,dTLB-load-misses,L1-dcache-loads,L1-dcache-load-misses` 到 worker tids，窗口 5 s; 或直接 wrap `perf stat -p $worker_pid` |
| 1.1.2 | 7 cache_pct × 1 dist × 3 rep = 21 cells, T=64 V=1024 zipf-0.99 local_read; 每 cell 同时拿 thpt + 上述 PMU 指标 |
| 1.1.3 | viz: per-cache_pct 的 LLC miss rate / dTLB miss rate / IPC / cycles-per-op trends; correlation matrix vs thpt |
| 1.1.4 | verdict 表: A-H1 (LLC) confirm/refute, A-H2 (dTLB) confirm/refute |

**HARD delivery**: phase1.1 RCA writeup 含每个假设的 PMU 证据 vs 反证；若 H1 + H2 都看 ≥ 0.7 correlation 与 thpt drop，标 BOTH CONFIRMED + 进 1.3 hugepages 实验给 H2 一个判定性测试。

#### 1.2 Cache layout sweep — 同总容量, 不同 bucket × entries 分布

**目标**: 区分 "bucket 数本身有害" vs "总 cacheline 占用有害"。

| 任务 | 交付物 |
|---|---|
| 1.2.1 | 暂改 src/cxl_cache_pool.h `kCacheEntriesPerBucket` 编译 4 个版本：4 / 8 / 16 / 32 entries-per-bucket；FUSEE_CACHE_BUCKETS 反向调整保持总 entry 数恒定 |
| 1.2.2 | 跑 grid: T=64 V=1024 zipf-0.99 local_read, (entries_per_bucket, total_entries) ∈ {(4,32K), (8,32K), (16,32K), (32,32K)} × {(4,1M), (8,1M), (16,1M), (32,1M)}, 3 reps |
| 1.2.3 | 比较：thpt 是否随 bucket count 单调（同总容量下）→ 若 yes, kCacheEntriesPerBucket=4 是次优, 大 bucket 更 cache-friendly |

**HARD delivery**: phase1.2 verdict 含 "bucket-数 vs 总容量 哪个驱动"。

#### 1.3 Hugepages 实验 — A-H2 判定性测试

**目标**: 用 hugepages 消除 dTLB 限制，看 thpt drop 是否消失或显著缓解。

| 任务 | 交付物 |
|---|---|
| 1.3.1 | 在 cxl_cache_pool 加 `FUSEE_CACHE_HUGEPAGES=1` env，mmap 时加 `MAP_HUGETLB \| MAP_HUGE_2MB` flag |
| 1.3.2 | 跑 7 cache_pct grid: hugepages=on vs off, 各 3 reps; 同时拿 dTLB-load-miss + LLC-load-miss PMU |
| 1.3.3 | 计算每 cache_pct 的 (hugepage_thpt / vanilla_thpt) ratio：大 cache 应 > 1.0 且单调增 |

**HARD delivery**: phase1.3 verdict 表 — hugepage 提升 ≥ 30 % @ cache_pct=100 → A-H2 CONFIRMED; 否则 REFUTED, A-H1 (LLC) 单独成立。

#### 1.4 Synthetic working set test — 排除 FUSEE 特有的混淆

**目标**: 写一个最小程序，纯 random read N-byte regions，sweep N，看是否复现同 thpt 曲线形状。

| 任务 | 交付物 |
|---|---|
| 1.4.1 | tools/cache_pool_synthetic_bench.cc — mmap N MB, 64 threads, 各自 random index 读 1088-B record per op + 数 ops/sec; sweep N ∈ {16M, 64M, 256M, 1G, 4G, 16G} 对应 cache_pct grid 量级 |
| 1.4.2 | 跑 6 × 3 rep 数据 |
| 1.4.3 | 跟 Phase 1.1 cache_pct 曲线叠图: 若形状一致 → confirm "anomaly 不是 FUSEE 特有，是 DRAM/LLC/TLB 通用现象"; 形状不一致 → FUSEE 有额外机制 |

**HARD delivery**: phase1.4 overlay PNG + writeup 决定 "FUSEE 特有 vs 通用".

#### 1.5 Phase 1 synthesis

写 `iter19A_phase1_rca.md`：对每个 A-H 假设给 CONFIRMED / REFUTED / PARTIAL + 引用 phase1.1-1.4 证据。给出消除 anomaly 的最小成本 fix 建议（不一定 ship）。

---

### Phase 2 — zipf-1.5 anomaly RCA

#### 2.1 Cache=0 vs cache=on @ zipf-1.5 (区分 cache 路径 vs hashtable 路径)

**目标**: 用 `FUSEE_CACHE=0` 关 cache_pool 跑 zipf-1.5, 看 thpt 是否依然崩。**关键 split test**。

| 任务 | 交付物 |
|---|---|
| 2.1.1 | dist ∈ {zipf-0.99, zipf-1.5} × cache ∈ {0, 10 %} × 3 rep = 12 cells, T=64 V=1024 local_read |
| 2.1.2 | 若 cache=0 时 zipf-1.5 仍崩 → 在 hashtable/bucket 路径（B-H3 LIKELY），cache 路径不是主因; 若 cache=0 时 zipf-1.5 不崩 → cache 路径是主因（B-H1 / B-H2 / B-H4 候选） |

**HARD delivery**: phase2.1 split verdict + 引用 thpt 数据。

#### 2.2 CAS retry counter + bucket perf c2c (B-H1 验证)

**目标**: 加 cpool_insert 的 CAS retry 累计 counter，看 zipf-1.5 vs zipf-0.99 retry 是否爆涨。

| 任务 | 交付物 |
|---|---|
| 2.2.1 | src/cxl_cache_pool.cc cache_pool_insert 加 `__atomic_fetch_add(&pool->insert_cas_retry, 1, RELAXED)` 在 CAS 失败 retry 路径; 同上加 `seqlock_read_retry` 在 read 路径 retry; 通过 bench dump_counters 落 grid.csv |
| 2.2.2 | 重跑 phase 4 4-dist grid，拿 insert_cas_retry 和 seqlock_read_retry rate (per op) |
| 2.2.3 | perf c2c 跑 zipf-1.5 local_read 4 s window, attach 64 worker tids, 抓 top HITM cachelines + 它们的 source; 跟 zipf-0.99 同位置对比 |

**HARD delivery**: phase2.2 verdict — H1 (insert thundering) confirm 条件: insert_cas_retry @ zipf-1.5 ≥ 10× zipf-0.99 + perf c2c 显示 hot key cache_pool entry cacheline 是 top HITM. 否则 REFUTED 进 2.3.

#### 2.3 Detail zipf sweep — 找拐点 (验证是否真有 "zipf-1.5 突变")

**目标**: 在 zipf {0.5, 0.7, 0.9, 0.99, 1.05, 1.15, 1.3, 1.5, 1.7, 2.0} 加密 sweep, 看 thpt 是连续下降还是某个具体 skew 点突变。

| 任务 | 交付物 |
|---|---|
| 2.3.1 | 10 zipf × 1 cache (10%) × 3 rep = 30 cells, T=64 V=1024 local_read |
| 2.3.2 | thpt vs zipf 折线 + 拐点定位 |

**HARD delivery**: phase2.3 拐点 PNG + writeup. 若拐点 = zipf-1.2 附近 → 1.5 不是 magic number，是 "skew 超过某临界后 cache_pool 退化"; 若 thpt 在某个 zipf 点突变（spikes down）→ 该 specific 点对应某 hardware/software 临界（e.g., dist 让 1 个 key 占 ≥ 50% 流量）.

#### 2.4 Local_read path stage decomp 加 probe

**目标**: iter-16A xhost_write / iter-18A xhost_read 都有 stage decomp，local_read 没有。补一个简化版（5 stage: R0_tls/R1/R2hit/R2miss/R5 重命名 LRS1..LRS5），跑 phase 4 grid 看 zipf-1.5 哪个 stage 时间炸。

| 任务 | 交付物 |
|---|---|
| 2.4.1 | src/cxl_kv_ops_A.cc local_read 路径 `search()` 加 ≥5 个 PROBE_OP("LRS*") 点; 复用 iter-16A cxl_probe.h |
| 2.4.2 | 编译 `FUSEE_LOCAL_READ_PROBE=1` 跑 phase 4 4-dist × 3 rep probe-on; 抽样 50k op stage timing median |
| 2.4.3 | per-dist stage breakdown 表 + stacked bar PNG |

**HARD delivery**: 每 stage 在 4 dist 下的 median 时间表 + 找出 zipf-1.5 哪个 stage 涨多少。若 "cache_pool_lookup" 涨 → confirm cache 路径; 若 "hashtable scan" 涨 → confirm hashtable 路径。

#### 2.5 LRU evict counter (B-H4) 验证

**目标**: cp_lru_evict counter 在 zipf-1.5 是否显著高。

| 任务 | 交付物 |
|---|---|
| 2.5.1 | iter-15A grid.csv 已有 cp_lru_evict 字段; 直接 query phase 0 baseline 数据 |
| 2.5.2 | 若 evict 数高且 hot key 反复 evict — 提议 patch: hot key affinity 不参与 LRU。code prototype 不必 ship。 |

**HARD delivery**: cp_lru_evict 数据表 + B-H4 verdict.

#### 2.6 r2hit counter audit (B-H5)

**目标**: 看 r2hit 是不是只数 "first-try success", 高 retry 下系统性低估。

| 任务 | 交付物 |
|---|---|
| 2.6.1 | grep `r2hit` 实现位置（应在 cxl_kv_ops_A.cc 或 tests/cxl_kv_bench_mp.cc）; review counter emit point |
| 2.6.2 | 若 r2hit 在 seqlock retry 内不 increment → metric artifact; 若 r2hit 在 retry 后 success 也 increment → 排除 H5, hit rate 真的低 |

**HARD delivery**: code annotation + 一句 verdict.

#### 2.7 Phase 2 synthesis

写 `iter19A_phase2_rca.md` — 综合 2.1-2.6 给每个 B-H 假设结论 + 在不同 zipf 区间适用的根因。

---

### Phase 3 — Synthesis + iter-20A backlog

| 任务 | 交付物 |
|---|---|
| 3.1 | `docs/iters/iter19A_summary.md` — 每个 anomaly 一个章节 + 对应 RCA verdict + 数据引用 |
| 3.2 | iter-20A backlog 候选: (a) cache_pool hugepages-backed 默认 (若 Phase 1.3 大于阈值收益), (b) cache_pool entries-per-bucket 调参 (若 Phase 1.2 yes), (c) cpool_insert thundering herd 修法 (若 Phase 2.2 confirm), (d) hot-key-skip-cache 策略 (若 Phase 2.5 confirm LRU evict-storm) |
| 3.3 | blueprint update (§I.3 KvCachePool): 添加 "iter-19A finding — cache_pool 在 working set > LLC 时 thpt 显著退化, 推荐 cache_pct 不超过 X" 的 一段注释 |

---

## 4. 设计选择 RAP

### RAP-1 — Phase 1.1 PMU attach 方法

**STATE**: 用 `perf stat -p $tid -e <events>` attach 到每个 worker，跑 ≥5 s window。

#### Attack vectors

| # | Category | Concern |
|---|---|---|
| AV1 | PERFORMANCE | perf attach 会 perturb timing → thpt 测量本身被污染 |
| AV2 | CORRECTNESS | PMU counter 在 hyperthread / migrated thread 上数据可能漂移 |
| AV3 | GENERALITY | T=64 worker × 5 PMU event = 320 perf 进程, sysctl perf_event_paranoid 可能限制 |
| AV4 | COMPLEXITY | 后处理 64 个 worker × 5 event 数据 → 平均 / 分位数复杂 |
| AV5 | PRIOR ART | iter-17A Exp 2 已用 perf stat 跑 receiver tids; 同样模式可复用 |
| AV6 | IMPLEMENTATION FEASIBILITY | g1/g2 是 root 登录, paranoid ≤2, perf 全功能可用 |

#### Ablation

无 perf 单纯跑 thpt → 跟 perf attach 跑 thpt 对比，确认 perturb < 5%; > 5 % 改用 `perf record -F 99` sample 而非 stat。

#### Verdict

ACCEPT — 已 mitigation: 用 sampling 模式 (perf record -F 99 -a) 全机 attribute 替代 per-tid attach；perturbation < 2% 历史确认。

---

### RAP-2 — Phase 1.3 hugepages 实施方式

**STATE**: 在 cxl_cache_pool 加 `FUSEE_CACHE_HUGEPAGES=1` env，mmap 时 `MAP_HUGETLB | MAP_HUGE_2MB`。Pre-reserve 通过 `sysctl vm.nr_hugepages` 或 `/sys/devices/system/node/.../hugepages/...`.

#### Attack vectors

| # | Category | Concern |
|---|---|---|
| AV1 | PERFORMANCE | 2MB hugepages 减 dTLB 压力 ~512×; 但 1GB hugepages 减更多, 留 fallback |
| AV2 | CORRECTNESS | MAP_SHARED + MAP_HUGETLB 兼容? 需要 verify on g1/g2 kernel 6.15 |
| AV3 | GENERALITY | 大 cache_pct=100 需要 ~9 GB → 4 个 2MB hugepages 不够, 需要 ≥4500 page reservation; pre-boot config |
| AV4 | COMPLEXITY | reservation 用户操作 + bench env 切换; 增加 setup 步骤 |
| AV5 | PRIOR ART | Linux memcached / Redis production routinely 用 2MB hugepages backed shm |
| AV6 | IMPLEMENTATION FEASIBILITY | g1/g2 自由 reboot 配 hugepages，但要协调 g3/g4 共用 testbed 用户 |

#### Ablation

跑 vanilla 4K vs hugepages 2MB 同 cache_pct=100; 测 dTLB-load-miss rate 应降 100×+; thpt 提升量 = H2 confirmation 强度。

#### Verdict

ACCEPT — 用 2MB pages (够 cache_pct ≤ 50); 1GB pages 留 Phase 1.3 ext if needed.

---

### RAP-3 — Phase 2.4 local_read decomp probe 插入位置

**STATE**: 在 search() 加 5 个 PROBE 点（LRS1=entry, LRS2=cache_pool_lookup_start, LRS3=cache_pool_lookup_end + 分支, LRS4=hashtable_fetch (miss only), LRS5=return）。

#### Attack vectors

| # | Category | Concern |
|---|---|---|
| AV1 | PERFORMANCE | local_read p50 ≈ 0.3 µs（zipf-0.99）, probe overhead 50 ns × 5 = 250 ns ≈ +83% 影响 → 改用 sampling probe (每 100 op 一次) |
| AV2 | CORRECTNESS | 5 个 probe 必须 ∑LRS == StageW invariant 持; 路径分支需要分别 emit |
| AV3 | GENERALITY | local_read 在 cache hit/miss 路径分叉; LRS4 仅 miss 路径出现 |
| AV4 | COMPLEXITY | analyzer 需要按 hit/miss 分类聚合 |
| AV5 | PRIOR ART | iter-16A xhost_write XWS / iter-18A XR 都 用 RDTSCP 50ns/probe; 同模式 |
| AV6 | IMPLEMENTATION FEASIBILITY | 已有 cxl_probe.h 框架, 加 macro `FUSEE_LOCAL_READ_PROBE`, 跟 FUSEE_PROBE / FUSEE_READ_PROBE 独立 gate |

#### Ablation

probe-on vs probe-off thpt 对比 — 任意 cell 退化 > 25% RCA 后再 ship; sample-only 模式 (every 10th op) 若 full probe 过重.

#### Verdict

ACCEPT — 先 full probe; 若 thpt 退 > 25% 改 sample-based.

---

## 5. Open Questions (CLOSED 2026-05-31)

- **QR1 — hugepages 默认 ship vs opt-in**: ✅ **opt-in env + auto-fallback**.
  `FUSEE_CACHE_HUGEPAGES=1` 才 enable; mmap MAP_HUGETLB 失败时打印 warning
  并 fall back 到 4K pages。CLAUDE.md / blueprint 加文档建议 "cache_pct
  ≥ 10 % 时强烈推荐 hugepages + sysctl 预 reserve"。conservative default
  与 iter-15A TLS L1 默认 off 同 pattern。
- **QR2 — hot-key skip cache 是否走 RAP**: ✅ **YES, walk through RAP,
  lightweight**. CLAUDE.md §XIII 触发 (optimization + 默认行为修改);
  RAP 强迫定义清楚 "什么算 hot" + 6 AV 半小时搞完。**iter-19A 本身只
  产生 RAP doc, 不 ship code** — implementation 留 iter-20A.
- **QR3 — Phase 0 不复现, fallback g3/g4 vs RCA**: ✅ **stay g1/g2 +
  HARD gate**. g1/g2 是 production target; ratio gate 判断 (anomaly A
  ratio = cache_pct=100/cache_pct=1 thpt; anomaly B ratio = zipf-1.5
  / zipf-0.99 thpt; iter-15A 基准 A=0.40, B=0.31).
    - 两 ratio 都 ≥ 0.6 → anomaly 显著弱化 → **iter-19A 停 anomaly
      study, 改写 "平台敏感性" report**
    - 任一在 0.3-0.6 → 部分复现 → 进 Phase 1/2
    - ≤ 0.3 → 完全复现 → 照原 plan
- **QR4 — stage decomp 不匹配 5 假设, 加 callgraph vs defer**: ✅
  **加 Phase 2.7 bounded callgraph**. 触发条件: phase 2.4 5 个 hypothesis
  全 refute / 全 stage 都不显著涨。Phase 2.7 跑一次 `perf record -F 99
  --callgraph dwarf` 5 s + perf report 给出 top 3 frame + verdict。
  **不允许说 "deferred, more study needed"** — 必须给出 verdict,
  哪怕是 "top frame = X, 未知机制" (per iter-15A 失败模式).
- **QR5 — kCacheEntriesPerBucket runtime vs 4 builds**: ✅ **4 个
  compile-time build**. Runtime 引入 branch overhead 污染想测的量级
  (loop unroll vs runtime loop ~5-10 ns/lookup ≈ Phase 1.2 sensitivity);
  4 build 切换成本 2 分钟 vs phase 跑数小时, ROI 极佳; 若某 epb 值
  winner, 直接此 build 部署不用 二次工作。
- **QR6 — Phase 1.1 PMU 工具 / sampling rate**: ✅ **stat for 1.1 +
  record -F 99 for 2.7**. Phase 1.1 用 `perf stat -p $tid -e <counters>`
  (硬件 counter, sampling rate N/A); Phase 2.2 c2c 用 `perf c2c record
  -a` 默认 -F 99; Phase 2.7 callgraph 用 `perf record -F 99 --callgraph
  dwarf`, 仅当 top frame 占 < 20 % 才升级 -F 999 二次 sample.

---

## 6. 交付清单 (HARD)

- [ ] `docs/iters/iter19A_summary.md` — 总结 + verdict
- [ ] `docs/iter19A_phase0_baseline_<ts>/` — Phase 0 复现数据
- [ ] `docs/iter19A_phase1_pmu_<ts>/` — Phase 1.1 PMU 数据
- [ ] `docs/iter19A_phase1_layout_<ts>/` — Phase 1.2 cache layout sweep
- [ ] `docs/iter19A_phase1_hugepages_<ts>/` — Phase 1.3 hugepages 数据
- [ ] `docs/iter19A_phase1_synthetic_<ts>/` — Phase 1.4 synthetic bench
- [ ] `docs/iter19A_phase2_cache_split_<ts>/` — Phase 2.1 cache=0 vs cache=on
- [ ] `docs/iter19A_phase2_cas_retry_<ts>/` — Phase 2.2 retry counter + perf c2c
- [ ] `docs/iter19A_phase2_zipf_detail_<ts>/` — Phase 2.3 dense zipf sweep
- [ ] `docs/iter19A_phase2_local_decomp_<ts>/` — Phase 2.4 stage decomp
- [ ] `iter19A_phase1_rca.md` + `iter19A_phase2_rca.md` — per-phase verdicts
- [ ] blueprint §I.3 KvCachePool 注释一段 "iter-19A finding: working set > LLC → ..."
- [ ] iter-20A backlog 候选 list

**Phase delivery audit table** 必须在 iter19A_summary 含每 sub-phase × 每假设的 ✅/⚠/❌ 矩阵 (per CLAUDE.md precedent #3).

---

## 7. 与 iter-15A 的关系

iter-19A 不 REPRODUCE iter-15A 全部数据，只针对 anomaly 做 attribution。本 iter 完成后:
- iter-15A Phase 6a "LLC pressure" claim 升级为 confirmed/refuted with PMU evidence
- iter-15A Phase 6.0c local_read 在 zipf-1.5 的缺口被填上 (与 local_write 的 LFM lock 解释独立)
- 两个 anomaly 之后任何引用都标注 "see iter-19A summary §X"
