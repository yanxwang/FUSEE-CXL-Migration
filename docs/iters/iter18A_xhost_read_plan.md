# iter-18A — xhost_read 阶段分解 + path optimization + multi-ring/receiver scaling

**草稿日期**: 2026-05-23
**规划方法**: 复用 iter-16A (decomp / probe / stage) + iter-17A (multi-ring + receiver) 对 xhost_write 完成的全套分析、实现、实验流程，原样套到 xhost_read 上。

---

## 0. Background — 为什么现在做 read

- iter-16A 已为 **xhost_write** 建好 stage-decomp 框架（15 probes / 11 latencies / 8 stages），并通过它定位 H9 single-flush opt（+22 % 全 T 段）。
- iter-17A 在 **xhost_write** 上完成 multi-ring + multi-receiver scaling，集群峰值 6.63 Mops/s（T=64 N=4 zipf-0.99）。
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

| 类别 | iter-16A xhost_write 对应物 | iter-18A xhost_read 任务 |
|---|---|---|
| Stage decomp 框架 | 15 probes / 11 latencies / 8 stages | 为 read 路径建同样的 probe→latency→stage 三层 |
| 4 个 sweep | T-sweep / V-sweep / dist-sweep / perfstat | 同 4 种，每种 ≥3 reps + median + anomaly scan |
| Single-flush opt（H9）| 写路径 single sfence per op | 读路径对应的 latency hotspot opt（待 decomp 暴露） |
| Multi-ring matrix | 3D (src,dst,shard) WriteRingMatrix | 3D ReadRingMatrix；与 write 同 actual_shards = ceil(T/N) |
| Multi-receiver | N ReadRecv / pinned / 软件 packing | 同上 |
| Plan A worker_id / Plan B key_hash | iter-17A 2 套 routing 实现 | 复用同 router；read 的 routing 公式与 write 必须一致（避免 hash diff） |
| Sweep grid 对比 | 7-/8-group 对比图 | 同种 8-group 对比 |

### 1.2 Out-of-scope (defer)

- YCSB full sweep（80-cell × 5-rep）— 等 multi-ring read 稳定后再做（iter-19A 或 iter-18A Phase 6+）。
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

### 2.2 已知问题

- iter-17A 的 multi-shard read 路径**未通过 YCSB workloadc 验证**：T=16 N=4 = 0.016 Mops vs N=0 = 3.47 Mops，**217× 退化**。诊断结果是 read 路径里 ReadRingMatrix shard routing 与 ReadStagingMatrix 的 staging slot routing **不对齐**（或类似的 cross-shard hazard）。需要在 iter-18A Phase 1 之前做一次 hash-diff regression test 把 bug 复现并定位。
- iter-17A 写过 ReadRecv 线程 spawning + CPU pinning + packing，但因为 xhost_write 测试没动 read 路径，**这条路径在 multi-receiver 下从未通过功能性测试**。

### 2.3 与 iter-17A 共用的基础设施

- `cxl_pending_ring.h`、`cxl_ring_routing.h` 已支持 multi-shard
- `compute_receiver_layout()`（src/cxl_kv_ops_A.cc:741）— iter-18A 复用，但需注意 iter-17A Exp 3 暴露的 **CPU oversubscription** 问题（force ∈ {8, 16} 时 modulo-wrap 导致 collapse）
- `set_ring_routing_mode()` env 切换 worker_id / key_hash — read 路径必须**同步切换**到一样的 mode（否则 cross-host ack routing 错位）

---

## 3. Phase 列表（按顺序执行，每个 phase 有 HARD delivery 条款）

### Phase 0 — Baseline + read-path multi-shard bug 定位

**目标**: 在动任何东西之前先把现状量出来 + 复现 iter-17A 的 217× 退化。

| 任务 | 交付物 |
|---|---|
| 0.1 | T-sweep xhost_read N=0（单 ring）3 rep median，记录 baseline thpt + p50/p99 latency |
| 0.2 | T-sweep xhost_read N=4 Plan A worker_id 3 rep median |
| 0.3 | 计算 N=4 vs N=0 比值；只要任何 cell < 50 % baseline 就启动 RCA |
| 0.4 | Hash-diff 对照（host 0 vs host 1 各 5 cell） |
| 0.5 | RCA write-up：定位 read 路径里 multi-shard 退化的根因（routing 不一致？staging slot index 不对？ack ring 不存在？） |

**HARD delivery**: 0.5 必须有具体源码行号 + 修复方案描述。**不允许停在"猜测"**。

---

### Phase 1 — Stage decomp 框架

**目标**: 为 read 路径建 iter-16A 同款的 probe→latency→stage 三层。

| 任务 | 交付物 |
|---|---|
| 1.1 | 在 src/cxl_kv_ops_A.cc 读路径关键位置插入 ≥10 个 PROBE 点；每个 probe 用 `clock_gettime(MONOTONIC)` 取 ns，TLS-aggregated（同 iter-16A） |
| 1.2 | 添加对应的 latency dict：worker side R1-R2-R3-R4-R5-R6 + receiver side RR1-RR2-RR3-RR4 |
| 1.3 | 添加 stage 汇总：8 个 stage（worker 6 + receiver-side 2 大块），输出 path-decomp 后缀 |
| 1.4 | 编译开关 `FUSEE_READ_PROBE=1`，默认关 |
| 1.5 | 单元 sanity：T=1 单 op 跑完，验证 ∑stage = wall-clock 用时（±5 % 容差） |

**HARD delivery**: 1.5 验证通过 + probe 注入不影响 thpt > 5 %（off-vs-on 对比）。

---

### Phase 2 — 4 个 sweep（同 iter-16A）

| Sweep | 维度 | reps | gates |
|---|---|---|---|
| 2.1 T-sweep | T ∈ {1,2,4,8,16,32,64}, V=1024, dist=zipf-0.99 | 3 | anomaly-scan; doubling-ratio ≥1.5× pre-saturation |
| 2.2 V-sweep | V ∈ {8,256,512,1024}, T=16, dist=zipf-0.99 | 3 | anomaly-scan |
| 2.3 dist-sweep | dist ∈ {uniform, zipf-0.5, zipf-0.99, zipf-1.4}, T=16, V=1024 | 3 | anomaly-scan |
| 2.4 perfstat | T ∈ {1,16,64}, perf stat -e cycles,instructions,cache-misses,LLC-loads,LLC-load-misses on ReadRecv tids | 1 (perf 是诊断不是 thpt 测) | IPC < 0.15 → memory-bound flag |

**HARD delivery**: 每个 sweep 输出 `grid.csv` + `medians.csv` + path-decomp 后缀 stats + `gap_to_target.md`（含 anomaly section）。

---

### Phase 3 — Dominant stage 识别 + 优化设计 RAP

**目标**: 用 Phase 2 数据画出 stage breakdown vs T，找到 dominant stage（≥30 % wall-clock），起草 1 个优化设计走 §XIII RAP。

| 任务 | 交付物 |
|---|---|
| 3.1 | iter-18A_read_stage_decomp.md：8 stage × 7 T 的 stack chart，每 stage 标注 % 占比 |
| 3.2 | 识别 top-1 dominant stage（参照 iter-16A 写路径 Stage 5 ack_wait 72-99 %） |
| 3.3 | 起草 1 个 optimization proposal 走 §XIII RAP（≥6 attack vectors 涵盖 6 类） |
| 3.4 | RAP 用户批准后才能 wire（iter-13A/iter-14A 教训） |

**HARD delivery**: 3.3 RAP 写完进 docs/iters/iter18A_read_opt_RAP.md；不写就不允许 Phase 4 wire。

---

### Phase 4 — Path optimization wire（基于 Phase 3 RAP）

**目标**: 实现 Phase 3 提出的 1 个 optimization，对照 sweep 验证。

| 任务 | 交付物 |
|---|---|
| 4.1 | 实现 optimization，rsync + rebuild g3/g4 |
| 4.2 | hash-diff 5 cell ✓（不变） |
| 4.3 | T-sweep V=1024 zipf-0.99 3 rep median；对比 Phase 2.1 baseline；要求 ≥10 % 全 T 段提升（或解释为何 dist/V 特定） |
| 4.4 | 把 4.3 的 thpt 写进 iter18A summary §"path opt 收益" |

**HARD delivery**: 即使 opt 失败（thpt 不升或反降），也要 documented + RAP 重新攻击。**不允许 silently abandon**。

---

### Phase 5 — Multi-ring + multi-receiver for read

**目标**: 把 iter-17A 已构筑的 3D ring matrix 在 read 路径上**真正接入并验证**（Phase 0.5 定位的 bug 必须在这里修掉）。

| 任务 | 交付物 |
|---|---|
| 5.1 | 修 Phase 0.5 暴露的 multi-shard read 退化 bug（最可能：ReadStagingMatrix 的 ack slot index 与 ReadRingMatrix req slot index 路由不一致；或 ReadRecv 在多 shard 下 spin 在错的 ring） |
| 5.2 | Wire xhost_read 走 ReadRingMatrix[*][*][shard] 路径；shard = compute_ring_idx(key) 与 write 同 router |
| 5.3 | Hash-diff 8 cell（N=0 / N=4 / N=8 × Plan A / Plan B） |
| 5.4 | T-sweep N ∈ {0, 4, 8} × routing ∈ {worker_id, key_hash} × V=1024 zipf-0.99 3 rep |
| 5.5 | 8-group 对比图（同 iter-17A 风格）：1=iter15A read baseline / 2=iter18A path opt N=0 / 3-5=Plan A N=0/4/8 / 6-8=Plan B N=0/4/8 |
| 5.6 | uniform 对照同 8-group |

**HARD delivery**: 5.3 hash-diff 8/8 PASS。任何 FAIL 阻断后续。

---

### Phase 6 — End-to-end YCSB sanity

**目标**: 在 multi-ring read 接入后跑 YCSB workloadc 的小范围 sanity（**不**做 80-cell sweep，那是 iter-19A）。

| 任务 | 交付物 |
|---|---|
| 6.1 | YCSB workloadc T ∈ {16, 32, 64} × N ∈ {0, 4} × 3 rep |
| 6.2 | 与 iter-17A T=16 N=4 = 0.016 Mops 对比；要求 ≥ N=0 baseline 的 50 %（修 bug 后） |
| 6.3 | 若 6.2 通过 → multi-ring read 验证成功，进 iter-19A 做大 YCSB |
| 6.4 | 若 6.2 失败 → 回到 Phase 0.5 重新 RCA |

**HARD delivery**: 6.2 必须有数值结果 + 与 N=0 比值 + 与 iter-17A 老结果对比。

---

## 4. 实验 grid 汇总（用于 RAP 时 vector A2 / A8 防被 attack）

```
Phase 0  Baseline + bug RCA:        ~ 24 cells
Phase 2  Decomp sweeps:              T-sweep 7 + V-sweep 4 + dist-sweep 4 + perfstat 3 = 18 cells
Phase 4  Path opt validation:         7 cells (T-sweep)
Phase 5  Multi-ring sweep:           T-sweep 7 × N(0/4/8) × routing(2) = 42 cells × 2 dist = 84 cells
Phase 6  YCSB sanity:                3 T × 2 N × 3 rep × 5 workloads = 90 cells (or smaller subset)
```

总 sweep 体量 ~220 cells（不含 5-rep 重测）。

---

## 5. Constraints / 不变量（per CLAUDE.md + spec §I/AP）

- **C1**（沿用 iter-13A）: hash_str FNV-1a 不动；test driver 与 sharding_hash_u64 路由一致。
- **C2**（沿用 iter-17A）: ReadRingMatrix entry 内嵌 key + sequence；value bytes 走 staging matrix 不走 ring slot（≤ 1088B 内嵌 = 编译期 reject）。
- **C3**: CXL atomic 字段读后写必 `flush_line + sfence`（iter-4A 教训）。
- **C4**: ReadRecv CPU pinning 与 WriteRecv 同 pool（start_cpu = max(T, 64)）；本 iter **不**改 CPU 池策略。
- **C5**: 任何 spec 改动走 P3 → 不允许悄悄修 spec 让实现"合法"（iter-3A Finding-1 教训）。
- **C6**: ReadRecv 在 multi-shard 下的 routing 公式与 WriteRecv 完全一致（read response 必须能找回 originating worker）。
- **C7**: Iter-17A Exp 3 暴露的 **modulo-wrap CPU oversubscription** 在本 iter 修掉或避开（推荐 cap force_threads_per_type ≤ pool_size/3）。

---

## 6. 待用户确认（QR1–QR6）

1. **QR1**: iter-18A 是否独立 iter？或者 merge 进 iter-19A（YCSB validation）作单 iter？建议独立，理由：read decomp + path opt 的代码改动量与 iter-16A/17A 相当，单 iter 单 focus 风险低。
2. **QR2**: Phase 3 路径优化设计可不可以**预选** "single-flush ack publish"（H9 写路径同款思路 → 读路径 receiver-side single sfence）？或保留 decomp 后再定？建议**保留**至 decomp 后定，防止盲目移植。
3. **QR3**: Phase 5 是否要做 8-group 全套对比（含 iter-15A read baseline + Plan B key_hash N=0）？或精简到 5-group（去掉冗余 Plan A N=0 ≡ Plan B N=0 + Plan B N=4 加 iter-17A 已知坏 case）？建议**全 8-group**，与 iter-17A 报告对齐方便横向比较。
4. **QR4**: Phase 6 YCSB sanity 用 workloadc only 还是含 workloada（50 % 写 + 50 % 读）？建议 **workloadc only**；workloada 已混入 write path，无法单独验 read。
5. **QR5**: 跨 iter 性能门槛设？建议 read peak ≥ 8 Mops/s（粗略推算 iter-17A write 6.6 Mops/s × 1.2 因子，read 无 inval broadcast）。**可商榷**。
6. **QR6**: CXL hardware 不可用窗口是否会出现？若 g3/g4 PXE 重启 → 触发 `feedback_rekey_slave.md` 标准恢复流程；按 iter-17A 节奏估计**不会**阻塞。

---

## 7. Phase delivery audit table（iter 末必填，per CLAUDE.md precedent #3）

iter-18A 关 iter 前必须填这张表 + 每行解释：

| Sub-phase | Plan | Delivered | Status |
|---|---|---|---|
| 0.1 | T-sweep xhost_read N=0 baseline | _填_ | ✅/⚠/❌ |
| 0.2 | T-sweep xhost_read N=4 | _填_ | ✅/⚠/❌ |
| 0.5 | RCA write-up 含源码行 + 修复方案 | _填_ | ✅/⚠/❌ |
| 1.1–1.5 | Probe / latency / stage / off-by-default / sanity 5 步 | _填_ | ✅/⚠/❌ |
| 2.1 | T-sweep 7 cells | _填_ | ✅/⚠/❌ |
| 2.2 | V-sweep 4 cells | _填_ | ✅/⚠/❌ |
| 2.3 | dist-sweep 4 cells | _填_ | ✅/⚠/❌ |
| 2.4 | perfstat 3 cells | _填_ | ✅/⚠/❌ |
| 3.1 | stage breakdown chart | _填_ | ✅/⚠/❌ |
| 3.2 | top-1 dominant stage identified | _填_ | ✅/⚠/❌ |
| 3.3 | optimization RAP doc | _填_ | ✅/⚠/❌ |
| 3.4 | user RAP 批准 | _填_ | ✅/⚠/❌ |
| 4.1–4.4 | path opt wire + sweep + ≥10 % proof | _填_ | ✅/⚠/❌ |
| 5.1 | multi-shard read bug 修复 | _填_ | ✅/⚠/❌ |
| 5.2 | wire 通过 | _填_ | ✅/⚠/❌ |
| 5.3 | hash-diff 8/8 | _填_ | ✅/⚠/❌ |
| 5.4 | multi-ring T-sweep | _填_ | ✅/⚠/❌ |
| 5.5 | 8-group zipf 对比 | _填_ | ✅/⚠/❌ |
| 5.6 | 8-group uniform 对比 | _填_ | ✅/⚠/❌ |
| 6.1 | YCSB workloadc small sweep | _填_ | ✅/⚠/❌ |
| 6.2 | ≥50 % N=0 baseline 验证 | _填_ | ✅/⚠/❌ |

任何 ⚠/❌ 必须引用用户授权 message 才允许进总结（precedent #3）。

---

## 8. 与 iter-17A 知识资产连续性

- iter-17A 已落地（feat/cxl-migration HEAD）的代码继续用：
  - `compute_receiver_layout()`（修过 modulo-wrap 后 iter-18A 复用）
  - `set_ring_routing_mode()` env / `g_ring_routing_mode` global
  - `compute_ring_idx(key)` / `worker_ring_idx_for_key()` helpers
  - WriteRingMatrix / ReadRingMatrix / InvalRingMatrix 3D 数据结构
- iter-17A 的 8-group 对比图模板（scripts/iter17A_8group_compare.py、scripts/iter17A_uniform_vs_zipf_compare.py）可改名 iter18A_ 直接复用，只改 sweep dir 路径。
- iter-16A 的 probe 框架代码（src/cxl_kv_ops_A.cc 的 `# PATH host=...` 输出格式）read 路径里 mirror 即可，不创新格式。

---

**Status**: 草稿。待用户对 QR1–QR6 答复后转 formal task plan。
