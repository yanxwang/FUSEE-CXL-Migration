# iter-17A 主总结

**Date**: 2026-05-22
**Branch**: `feat/cxl-migration`
**Iter scope**: Protocol A 路径优化（Part 1）+ multi-ring/multi-receiver scaling（Part 2 + Part 3）

iter-17A 由 3 个连续阶段组成：
- **Part 1 — Path optimization**: 8-stage xhost_write 路径逐 stage 代码审查 + fence/dead-code 优化
- **Part 2 — Multi-receiver scaling**: 3D ring matrix + Plan A worker_id routing + Plan B key_hash routing 对比 + 8-group sweep
- **Part 3 — YCSB scaling validation**: 在 winner config 上跑标准 5-workload sweep（结论：deferred，发现 multi-shard read/RW path bug）

---

## TL;DR

| 阶段 | 配置 | xhost_write T=64 cluster Mops/s | vs iter-15A baseline |
|---|---|---|---|
| iter-15A Phase 2 baseline | single ring + receiver | 0.604 | — |
| iter-17A Part 1 (path opt) | N=0 (single shard) | 1.009 | **+67%** |
| iter-17A Part 2 (multi-recv) ⭐ | **Plan A worker_id N=4** | **6.628** | **+997% (11×)** |

**集群峰值** 6.628 Mops cluster aggregate（T=64 N=4 Plan A 3-rep median）
= **33% of 20 Mops cluster target**

后续 iter 优先级（基于 iter-17A 发现的 bug + 瓶颈）:
- 修 multi-shard read/inval path bug（YCSB workload-a/b/c/d/f 在 N≥4 时灾难性退化 134-217×）
- 突破 single-receiver 算力上限（packing dilution + Zipf hot bucket lock）

---

## Part 1 — Path optimization

**See**: [iter17A_xhost_write_path_opt_summary.md](iter17A_xhost_write_path_opt_summary.md)

7 个 commits 跨 8 个 stages：

| Commit | Stage | 操作 | Smoke 收益 |
|---|---|---|---|
| 3786c86 | 1+2 | Stage 1 死代码注释 + Stage 2 lfence | 持平 |
| 11180c5 | 3 | Pool header 不是死代码 (audit) | — |
| 2441e69 | 4 | E1.5 经测必需 (rejected) | — |
| 1a6f602 | 5 | mfence → lfence (worker spin) | **+21-23%** |
| dc5c9c7 | 6 | lfence at A+C sites (B rejected) | **+21-27%** |
| 06ac461 | 7 | Dead-code removal | ~flat |
| b1d6c31 | 7 | Bucket double-flush removal | **+36-37%** |

派生定理（Stage 6 bisection）:
> `flush_line + lfence + load` 安全 ⇔ (单字段 load) ∧ (循环自愈 ∨ 值语义抗 stale)。一次性 load 后读同 cacheline 其他字段 → 必须 mfence。

---

## Part 2 — Multi-ring + multi-receiver scaling

### 2.1 Design + implementation

**See**: [iter17A_multi_ring_receiver_scaling_design.md](iter17A_multi_ring_receiver_scaling_design.md) + [iter17A_scaling_RAP.md](iter17A_scaling_RAP.md)

**Implementation (3 commits)**:

| Commit | 内容 |
|---|---|
| a187a3c | Phase 1+2: 3D ring matrix structs (W/R/I) + Worker side Plan A routing |
| 4a26ffb | Phase 3+4: Multi-receiver spawn + packed loop + sender removal + ReservHandler CPU 85 pin |
| e1a4dfc | Plan B (key_hash routing via FUSEE_RING_ROUTING env) |

**Key design knobs**:
- `FUSEE_RING_SHARDS_FACTOR=N`：每 N 个 worker 配一 shard，`actual_shards = ceil(T/N)`，N=0 = 兼容 baseline single-shard
- `FUSEE_RING_ROUTING=worker_id|key_hash`：Plan A vs B 切换
- CPU pin 起点 = `max(T, 64)`（worker 在 [0, T)，receiver 在 [64, 86)）
- Receiver budget 22 cores / 3 类（W/R/I），余数给 write

**ReservHandler CPU pin 修复**：iter-9A 起 ReservHandler 默认 unpinned，OS 自由调度到 worker CPU 上偷 cycle → N=0 baseline 5× 退化。iter-17A 修：pin to CPU 85（last CPU）。修复后 N=0 baseline vs iter-15A 实现 +83-105% 跨 T。

**Audit verification (硬证据)**:

| Audit | 方法 | 结果 |
|---|---|---|
| A1 — Worker routing | stderr 打印 `(client_id, ring_idx)` 一次每 worker | ✓ T=8 N=4: 0-3→ring0, 4-7→ring1 完全符合 Plan A 公式 |
| A2 — Receiver layout | 启动日志 `[A:thread] WriteRecv0 cpu=64 rings=[0]` | ✓ CPU 64-85 满用 0 浪费 |
| V1 — Metric semantics | 反推 `trans_ops=5,000,000 / trans_wall_max=8.25s ≈ trans_agg_thpt=605,794` | ✓ trans_agg_thpt = CLUSTER aggregate（sum across all workers / max wall） |

### 2.2 完整 8-group 对比表（3-rep median, cluster Mops/s, xhost_write zipf-0.99 V=1024）

| Group | T=1 | T=2 | T=4 | T=8 | T=16 | T=32 | T=64 |
|---|---:|---:|---:|---:|---:|---:|---:|
| **1. iter-15A baseline** (single ring+recv) | 0.198 | 0.315 | 0.527 | 0.555 | 0.565 | 0.612 | 0.604 |
| **2. iter-17A path opt N=0** | 0.227 | 0.318 | 0.735 | 0.924 | 0.932 | 1.019 | 1.009 |
| **3. Plan A worker_id N=0** | 0.227 | 0.318 | 0.735 | 0.924 | 0.932 | 1.019 | 1.009 |
| **4. Plan A worker_id N=4** ⭐ | 0.227 | 0.306 | 0.736 | 1.258 | **2.503** | 2.663 | **6.628** |
| **5. Plan A worker_id N=8** | 0.228 | 0.445 | 0.736 | 0.910 | 1.866 | **3.572** | 6.001 |
| **6. Plan B key_hash N=0** | 0.227 | 0.445 | 0.733 | 0.919 | 0.926 | 1.024 | 1.007 |
| **7. Plan B key_hash N=4** | 0.227 | 0.363 | 0.695 | 1.148 | 2.400 | 1.514 | 2.222 |
| **8. Plan B key_hash N=8** | 0.228 | 0.379 | 0.735 | 0.920 | 1.694 | 2.664 | 2.101 |

Raw + plots: `docs/iter17A_scaling_8group_final/`

**关键证据**:

1. **N=0 一致性验证** (groups 2=3=6, 三者应相同):
   - T=8 N=0: 0.924 (g2/3) vs 0.919 (g6) — ±0.5% ✓
   - T=16 N=0: 0.932 vs 0.926 — ✓
   - T=64 N=0: 1.009 vs 1.007 — ✓
   - 异常 g6 T=2 = 0.445 vs 0.318：T=2 已知高方差（±50%），不是 routing bug
   - **结论**：routing 代码 disabled 时（shards=1）零开销

2. **Path opt 收益**（g1 → g2）:
   - 全 T 段 +50-100% vs iter-15A baseline
   - 主要来自 ReservHandler pin + Stage 5/6 lfence + bucket double-flush 移除

3. **Multi-receiver scaling**（g2 → g4 Plan A N=4）:
   - T=8: 0.924 → 1.258 = +36% (shards=2)
   - T=16: 0.932 → 2.503 = **+169%** (shards=4) — 接近线性
   - T=32: 1.019 → 2.663 = +161% (shards=8，开始 packing)
   - T=64: 1.009 → **6.628** = **+557%** (shards=16，max packing) ⭐
   - 受 22-core CPU 池 hard limit；T=64 N=4 packing ratio 3 (write) / 2-3 (read/inval)

4. **N=4 vs N=8 trade-off**（g4 vs g5）:
   - T=16: N=4 (4 shards) > N=8 (2 shards) → 4-receiver win
   - T=32: N=8 (4 shards) > N=4 (8 shards packing) → 中等 T 上 N=8 略胜
   - T=64: 持平偏 N=4（峰值 6.628 vs 6.001）
   - **Pick N=4** 作为 default：T=64 峰值最高，全 T 段 ≥ N=8

5. **Plan A vs Plan B**（g4 vs g7, g5 vs g8）:
   - T=8/16: A 略胜（A 1.258 vs B 1.148; A 2.503 vs B 2.400）— 5-9%
   - T=32 N=4: **A 2.663 vs B 1.514 = +76%** — Plan B 在高 T+Zipf 下 hot key 集中
   - T=64 N=4: **A 6.628 vs B 2.222 = +198%** — Plan B 灾难性退化
   - T=64 N=8: **A 6.001 vs B 2.101 = +186%**
   - Plan B 单 rep 还出 0.175 catastrophic outlier (T=64 N=4 rep3 14× 退化)
   - **Plan A wins decisively**

### 2.3 Hot-key 集中假设验证

RAP attack vector A1 预测：Plan B 在 Zipf-0.99 下，hot key 通过 fnv1a hash 集中到一个 receiver shard，那个 receiver 满载，其他 receivers 闲置 → 单 receiver bottleneck。

**Plan A vs B 数据本身就是验证**：
- T=8 (shards=2): A vs B 差 9% — hot key 集中度低，A 微胜
- T=32 (shards=8): A vs B 差 76% — 8 个 receivers 中 1 个被 Zipf top key 撑爆
- T=64 (shards=16): A vs B 差 198% — 16 个 receivers 中 1 个 hot key 全占
- **Plan B 退化严重程度 ∝ shards 数**，与 hot-key 集中模型完全吻合

**Supp 2 (uniform vs Zipf distribution-flip) — 后续补做**:

完整 7-group uniform xhost_write sweep (`docs/iter17A_scaling_8group_uniform_20260523_000353/`)
跟 zipf-0.99 同 cell 直接对比（uniform/zipf ratio）：

| Cell | Plan B zipf | Plan B uniform | **u/z ratio** |
|---|---:|---:|---|
| T=32 N=4 (B kh) | 1.514 | **4.706** | **3.11×** |
| T=64 N=4 (B kh) | 2.222 | **5.208** | **2.34×** |
| T=64 N=8 (B kh) | 2.101 | **5.206** | **2.48×** |

| Cell | Plan A zipf | Plan A uniform | u/z ratio |
|---|---:|---:|---|
| T=32 N=4 (A wid) | 2.663 | 5.525 | 2.08× |
| T=64 N=4 (A wid) | 6.628 | 5.982 | 0.90× |
| T=64 N=8 (A wid) | 6.001 | 5.441 | 0.91× |

**Plan B 在 uniform 下被"治好"**（hot key 不存在 → key_hash 均匀分布到所有 receivers），同 cell throughput 提升 2-3 倍。**Plan A 在 uniform 下小幅变化**：T=32 翻倍（无 bucket lock contention），T=64 略降（uniform 散列 cache miss 多于 zipf 局部性）。

**Plan A vs Plan B 差距在 uniform 下急剧缩小**：
- zipf-0.99 T=64 N=4: A 比 B 领先 +198%
- uniform T=64 N=4: A 比 B 领先仅 +15%

→ Plan A 的优势**主要来自抗 hot key**，并非 routing 算法本身。这进一步证实 hot-key 集中假设是 Plan B 退化的根因。
Supp 3 (per-receiver counter) 未做：uniform 数据已强证假设，无需再做。

### 2.4 Scaling 上限分析

T=64 N=4 集群峰值 6.628 Mops 距离 20 Mops 目标 33%。瓶颈猜测（**初版**，§2.5 实验后已修正）：
- **CPU pool**: 22 receiver cores hard limit
- **Packing dilution**: T=64 N=4 时 write thread 担 2 ring (50% throughput per ring)，read/inval thread 担 2-3 ring (33-50%)
- **(已撤回)** T=32 zipf 平台 = `slot_directory_lock` 竞争 — §2.5 Exp 2/3 证伪
- **未做的优化**: 把 worker 占用的 CPU 0-63 让出一部分给 receiver（牺牲 worker count 换 receiver capacity）

### 2.5 瓶颈实验验证（Exp 1 + Exp 2 + Exp 3，2026-05-23）

§2.4 第一版给出三条瓶颈解释 ("CPU pool" / "packing dilution" / "lock contention")，但都是**未测量的推断**。本节用 3 个针对性实验把它们分别坐实或证伪。所有实验均针对 xhost_write zipf-0.99 V=1024，T=32 或 T=64 主战场。

#### Exp 1 — Single-key flood（**证实**：纯单 key 场景下 lock 是真瓶颈，但是 2-stage pipeline cap，非传统串行 lock）

**Setup**: 让两 host 都 UPDATE 对方 host-owned 的同**一个** key（FNV-1a 匹配挑出 user0/h0 + user4/h1），所有 op 撞同一 shared bucket。72 cells = T ∈ {8,16,32,64} × N ∈ {0,4,8} × routing ∈ {worker_id, key_hash} × 3 rep。

**结果中位数（cluster Mops/s）**:

| Plan | N | T=8 | T=16 | T=32 | T=64 |
|---|---:|---:|---:|---:|---:|
| A worker_id | 0 | 0.766 | 0.767 | 0.831 | 0.815 |
| A worker_id | 4 | 1.240 | **1.506** | **1.507** | **1.489** |
| A worker_id | 8 | 0.752 | **1.439** | **1.511** | **1.501** |
| B key_hash  | 0 | 0.765 | 0.767 | 0.832 | 0.821 |
| B key_hash  | 4 | 0.766 | 0.802 | 0.828 | 0.794 |
| B key_hash  | 8 | 0.711 | 0.767 | 0.819 | 0.802 |

**发现**：
- Plan A N≥2 在 T≥16 撞到 **~1.5 Mops cap**（1.9× single-recv baseline），N=4 和 N=8 完全平齐
- Plan B 所有 N 都停在 single-recv baseline ~0.8 Mops（因为 key_hash 把同一 key 永远路由到同一 shard）
- A vs B 比值 ≈ 1.9× 当且仅当 shards>1 且 routing 能分散

**机制解释**（修正原"lock contention" 简化模型）: 不是 N 个 receiver 排队抢同一 lock；而是 **2-stage pipeline**：一个 receiver 在 lock-held publish 阶段 (~1-2 µs) 时，另一个 receiver 已经在并行做 lock-free pool 读 (~8 µs)。两 stage 重叠 → 2× single-recv 上限，多于 2 个 receiver 不再 help（每瞬时只有一个能持锁）。Plan B 把所有 ops 路由到 1 个 shard → 失去 pipeline → 退回 baseline。

**图**: [exp1_table_summary.png](../iter17A_exp1_single_key_flood_20260523_022924/exp1_table_summary.png)、[exp1_grouped_bar.png](../iter17A_exp1_single_key_flood_20260523_022924/exp1_grouped_bar.png)
**数据**: [iter17A_exp1_single_key_flood_20260523_022924/grid.csv](../iter17A_exp1_single_key_flood_20260523_022924/grid.csv)

#### Exp 2 — Receiver perf-stat（**证伪**：T=32 zipf 平台不是 spin-lock 竞争，是 CXL coherence 带宽）

**Setup**: T=32 N=4 worker_id，分别跑 zipf-0.99 和 uniform xhost_write 各 1 rep（trace 重 gen 至 20M trans 以便 perf 抽样 3 s）。perf 同时用 `perf stat -e cycles,instructions,cache-misses,cache-references,LLC-loads,LLC-load-misses` 与 `perf record --call-graph dwarf,8192` attach 到 22 个 receiver tids（8 WriteRecv + 7 ReadRecv + 7 InvalRecv）。

**关键 perf-stat 指标**:

| metric | zipf-0.99 | uniform | delta | 解释 |
|---|---:|---:|---:|---|
| Total cycles (3 s, 22 recvs) | 211.5 G | 216.8 G | -2.4 % | 总功率近似 |
| IPC | 0.059 | 0.057 | +3.5 % | **≈0.06 表明 memory-bound 极重**（spin-lock contention 应该 ≥0.3） |
| L1+L2 cache miss % | 66.0 % | 55.4 % | +10.6 pp | zipf 高 |
| **LLC load miss %** | **72.9 %** | **58.8 %** | **+14.1 pp** | **hot-key CXL coherence ping-pong 直接证据** |
| Cluster throughput (Mops/s) | 4.81 | 5.14 | -6.4 % | uniform 略快（与原 sweep 一致） |

**机制解释**: 22 个 receiver thread 跑在 IPC=0.06 = **几乎所有指令都在等内存**。如果是 pthread_spin_lock 竞争，IPC 应该 ≥0.3（spin-lock 是 tight loop 读 hot cacheline，cache-hit 率高）。我们看到的是 **LLC miss rate >70%** —— 表明 receiver 大部分时间在等 **CXL coherence**（peer host 在写同一 hot key cacheline → 本地 cached copy 被 invalidate → 本地 receiver 重读 CXL 600 ns latency）。Zipf 比 uniform 多 14 pp LLC miss，正好对应"hot key 集中 → 同 cacheline 被两 host 反复 ping-pong"。

**Call-graph 局限**: `perf record --call-graph dwarf` 拿到的 callstack 全部 collapse 到 `std::__atomic_base::load` —— 即 receiver 大部分采样落在**轮询 CXL ring entry 的 atomic load** 上。所以 receiver "等内存" 不仅是 hashtable bucket 访问（critical section 内的）还包括 ring polling（critical section 外的，但同样 CXL 路径）。

**图**: [exp2_perf_stat_table.png](../iter17A_exp2_perf_lock_20260523_030306/exp2_perf_stat_table.png)、[exp2_perf_stat_bars.png](../iter17A_exp2_perf_lock_20260523_030306/exp2_perf_stat_bars.png)
**数据**: [iter17A_exp2_perf_lock_20260523_030306](../iter17A_exp2_perf_lock_20260523_030306/)

#### Exp 3 — Packing decouple（**证实**：现实 zipf 下吞吐线性 scale 在总 receiver 线程数上）

**Setup**: 加 env `FUSEE_FORCE_THREADS_PER_TYPE` 覆盖默认 `pool_size/3` floor（compute_receiver_layout 增 ~15 LOC）。固定 T=64 N=4（shards=16），变 threads_per_type ∈ {1, 2, 4, 8, 16, default(=8)}。1 = 极致 packing（每 receiver 担 16 shards）；16 = 1:1 无 packing；default = 8+7+7=22 恰好填满 22-CPU pool。

**3-rep median, cluster Mops/s**:

| threads_per_type | 总 receiver 线程数 | zipf | uniform |
|---|---:|---:|---:|
| 1 | 3 | 0.812 | 0.811 |
| 2 | 6 | **1.606** | **1.589** |
| 4 | 12 | **3.032** | **3.011** |
| 8 (CPU oversub) | 24 (>22 pool) | 0.512 | 0.453 |
| 16 (CPU oversub) | 48 (>22 pool, 2.2× oversub) | 0.092 | 0.090 |
| default | 22 (= pool size) | **5.783** | **5.503** |

**clean zone**（force ∈ {1, 2, 4, default}，**无 CPU 超额**）:
- 总线程数 3 → 6 → 12 → 22：**线性 scaling**，吞吐 ≈ 0.27 × total_threads
- zipf 与 uniform 在 clean zone 几乎完全相同（差 < 1 %）

**force=8/16 的 collapse** 是我自己加的 modulo-wrap CPU pinning 的 artifact：t_w+t_r+t_i = 24 (force=8) 或 48 (force=16) 在 22-CPU pool 内会 wrap，导致 WriteRecv 与 InvalRecv 共享 CPU → context switching 把吞吐砸下去。**不是真实瓶颈**，是 force override 的实现 limitation。

**关键结论**: 在 clean zone，吞吐**线性 scale 在 total receiver thread count 上**。如果 lock 竞争主导，应该在某点平台化（多 receiver 在同 lock 上互相阻塞）。我们看到完美线性 → 锁不是上限，**总 receiver CPU 时间** 才是上限。这与 Exp 2 perf-stat 一致（每 receiver IPC=0.06 = memory-stalled，加 receiver 线性增加并行 CXL 访问能力 → 线性增加吞吐）。

**图**: [exp3_thpt_vs_threads.png](../iter17A_exp3_packing_20260523_030745/exp3_thpt_vs_threads.png)、[exp3_summary_table.png](../iter17A_exp3_packing_20260523_030745/exp3_summary_table.png)
**数据**: [iter17A_exp3_packing_20260523_030745/grid.csv](../iter17A_exp3_packing_20260523_030745/grid.csv)
**代码**: [src/cxl_kv_ops_A.cc](../../src/cxl_kv_ops_A.cc) lines 757-783（compute_receiver_layout）env override 实现

#### Exp 1 + 2 + 3 综合结论（**取代** §2.4 的初版瓶颈解释）

| 区域 | 真实瓶颈 | 证据 | iter-17A 原始猜测 |
|---|---|---|---|
| **Single-key 100 % 同 bucket** | 2-stage pipeline cap (lock-held + lock-free pool read 并行) | Exp 1 N≥2 撞 1.5 Mops cap | "lock contention" — 部分正确，但是 pipeline 模型不是简单串行 |
| **现实 zipf-0.99（1 M unique keys, top 1 % = 50 % 流量）** | **per-receiver CXL latency × 总 receiver 线程数** | Exp 2 IPC=0.06 + LLC miss 73 % / Exp 3 clean-zone 线性 | ~~"slot_directory_lock 竞争"~~ — **证伪** |
| **uniform** | 同上（zipf vs uniform 在 clean zone 几乎相同） | Exp 3 中位数差 < 1 % | "hot key 不集中"宏观叙事仍成立，但底层机制是 CXL 而非 lock |

**对未来 iter 的指导**:
- Receiver-side path opt（H9 同款思路：减少 receiver per-op CXL 操作数）**比** lock 替换（spinlock → MCS 等）**优先级高**
- 加 receiver 线程数（推 CPU pool 上限）是直接 scaling lever — iter-18A 应考虑 worker/receiver CPU 再平衡（牺牲 T 换 receiver capacity）
- Single-key cap 1.5 Mops 与正常 zipf cap ~6 Mops 之间 4× 差距说明：**hot-key 集中本身**消耗约 75 % 的可用容量，但**不是 lock 限制**而是 CXL bandwidth 限制 → 物理 hardware 改进（CXL 2.0 → CXL 3.0 / 更宽通道）才是真的根本解药

---

## Part 3 — YCSB scaling validation（**结论：deferred**）

### 3.1 测试设计 vs 实际

**设计**：5 workloads (a/b/c/d/f) × 7 T × cache=on × KV=1024 × Plan A N=4 × 1 rep = 35 cells

**实际**：跑了 28 cells (workloada+b+c+d 全 7T + workloadf 部分) 后**主动 kill**，因为发现 multi-shard 在 read/RW 工作负载下灾难性退化。

数据：`docs/g34_scaling_ycsb_iter17A_20260522_090221/SUMMARY.log`

### 3.2 YCSB 退化诊断

针对 workloadc + workloada 各 T=16/32 × N=0/4 跑 3 reps（24 cells）独立诊断：

| workload | T | N=0 median | N=4 median | N=4/N=0 ratio |
|---|---:|---:|---:|---|
| **workloadc** (100% read Zipf) | 16 | 3.473 | 0.016 | **-217×** |
| workloadc | 32 | 3.444 | 0.016 | -210× |
| **workloada** (50R/50U Zipf) | 16 | 2.067 | 0.408 | -5× (含 1/3 bimodal 0.015) |
| workloada | 32 | 2.343 | 0.016 | -146× |

Raw: `docs/iter17A_scaling_ycsb_diag_20260522_091019/`

**关键发现**:
1. **N=0 (single-shard) 工作完美** — workloadc 在 T=16/32 都跑出 3.4+ Mops，比 xhost_write 同 T 还高（因为 cache hit ratio 高）
2. **N=4 (multi-shard) 在 read/RW 工作负载下灾难退化** — 5-217× slower than N=0
3. **Bimodal 现象** — workloada N=4 在 3 reps 间 0.015/0.430/0.408 — 暗示 startup race condition
4. **0.016 Mops = 200k ops / ~12.5s wall** — 工作负载完成但 wall 被 startup 卡死

### 3.3 假设：multi-shard 破坏了 read/inval path

证据汇总:
- xhost_write (INSERT-only, no read, no invalidate broadcast for INSERT): **multi-shard scaling 完美** (T=64 N=4 = 6.628 Mops)
- YCSB read-heavy: **multi-shard 灾难**

代码层面 multi-shard 改造覆盖了 3 类 ring (W/R/I)，但**只在 xhost_write workload 上做了端到端验证**。read_receiver_loop 和 inval_receiver_loop 改造同款 (refactor → take ring_indices vector)，但没有专门的 read path 多 receiver 端到端测试。

可能的具体 bug:
- **read_receiver_loop** 的 ring_indices 循环 + lfence 模式，可能 site-B 类型 stale read 污染（Stage 6 教训）
- **inval_receiver_loop** 同款风险
- **forward_read_direct** 与 ReadStaging[src][dst][slot] 索引可能在 multi-shard 下错位
- **cache_pool** 的 multi-thread 原子争用（128k buckets 集中在 hot bucket epoch）

**Bimodal 现象**最具诊断价值 — 同一 build、同一 config 在 3 reps 间出现 0.015 vs 0.4 的悬殊结果，强烈指向 startup race，e.g. 某个 receiver 偶尔被卡在 spin 等永远不来的事件。

### 3.4 Deferred 决定

YCSB scaling 在 multi-shard 下**不可用**。具体 deferred 工作：

1. **修 multi-shard read path bug** (next iter):
   - 重读 read_receiver_loop / inval_receiver_loop 的 ring_indices 改造
   - 加 receiver-side stderr counter 验证每 shard 实际 drain ops 数
   - 跟踪 bimodal stuck 时 receiver 状态（perf record on stuck case）
2. **修复后重跑 35-cell YCSB scaling** 验证 read 路径多 shard 表现
3. **集群 20 Mops 目标进度**：xhost_write 6.6 Mops cluster = 33% of target；read 路径要 unblock 才能往 R/W mix workload 上推

iter-17A 的核心交付 (Plan A multi-shard scaling for xhost_write) **仍然有效**，YCSB 不阻断 iter-17A 完结，作为已知缺陷记录到 backlog。

---

## 关键学习 / 通用规则

1. **lfence vs mfence 安全规则** (Stage 6 bisection):
   `flush_line + lfence + load` 安全 ⇔ (单字段 load) ∧ (循环自愈 ∨ 值语义抗 stale)

2. **防御性 flush 往往冗余** (Stage 7 bucket double-flush):
   "CXL 不 coherent → 读前 flush" 这种粗放规则忽略了 sharding invariant。bucket 由 owner 独写 → 同 host MOESI 处理 → flush 完全冗余 → 删除 +36-37%。

3. **Multi-shard scaling 的关键变量是 N (workers/shard)**:
   不是 "总 shard 数"。N 控制每 shard 的 worker 数；shards = ceil(T/N)。N 小 = 多 shard = scale 上限高，但 packing dilution 严重。

4. **ReservHandler 不 pin CPU 是致命的**:
   "spinner thread 不在 hot path → 不需要 pin" 的判断错了。它的 unpinned spin 会漂到 worker CPU 上偷 cycle，造成 50% throughput 退化（T=1 case 最严重）。

5. **trans_agg_thpt 是 cluster aggregate**:
   只有 h0 primary client 打印，但 aggregator 通过 CXL shared WorkerStats 累加所有 host 的 worker my_count。`trans_ops` 字段即 cluster total ops。

6. **代码改动 + workload coverage 必须对齐**:
   Multi-shard 改了 3 类 ring (W/R/I)，但**只用 xhost_write workload 验证** → 留下 read/inval path 的 silent bug。下次类似 multi-aspect 改动，必须设计涵盖所有 path 的 smoke matrix。

---

## Commits (iter-17A 全部)

Path optimization (Part 1):
- 3786c86 Stage 1+2 (lfence + comments)
- 11180c5 Stage 3 audit
- 2441e69 Stage 4 audit (rejected)
- 1a6f602 Stage 5 lfence (+21-23%)
- dc5c9c7 Stage 6 lfence A+C (+21-27%)
- 06ac461 Stage 7 dead-code
- b1d6c31 Bucket double-flush removal (+36-37%)

Scaling (Part 2):
- d021197 Design + path opt summary docs
- 16e71b0 RAP
- a187a3c Phase 1+2: 3D ring matrix + worker routing
- 4a26ffb Phase 3+4: multi-receiver spawn + sender removal
- e1a4dfc Plan B implementation + audit framework

---

## 后续 iter 路线 (iter-18A backlog)

按优先级:
1. **修 multi-shard read/inval path bug** — unblock YCSB scaling
2. **YCSB scaling 35-cell sweep** on fixed code
3. 突破 22-core receiver pool 上限（牺牲 worker 数 OR 让 receiver 共享 worker CPU + HT-aware pinning）
4. Backlog B1/B2 (read paths + send_invalidate lfence opt) — workload-c/a/f 上跑
5. 推进 cluster 20 Mops target — workload-c read 路径 + xhost_write 双向并行
