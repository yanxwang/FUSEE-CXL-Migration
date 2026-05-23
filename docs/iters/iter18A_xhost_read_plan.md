# iter-18A — xhost_read 阶段分解 + 逐 stage path optimization + multi-ring/receiver scaling

**草稿日期**: 2026-05-23（user-revised 2026-05-23）
**规划方法**: 复用 iter-16A (decomp / probe / stage) + iter-17A (per-stage 1-2 行迭代 opt + multi-ring + receiver) 对 xhost_write 完成的全套分析、实现、实验流程，原样套到 xhost_read 上。

---

## 0. Background — 为什么现在做 read

- iter-16A 已为 **xhost_write** 建好 stage-decomp 框架（15 probes / 11 latencies / 8 stages），并通过它定位 H9 single-flush opt（+22 % 全 T 段）。
- iter-17A 在 **xhost_write** 上完成 (a) per-stage 1-2 行迭代 path opt（Stage 1+2 lfence / Stage 3 audit / Stage 5 lfence +21-23% / Stage 6 lfence +21-27% / bucket double-flush -36-37%，共 7 个 commit，每个一两行加 smoke test）+ (b) multi-ring + multi-receiver scaling，集群峰值 6.63 Mops/s（T=64 N=4 zipf-0.99）。
- xhost_read 至今**仅有 iter-15A 4-path baseline**（粗粒度，无 stage decomp），且 iter-17A 的 multi-shard ring 在 read 路径上**未验证**（YCSB workloadc T=16 N=4 = 0.016 Mops vs N=0 = 3.47 Mops，217× 下降是 iter-17A defer 的核心遗留）。
- 这意味着 read 路径既缺**诊断框架**也缺**实现验证**，与 write 路径不对称。
- 目标场景 (YCSB-C 100 % 读 / YCSB-A 50 % 读) 中 read 是第一时间影响 20 Mops/s 目标的路径。

### 设计约束（与 iter-17A 收尾一致）
- Protocol A only（不动 B/C，不改 protocol A spec invariants I1–I12）。
- §13 gate 5 anomaly-scan 强制；任何 sweep 输出 `gap_to_target.md` 必须含 anomaly section。
- 任何对 spec 触动走 §XIII RAP（attack vectors ≥6 / 6 类）。
- 计划里**不写时间估计**（per CLAUDE.md 2026-04-27 规则）。
- 不允许 silent descope；任何 sub-phase 没交付，进 iter 总结的 **Phase delivery audit table** 时必须显式 ❌ NOT DONE 且引用用户授权 message，否则 iter 不算完成（per precedent #3 / iter-9A）。

---

## 1. 范围与不在范围

### 1.1 In-scope

| 类别 | iter-16A/17A xhost_write 对应物 | iter-18A xhost_read 任务 |
|---|---|---|
| Stage decomp 框架 | ≥10 probes / 8 stages / RDTSCP+LFENCE | 为 read 路径建同样的 probe→latency→stage 三层，**timing 用 RDTSCP+LFENCE（保持与 cxl_probe.h 一致，不另写 timing 代码）** |
| 4 个 sweep | T / V / dist / perfstat | 同 4 种，每种 ≥3 reps + median + anomaly scan |
| 逐 stage path opt | iter-17A xhost_write Stage 1-7 + bucket double-flush 共 7 commit | **每 stage 1-2 行改动 + smoke test + keep/revert** 循环 |
| Multi-ring matrix | 3D (src,dst,shard) WriteRingMatrix | 3D ReadRingMatrix；与 write 同 actual_shards = ceil(T/N) |
| Multi-receiver | N ReadRecv / pinned / 软件 packing | 同上 |
| Plan A worker_id / Plan B key_hash | iter-17A 2 套 routing 实现 | 复用同 router；read 的 routing 公式与 write 必须一致（避免 hash diff） |
| Sweep grid 对比 | 7-/8-group 对比图 | 同种 8-group 对比 |

### 1.2 Out-of-scope (defer)

- YCSB full sweep（80-cell × 5-rep）— 等 multi-ring read 稳定后再做（iter-19A）。
- Local read 路径优化（host 自己的 key）— 已在 iter-15A 测过，不在本 iter 范围。
- Protocol C / B 任何变化。
- Sharding key space 重新分区。

---

## 2. xhost_read 路径现状（iter-17A 收尾时）

### 2.1 当前 read 路径概览（src/cxl_kv_ops_A.cc 摘要）

```
Worker (host 0) 发起 GET(key)，key 属 host 1
  ↓ Stage R1: 路由 + 找出 owner=host 1
  ↓ Stage R2: cache_pool lookup（本地 cache 命中？）
       hit  → 直接 return（不走 xhost）
       miss → 继续
  ↓ Stage R3: read_ring[host0→host1][shard].push(GetReq)，flush_line, sfence
  ↓ Stage R4: wait_for_resp（spin on ack_op_id）
              ┊ ─ host 1 ReadRecv drain ring slot
              ┊ ─ host 1 hashtable lookup → 读 value 字节
              ┊ ─ host 1 read_staging[host1→host0][shard].write(payload)
              ┊ ─ host 1 publish ack_op_id（atomic store + flush_line）
  ↓ Stage R5: read response staging slot → copy value bytes
  ↓ Stage R6: cache_pool insert（subject to LRU evict）
  ↓ Stage R7: return to YCSB caller
```

8 个 stage 是当前估计，实际 decomp 后可能合并/拆分。

### 2.2 已知问题（Phase 4.1 修复目标）

- iter-17A 的 multi-shard read 路径**未通过 YCSB workloadc 验证**：T=16 N=4 = 0.016 Mops vs N=0 = 3.47 Mops，**217× 退化**。诊断结果（猜测）是 read 路径里 ReadRingMatrix shard routing 与 ReadStagingMatrix 的 staging slot routing **不对齐**（或类似的 cross-shard hazard）。
- iter-17A 写过 ReadRecv 线程 spawning + CPU pinning + packing，但因为 xhost_write 测试没动 read 路径，**这条路径在 multi-receiver 下从未通过功能性测试**。

### 2.3 与 iter-17A 共用的基础设施

- `cxl_pending_ring.h`、`cxl_ring_routing.h` 已支持 multi-shard
- `cxl_probe.h` 已实现 RDTSCP+LFENCE probe + per-thread mmap'd ring（Phase 1 直接复用，**不重写 timing 机制**）
- `compute_receiver_layout()`（src/cxl_kv_ops_A.cc:741）— iter-18A 复用，但需注意 iter-17A Exp 3 暴露的 **CPU oversubscription** 问题（force ∈ {8, 16} 时 modulo-wrap 导致 collapse）
- `set_ring_routing_mode()` env 切换 worker_id / key_hash — read 路径必须**同步切换**到一样的 mode（否则 cross-host ack routing 错位）

---

## 3. Phase 列表（按顺序执行，每个 phase 有 HARD delivery 条款）

### Phase 1 — Stage decomp 框架（RDTSCP+LFENCE timing）

**目标**: 为 read 路径建 iter-16A 同款的 probe→latency→stage 三层。

| 任务 | 交付物 |
|---|---|
| 1.1 | 在 src/cxl_kv_ops_A.cc 读路径关键位置插入 ≥10 个 `PROBE_OP("XR*", op_id)` 点；**timing 复用 cxl_probe.h 的 RDTSCP+LFENCE（与 iter-16A xhost_write 一致）**，不引入 clock_gettime |
| 1.2 | Worker side 标签：`XRS1S/XRS1E/XRS2E/XRS3E/XRS4E/XRS5E/XRS6E`；Receiver side 标签：`XRR1S/XRR1E/XRR2E/XRR3E/XRR4E` |
| 1.3 | 添加 stage 汇总：8 个 stage（worker 6 + receiver-side 2 大块）；offline analyzer 模板复用 [scripts/iter16A_xhost_decomp_analyze.py](../../scripts/iter16A_xhost_decomp_analyze.py)，新建 iter18A_read_decomp_analyze.py，`--cpu-ghz 2.4` 同（g3/g4 Xeon TSC 标称频率） |
| 1.4 | 编译开关 `FUSEE_PROBE=1 -DFUSEE_READ_PROBE=1`（或新加 `FUSEE_READ_PROBE`），默认关；与 cxl_probe.h 已有的 `FUSEE_PROBE_PATH` 风格一致 |
| 1.5 | 单元 sanity：T=1 单 op probe-on 跑完，验证 ∑stage ≈ wall-clock 用时（±5 % 容差）；probe-on vs probe-off 整体吞吐差 <5 % |

**HARD delivery**: 1.5 验证通过；并附 T=1 / T=16 / T=64 的 path-decomp 后缀格式样本（与 iter-16A 写路径输出格式同构）。

---

### Phase 2 — 4 个 sweep（参数 user-revised 2026-05-23）

| Sweep | 维度 | reps | gates |
|---|---|---|---|
| **2.1 T-sweep** | T ∈ {1, 2, 4, 8, 16, 32, 64}, V=1024, dist=zipf-0.99 | 3 | anomaly-scan; doubling-ratio ≥1.5× pre-saturation |
| **2.2 V-sweep** | V ∈ {64, 256, 512, 1024}, T=16, dist=zipf-0.99 | 3 | anomaly-scan |
| **2.3 dist-sweep** | dist ∈ {uniform, zipf-0.5, zipf-0.99, zipf-1.5}, **T=64**, V=1024 | 3 | anomaly-scan |
| **2.4 perfstat** | **T ∈ {1, 8, 64}**, perf stat -e cycles,instructions,cache-misses,LLC-loads,LLC-load-misses on ReadRecv tids | 1 (perf 是诊断不是 thpt 测) | IPC < 0.15 → memory-bound flag |

总 cells: 7 + 4 + 4 + 3 = 18 cells × 3 reps = 54 runs（perfstat 单 rep）= 51 runs.

**HARD delivery**: 每个 sweep 输出 `grid.csv` + `medians.csv` + path-decomp 后缀 stats + `gap_to_target.md`（含 anomaly section）。

**与之前版本的 diff**:
- 2.2 V 起点 **8 → 64**（V=8 payload 太小，对读路径主要观察意义不大）
- 2.3 T=16 → **T=64**（饱和区才显出 dist 差异；与 iter-17A Exp 2 同思路）
- 2.3 zipf-1.4 → **zipf-1.5**
- 2.4 T=16 → **T=8**（捕捉早期 thread 扩展段，与 T=1 配合看从单 thread 到小并发的转变）

---

### Phase 3 — 逐 stage 1-2 行迭代 path opt（合并原 Phase 3+4，参考 iter-17A xhost_write 的方法）

**目标**: 不做"一次大重构"，而是**逐 stage 1-2 行代码动静**（参 iter-17A xhost_write 的 Stage 1+2 / 3 / 4 / 5 / 6 / 7 / bucket-double-flush 共 7 个 commit），每个改动配 smoke test，量正才 keep，量负 (或 ≤ 噪声) revert，跑完所有 stage 等于完成整条路径的优化。

### 3.0 总规则（iter-17A 同款）

- **每个 stage 一个 commit**。一次改动尽量 ≤ 2 行实质代码（注释/重命名/小常量调整不计）。
- **每改一次必跑 smoke test**：T ∈ {8, 32, 64} × V=1024 × zipf-0.99 × 3 rep（9 runs），与 Phase 2.1 同一 cell 的 median 对比。
- **Keep/Revert 判据（阈值与 iter-17A 一致）**:
  - thpt 提升 ≥ 5 %（最弱 cell）→ **Keep**；commit message 标注 `[Stage N opt][I9] +X-Y% on T={8,32,64}`
  - thpt 在 ±5 % 之内（噪声内）→ **Audit-only commit**，标注 `[Stage N audit][I9] no measured impact, keep code for clarity`
  - thpt 负向 → **Revert**，commit message 标注 `[Stage N revert][I9] -X% regression on T=N, reverted`
- **任何 Keep 改动**必须同时跑 **hash-diff 5 cell**（5 个 worker 各算 1 万 op 的 H0/H1 hash，须匹配）以保证 CORRECTNESS 不破。
- **Audit 笔记**: 每改一次先写 50-200 字 audit note 到 commit message body：哪一两行代码看着可疑、改动 hypothesis、预期影响。Iter-17A 写路径 commit body 是模板。

### 3.1 候选 audit-and-modify 列表（按 Phase 2 决出的 dominant stage 排序，最多 dominant 的先做；下面列预估，实际顺序待 Phase 2 出来定）

| 候选改动 | 灵感来源 (iter-17A 写) | 改动行数估计 |
|---|---|---|
| Stage R3 (ring push) `flush_line` 合批 | 写路径 Stage 1+2 lfence | 2-3 行 |
| Stage R3 sfence 收 fence-fan-out | 写路径 Stage 5 lfence (+21-23%) | 1-2 行 |
| Stage R4 worker spin loop: 改 `pause` 节奏 / 改 budget | 写路径 Stage 6 lfence A+C | 1 行 |
| Stage RR1 (receiver drain) ring head 读 `acquire` 改弱 fence | 写路径 Stage 7 dead-code 思路 | 1 行 |
| Stage RR2 (hashtable lookup) bucket prefetch | 写路径 bucket double-flush -36-37% 反向：受路径**少**而非多 flush | 1-2 行 |
| Stage RR3 (staging write) single sfence per op vs per slot | iter-16A H9 single-flush +22% | 1-2 行 |
| Stage RR4 (ack publish) atomic_store + 单 flush_line | 写路径 receiver-side opt | 1 行 |
| Stage R5 (worker copy from staging) `memcpy` vs avx | 通用 µopt | 1 行 |

**每个候选 = 一次 ≤ 2 行 commit + smoke test**。预估 7-9 commit；按 iter-17A 经验 ~3-5 个 keep，~2-3 个 audit-only，~1-2 个 revert。

### 3.2 整体 HARD delivery（Phase 3 关闭条件）

- 上面 8 个候选**每个都尝试过**（不允许跳过；audit-only / revert 也算"尝试过"）。
- 累计 keep 改动需在 Phase 2.1 baseline 之上至少**≥10 % 全 T 段提升**；或者每个 keep 改动单独 ≥5 %，可累加证明 ≥10 % 全 T 段。
- 任意 cell 出现 hash-diff FAIL → 该 commit revert。
- Phase 3 完成时输出 iter18A_read_opt_summary.md（仿 iter17A_xhost_write_path_opt_summary.md），列每个 stage 改动 + delta 表 + keep/revert 决策矩阵。

### 3.3 §XIII RAP 边界

- **轻量 audit note**（commit body 50-200 字 + smoke test）适用于：fence 调整 / flush 合批 / pause 节奏 / atomic memory_order 弱化 / dead code 删除等"局部不动语义"类。
- **完整 §XIII RAP**（≥6 attack vectors / 6 类）适用于：staging matrix layout 改动 / receiver loop 算法换骨 / ring 数据结构语义改 / 引入新 fence 模型等"动语义"类。任意 Phase 3 commit 触动后者必须 stop-and-ask + 写 RAP 走流程，不允许"塞进 1-2 行 commit 蒙混"。

---

### Phase 4 — Multi-ring + multi-receiver for read（含 iter-17A 217× 退化 bug 修复）

**目标**: 把 iter-17A 已构筑的 3D ring matrix 在 read 路径上**真正接入并验证**。

| 任务 | 交付物 |
|---|---|
| 4.1 | 复现 iter-17A 的 YCSB workloadc T=16 N=4 = 0.016 Mops 退化；定位根因（最可能：ReadStagingMatrix 的 ack slot index 与 ReadRingMatrix req slot index 路由不一致；或 ReadRecv 在多 shard 下 spin 在错的 ring）；RCA write-up 含具体源码行号 + 修复方案 |
| 4.2 | 实现修复，rsync + rebuild g3/g4 |
| 4.3 | Wire xhost_read 走 `ReadRingMatrix[*][*][shard]` 路径；shard = `compute_ring_idx(key)` 与 write 同 router |
| 4.4 | Hash-diff 8 cell（N=0 / N=4 / N=8 × Plan A / Plan B）**必须 8/8 PASS** |
| 4.5 | T-sweep N ∈ {0, 4, 8} × routing ∈ {worker_id, key_hash} × V=1024 zipf-0.99 3 rep |
| 4.6 | 8-group 对比图（同 iter-17A 风格）：1=iter15A read baseline / 2=iter18A path opt N=0 (Phase 3 末) / 3-5=Plan A N=0/4/8 / 6-8=Plan B N=0/4/8 |
| 4.7 | uniform 对照同 8-group |

**HARD delivery**: 4.4 hash-diff 8/8 PASS。任何 FAIL 阻断后续。

---

### Phase 5 — End-to-end YCSB sanity

**目标**: 在 multi-ring read 接入后跑 YCSB workloadc 的小范围 sanity（**不**做 80-cell sweep，那是 iter-19A）。

| 任务 | 交付物 |
|---|---|
| 5.1 | YCSB workloadc T ∈ {16, 32, 64} × N ∈ {0, 4} × 3 rep |
| 5.2 | 与 iter-17A T=16 N=4 = 0.016 Mops 对比；要求 N=4 ≥ N=0 baseline 的 50 %（修 bug 后） |
| 5.3 | 若 5.2 通过 → multi-ring read 验证成功，进 iter-19A 做大 YCSB |
| 5.4 | 若 5.2 失败 → 回到 Phase 4.1 重新 RCA |

**HARD delivery**: 5.2 必须有数值结果 + 与 N=0 比值 + 与 iter-17A 老结果 0.016 Mops 对比。

---

## 4. 实验 grid 汇总（用于 RAP 时 vector A2 / A8 防被 attack）

```
Phase 2  Decomp sweeps:                T 7 + V 4 + dist 4 + perfstat 3 = 18 cells × 3 rep = 54 runs (perfstat 1 rep so 51 runs)
Phase 3  Per-stage opt smoke tests:    9 candidates × T(8/32/64) × V=1024 × zipf × 3 rep = 81 runs (estimate)
Phase 4  Multi-ring sweep:             T(7) × N(0/4/8) × routing(2) = 42 cells × 3 rep × 2 dist = 252 runs
Phase 5  YCSB sanity:                  T(3) × N(2) × 3 rep × 1 workload = 18 runs
```

总 sweep 体量 ~400 runs（不含 5-rep 重测 / hash-diff cells）。

---

## 5. Constraints / 不变量（per CLAUDE.md + spec §I/AP）

- **C1**（沿用 iter-13A）: hash_str FNV-1a 不动；test driver 与 sharding_hash_u64 路由一致。
- **C2**（沿用 iter-17A）: ReadRingMatrix entry 内嵌 key + sequence；value bytes 走 staging matrix 不走 ring slot（≤ 1088B 内嵌 = 编译期 reject）。
- **C3**: CXL atomic 字段读后写必 `flush_line + sfence`（iter-4A 教训）。
- **C4**: ReadRecv CPU pinning 与 WriteRecv 同 pool（start_cpu = max(T, 64)）；本 iter **不**改 CPU 池策略。
- **C5**: 任何 spec 改动走 §XIII RAP → 不允许悄悄修 spec 让实现"合法"（iter-3A Finding-1 教训）。
- **C6**: ReadRecv 在 multi-shard 下的 routing 公式与 WriteRecv 完全一致（read response 必须能找回 originating worker）。
- **C7**: Iter-17A Exp 3 暴露的 **modulo-wrap CPU oversubscription** 在本 iter 修掉或避开（推荐 cap force_threads_per_type ≤ pool_size/3）。
- **C8** (NEW): Phase 3 任一 commit 的代码改动行数 ≤ 2（注释/重命名不计），改动语义类型属轻量（fence / flush 合批 / pause 节奏 / atomic order 弱化 / dead-code）才适用"audit note + smoke test"；属重量（layout / loop algo / 新 fence 模型）必须 stop-and-ask + §XIII RAP。

---

## 6. 待用户确认（QR1–QR5；QR2 已经在本次 revise 中确定）

1. **QR1**: iter-18A 是否独立 iter？或者 merge 进 iter-19A（YCSB validation）作单 iter？建议独立，理由：read decomp + per-stage opt + multi-ring 的代码改动量与 iter-16A/17A 相当，单 iter 单 focus 风险低。
2. ~~**QR2**: Phase 3 路径优化设计可不可以预选 single-flush？~~ **已确定**：Phase 3 改成逐 stage 迭代，不预选；候选列表由 Phase 2 dominant stage 排序后定。
3. **QR3** (was QR3): Phase 4 是否要做 8-group 全套对比（含 iter-15A read baseline + Plan B key_hash N=0）？或精简到 5-group（去掉冗余 Plan A N=0 ≡ Plan B N=0 + Plan B N=4 加 iter-17A 已知坏 case）？建议**全 8-group**，与 iter-17A 报告对齐方便横向比较。
4. **QR4** (was QR4): Phase 5 YCSB sanity 用 workloadc only 还是含 workloada（50 % 写 + 50 % 读）？建议 **workloadc only**；workloada 已混入 write path，无法单独验 read。
5. **QR5** (was QR5): 跨 iter 性能门槛设？建议 read peak ≥ 8 Mops/s（粗略推算 iter-17A write 6.6 Mops/s × 1.2 因子，read 无 inval broadcast）。**可商榷**。

---

## 7. Phase delivery audit table（iter 末必填，per CLAUDE.md precedent #3）

iter-18A 关 iter 前必须填这张表 + 每行解释（任何 ⚠/❌ 没用户授权 message 引用 → iter 不算完成）：

| Sub-phase | Plan | Delivered | Status |
|---|---|---|---|
| 1.1 | ≥10 PROBE_OP 点 (RDTSCP+LFENCE) 插入 read 路径 | _填_ | ✅/⚠/❌ |
| 1.2 | Worker XRS* + Receiver XRR* 标签集 | _填_ | ✅/⚠/❌ |
| 1.3 | iter18A_read_decomp_analyze.py + 8 stage 汇总 | _填_ | ✅/⚠/❌ |
| 1.4 | FUSEE_READ_PROBE 编译开关默认关 | _填_ | ✅/⚠/❌ |
| 1.5 | sanity ∑stage≈wall-clock ±5%；probe-on vs off ≤5% | _填_ | ✅/⚠/❌ |
| 2.1 | T-sweep 7 cells × 3 rep + anomaly scan | _填_ | ✅/⚠/❌ |
| 2.2 | V-sweep 4 cells × 3 rep | _填_ | ✅/⚠/❌ |
| 2.3 | dist-sweep 4 cells × 3 rep (T=64) | _填_ | ✅/⚠/❌ |
| 2.4 | perfstat 3 cells (T=1/8/64) | _填_ | ✅/⚠/❌ |
| 3.0 | iter18A_read_opt_summary.md 列每 stage delta + keep/revert 决策矩阵 | _填_ | ✅/⚠/❌ |
| 3.1-3.x | 每个候选 stage 一个 commit (predicted 7-9 commits) — 全部尝试过（keep/audit/revert 都算） | _填 (列出每 commit hash + delta)_ | ✅/⚠/❌ |
| 3.2 | 累计 keep 改动 ≥10 % 全 T 段提升 vs Phase 2.1 baseline | _填_ | ✅/⚠/❌ |
| 4.1 | 217× 退化 RCA 含源码行 + 修复方案 | _填_ | ✅/⚠/❌ |
| 4.2 | bug 修复 + 部署 | _填_ | ✅/⚠/❌ |
| 4.3 | xhost_read wire 多 shard 路径通 | _填_ | ✅/⚠/❌ |
| 4.4 | hash-diff 8/8 PASS | _填_ | ✅/⚠/❌ |
| 4.5 | T-sweep × N × routing × zipf 3 rep | _填_ | ✅/⚠/❌ |
| 4.6 | 8-group 对比图 zipf-0.99 | _填_ | ✅/⚠/❌ |
| 4.7 | 8-group 对比图 uniform | _填_ | ✅/⚠/❌ |
| 5.1 | YCSB workloadc T×N×3 rep 跑齐 | _填_ | ✅/⚠/❌ |
| 5.2 | N=4 ≥ 50 % N=0 baseline | _填_ | ✅/⚠/❌ |

任何 ⚠/❌ 必须引用用户授权 message 才允许进总结（precedent #3）。

---

## 8. 与 iter-17A 知识资产连续性

- iter-17A 已落地（feat/cxl-migration HEAD）的代码继续用：
  - `compute_receiver_layout()`（C7 修过 modulo-wrap 后 iter-18A 复用）
  - `set_ring_routing_mode()` env / `g_ring_routing_mode` global
  - `compute_ring_idx(key)` / `worker_ring_idx_for_key()` helpers
  - WriteRingMatrix / ReadRingMatrix / InvalRingMatrix 3D 数据结构
  - **cxl_probe.h RDTSCP+LFENCE 机制** — Phase 1 直接 include 复用，不另写
- iter-17A 的 8-group 对比图模板（scripts/iter17A_8group_compare.py、scripts/iter17A_uniform_vs_zipf_compare.py）可改名 iter18A_ 直接复用，只改 sweep dir 路径。
- iter-16A 的 probe 框架代码（src/cxl_kv_ops_A.cc 的 `# PATH host=...` 输出格式）read 路径里 mirror 即可，不创新格式。
- iter-16A 的 offline analyzer（scripts/iter16A_xhost_decomp_analyze.py）改名 iter18A_read_decomp_analyze.py，stage 字典换 read tag (XR*) 即可。

---

## 9. 与原版（2026-05-23 Round 1）的 diff 速查

| 改动 | 原版 | 新版 | 原因 |
|---|---|---|---|
| Phase 0 | Baseline + bug RCA | **删除** | 与 Phase 2 baseline + Phase 4.1 bug RCA 重复 |
| Phase 1 timing | clock_gettime | **RDTSCP+LFENCE** | 用户要求与 iter-16A xhost_write 一致；cxl_probe.h 现有机制直接复用 |
| Phase 2.2 V 起点 | 8 | **64** | V=8 payload 太小，读路径学不到东西 |
| Phase 2.3 T | 16 | **64** | 饱和区才显出 dist 差异 |
| Phase 2.3 zipf 上限 | 1.4 | **1.5** | 用户指定 |
| Phase 2.4 中间 T | 16 | **8** | 捕捉 T=1→8 早期扩展段 |
| Phase 3 | "起草 1 个 RAP, 然后 Phase 4 一次性 wire" | **逐 stage 1-2 行 + smoke test + keep/revert/audit，~7-9 个 commit** | 用户要求复用 iter-17A xhost_write Stage 1-7 + bucket-double-flush 的 7-commit 迭代法；每改一两行立即测量，避免一次大重构后无法定位 regression |
| Phase 4/5/6 编号 | 4 (wire) / 5 (multi-ring) / 6 (YCSB) | **4 (multi-ring) / 5 (YCSB)** | Phase 3 合并了原 4 |
| C8 (新增) | — | 限制 Phase 3 commit ≤ 2 行；重量改动强制走 §XIII RAP | 防止"塞进 1-2 行 commit 蒙混"，保证 iter-17A 写路径方法的可重复性 |

---

**Status**: 二轮草稿（user-revised）。待 QR1/QR3/QR4/QR5 答复后转 formal task plan，启动 Phase 1。
