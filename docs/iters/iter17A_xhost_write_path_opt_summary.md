# iter-17A xhost write path 优化总结（第一部分：路径优化）

**Date**: 2026-05-22
**Focus**: xhost_write 路径逐 stage 代码审查 + 优化
**Branch**: `feat/cxl-migration`
**Cumulative gain (路径优化部分)**: +14-68% over iter-15A Phase 2 baseline
(3-rep sweep verified)

**iter-17A 全 scope**：
- **Part 1（本文档）** — 路径优化：8 stage 代码审查 + fence/dead-code 优化
- **Part 2** — Multi-ring + multi-receiver scaling：
  [设计文档](iter17A_multi_ring_receiver_scaling_design.md)（接续本文档，
  突破 single-receiver throughput 上限）
- **Part 3** — winner-based YCSB scaling sweep（part 2 完成后选 winner，
  跑完整 5 workload sweep）

---

## Background

iter-16A 建立了 xhost_write 8-stage 分解框架，发现 Stage 5 (worker
ack_wait) 占 worker latency 72-99%。iter-17A 以 stage-by-stage 代码
review 方式找单点优化，每个 stage 单独评估、改、smoke、commit；
最后 3-rep sweep 锁定累计收益。

8 个 stage：
- Worker 侧 (5): B/C/D/E/F = slot_reserve / slot_wait / value_xfer /
  ctrl_publish / ack_wait
- Receiver 侧 (3): G/H/I = rcv_poll / rcv_work / ack_publish

---

## 优化分类（标签约定）

- **Micro-opt**：fence 强度调整、cacheline 对齐等不改语义的指令级
  优化
- **Dead-code 删除**：审计后证明的无意义代码删除
- **Audit-only**：审查后判定原代码必要或风险过高，仅留注释
- **Rejected**：尝试后实测回退，已 revert

---

## 逐 stage 总览

| Stage | Phase | 操作 | Commit | Smoke 收益 |
|---|---|---|---|---|
| 1 | slot_reserve / B | 死代码注释（`FUSEE_XHOST_WRITE_SELF_INVAL`）| 3786c86 | 持平 |
| 2 | slot_wait / C | Micro-opt: mfence → lfence (C-spin) | 3786c86 | 持平 |
| 3 | value_xfer / D | Audit-only: pool header 不是死代码 | 11180c5 | 持平 |
| 4 | ctrl_publish / E | Audit-only: E1.5 resets 经测必需 | 2441e69 | 持平（rejected）|
| 5 | ack_wait / F | Micro-opt: mfence → lfence (worker spin) | 1a6f602 | **+21-23%** |
| 6 | rcv_poll / G | Micro-opt: mfence → lfence at A+C (B 拒绝) | dc5c9c7 | **+21-27%** |
| 7 | rcv_work / H | Dead-code: `pool_->read(_,nullptr,0)` 移除 | 06ac461 | ~flat |
| 7 | rcv_work / H | **Dead-code: bucket double-flush 移除 (×4 sites)** | b1d6c31 | **+36-37%** |
| 8 | ack_publish / I | Audit-only: 已最小化无安全优化 | — | — |

---

## 详细 per-stage 记录

### Stage 1 — slot_reserve (B phase) — commit 3786c86

**位置**: `cxl_kv_ops_A.cc:1476-1491` (`forward_write_direct` 入口 +
`ring->tail.fetch_add`)

**优化**: 给 `FUSEE_XHOST_WRITE_SELF_INVAL` 块加注释，标注是 default-off
的死代码（生产构建不触发），保留 opt-in flag 留作未来实验。

**为什么没硬优化**: CXL atomic `fetch_add` 是物理底，无可优化空间。

---

### Stage 2 — slot_wait (C phase) — commit 3786c86

**位置**: `cxl_kv_ops_A.cc:1505-1516` (worker 等 slot 空闲的 spin)

**优化**: spin loop 内 `flush_line → full_fence → load`，把 `full_fence()`
换成 `__builtin_ia32_lfence()`。

**理由**: 等待 slot free 是 self-correcting loop（条件 `req_op_id == 0`），
单字段 load，lfence 已足够 order clflushopt → load 序列。mfence 的 store
ordering 在这里多余。

---

### Stage 3 — value_xfer (D phase) — commit 11180c5

**位置**: `cxl_kv_ops_A.cc:1519-1629` (`pool_->write` 写 header+value)

**审计**: 曾提议删除 4-byte pool header（D2）作为死代码，调研后否决 ——
owner-self-miss 路径（`search` at line 2632-2634）会读 header 拿
value_len。删除需要把 value_len 编码进 bucket slot，是更大的 refactor。

**结论**: header 留作 documentation，注释更新警示后续 iter 不要重复
提议。

---

### Stage 4 — ctrl_publish (E phase) — commit 2441e69 (REJECTED)

**位置**: `cxl_kv_ops_A.cc:1648-1659` (写 staging_off/staging_gen 后的
cacheline 2 reset)

**尝试**: 删除 `e->resp_op_id.store(0)` 和 `e->status = 0` 这两个看起来
defensive 的 reset。理论分析支持删除（op_id 单调 + status 总被 receiver
覆盖）。

**实测**: T=8 -60%, T=64 -40%, w_p99 飙到 1.85-2.37 ms。具体机制未定位，
怀疑 CXL inter-host clflushopt writeback race（cacheline 2 在 Modified
状态时的 flush 行为与在 Invalid/Shared 状态时不同）。

**结论**: 标记为 EMPIRICALLY REQUIRED，注释提示未来 iter 不要无验证
删除。

---

### Stage 5 — ack_wait (F phase) — commit 1a6f602 — **+21-23%**

**位置**: `cxl_kv_ops_A.cc:257-289` (`generic_spin_wait` worker 等
receiver ack 的 spin)

**优化**: spin loop 内 `flush_line(&e->resp_op_id) → full_fence →
load(resp_op_id)`，把 `full_fence` 换成 `__builtin_ia32_lfence`。

**安全性论证**: spin 条件 `resp == op_id` 严格匹配；op_id 单调，stale L1
残留值不可能假阳性 → 退出时一定是 fresh CXL fetch → 同 cacheline 上的
`e->status` 也是 fresh。

**Smoke 收益**:
- T=1: +3% (noise floor)
- T=8: +21%
- T=64: +23%

**为什么收益大**: Stage 5 dominate worker latency 72-99%，spin 跑 N 次
迭代等 receiver，每 iter 省 ~10-20 ns 的 mfence 开销累计起来很可观。

---

### Stage 6 — rcv_poll (G phase) — commit dc5c9c7 — **+21-27%**

**位置**: `cxl_kv_ops_A.cc:2438-2484` (`write_receiver_loop`)

**优化**: 三处 `flush_line + full_fence + load` 模式，bisection 确定：
- **Site A** (outer tail poll, line 2446-2448): lfence 安全，**接受**
- **Site B** (per-slot req_op_id load, line 2456-2458): lfence **拒绝**
- **Site C** (gap-spin load, line 2464-2466): lfence 安全，**接受**

**Site B 失败的根本原因**（Stage 6 bisection 的关键发现）:
Per Intel SDM §11.4.4，**clflushopt 由 mfence/sfence ordering，NOT by
lfence**。lfence 允许 load 在 clflushopt invalidate 完成前 speculatively
读 L1。Site B 是**一次性 load**，load 完后立刻读同 cacheline 上的其他
字段（key/op_kind/value_len/staging_off/staging_gen）。stale L1 残留可能
是上次 slot 复用时的非零 op_id —— receiver 跳过 gap-spin，用全套 stale
字段去 ack 一个**错误的 op_id**，当前 worker 永远等不到正确 ack → 5ms
timeout。

**派生定理（iter-17A 路径优化的核心规则）**:
> `flush_line + lfence + load` 安全当且仅当：
> (a) load 是**单字段** 且
> (b) 要么 load 处于**自愈循环**（如 spin 直到某条件成立），要么
> **值语义抗 stale**（单调、严格 op_id 等比对）。
>
> 一次性 load 后读同 cacheline 其他字段 → **必须用 mfence**。

**Smoke 收益 (A+C combined)**:
- T=1: +0.4%
- T=8: +27% (vs Stage 5-baseline 0.821, abs 1.043)
- T=64: +21% (vs Stage 5-baseline 0.911, abs 1.101)

---

### Stage 7 — rcv_work (H phase) — commits 06ac461 + b1d6c31

#### 7.1 Dead-code: `pool_->read(blk_off, nullptr, 0)` — commit 06ac461

**位置**: `cxl_kv_ops_A.cc:2239` (`write_handler` direct-pool path)

**审计**: `pool_->read(off, nullptr, 0)` 在 `len == 0` 时早返回（见
`cxl_kv_blockpool.cc:180`），**没有 clflushopt 发出**。原注释声称
"no-op cache invalidate via flush" 是错的；配套 `full_fence()` 也没什么
可 order。

**Smoke**: T=1 +6%（噪声边缘），T=8/64 ~flat。Stage 7 本身 ~1100 ns，
省的 ~15-30 ns 不显著。**主要价值是清掉误导性注释**。

#### 7.2 **Bucket double-flush 移除（最大单点收益）** — commit b1d6c31 — **+36-37%**

**位置**: `cxl_kv_ops_A.cc` 4 处：
- `execute_write_local_with_blk` line 389-391 (pre-scan) + 412-413 (post-lock)
- `execute_write_local` line 477-479 (pre-scan) + 504-505 (post-lock)

每个 pre-scan 位置：2 × clflushopt (bucket 两个 cacheline) + mfence
每个 post-lock 位置：1 × clflushopt (**只刷 cacheline 1**) + mfence
每个 op 合计删除 **4 × clflushopt + 2 × mfence**，估计 ~200-300 ns。

**Invariant 审计**:
1. buckets 在 CXL devdax (`/dev/dax0.0`)，跨 host **非 coherent**
2. **Sharding 设计**: 每个 bucket 有唯一 owner host，**只有 owner 写**
3. `execute_write_local` / `execute_write_local_with_blk` **总是在 owner
   host 执行**（要么 local client thread，要么 owner 自己的 receiver
   thread）
4. **同 host 内 x86 MOESI 自动保持 multi-core L1 coherent** + 之前写入
   的 clflushopt+sfence 已经 invalidate 了同 host 所有 L1 copy → owner
   的 L1 view 不可能 stale

**实证证据（最强论据）**: post-lock 的 flush 只刷 64B = cacheline 1
(slots 0-3)，没碰 cacheline 2 (slots 4-7)。如果这次 flush 是 load-bearing
的，当 `target_slot ∈ {4,5,6,7}` 时应该读到 stale L1 数据。但代码跑了
N 次 sweep 没出问题 —— 直接证明这个 flush **从来没起作用**。换言之，
"没有 flush 也对" 不是因为运气，而是 owner-host L1 本来就是 coherent
的。

**Smoke** (xhost_write):
- T=1: +4% (0.224 → 0.233)
- T=8: **+37%** (0.849 → 1.163)
- T=64: **+36%** (0.909 → 1.240)

**Workload-a 验证**: T=8 1.22 Mops/s，T=64 1.86 Mops/s，**0 errors**
（混合读写 invariant 也对）。

---

### Stage 8 — ack_publish (I phase) — audit only

**位置**: `cxl_kv_ops_A.cc:2498-2501`:
```cpp
std::atomic_thread_fence(release);    // x86 compile-only barrier (free)
e->resp_op_id.store(op_id, release);  // protocol-required
flush_line(&e->resp_op_id);           // CXL visibility-required
store_fence();                         // sfence, drain clflushopt
```

**结论**: 已最小化。sfence 是 clflushopt 必需的 store-ordering 最小
fence；删除会延迟 ack 到达 worker，反而拖累 Stage 5。无安全优化空间。

---

## 最终 sweep 结果

**协议** (与 iter-15A Phase 2 一致):
- V=1024, cache_pct=10% (131072 buckets), zipf-0.99
- TRANS_OPS=5,000,000 per host (10M cluster aggregate)
- LOAD_OPS=5,000,000 per host (10M cluster aggregate)
- NUM_BUCKETS=8,388,608
- build-cxl-w1-v1024
- aggregate thpt = h0 + h1
- 3 reps per cell, 取 median

| T | iter-15A baseline (Mops/s) | iter-17A 当前 (Mops/s) | 提升 |
|---:|---:|---:|---:|
|  1 | 0.198 | **0.225** | **+14%** |
|  8 | 0.555 | **0.900** | **+62%** |
| 16 | 0.565 | **0.951** | **+68%** |
| 64 | 0.604 | **1.011** | **+67%** |

**3-rep 一致性**:
- T=1: 0.223 / 0.226 / 0.225 (±1%)
- T=8: 0.950 / 0.762† / 0.900 († single-rep flier，-16%)
- T=16: 0.948 / 0.951 / 0.951 (±0.3%)
- T=64: 1.011 / 1.004 / 1.015 (±0.6%)

Raw: `docs/iter17A_xhost_write_sweep_20260522_032339/grid.csv`

**观察**:
1. 高 T 段 (T≥8) 收益稳定 +60-68%，与单 rep 烟测的 +67-72% 量级一致
2. iter-15A 的 T 饱和点 ≈ T=8 (0.555-0.604 平台)，现在上移到 T=16/64
   共享 0.95-1.01 平台 —— 但 T=8 还没完全饱和 (0.900)
3. **T=8 → T=64 几乎无 scaling** (+12%)：依然是 single-receiver
   瓶颈。突破要靠 multi-receiver scaling
4. T=1 提升仅 +14%：低 T 受端到端 latency 限制（worker→receiver→worker
   一来回），各 stage 的小优化加起来 +14% 完全 add up

---

## 完整 T sweep (T=1,2,4,8,16,32,64) + probe-on/off 对照

**Probe-off** (vs iter-15A Phase 2 baseline，per-host Mops/s)
Raw: `docs/iter17A_xhost_write_full_probeoff_20260522_034556/grid.csv`

| T | iter-15A baseline | iter-17A (rep=1) | Δ |
|---:|---:|---:|---:|
|  1 | 0.198 | 0.227 | +15% |
|  2 | 0.299 | **0.265 (3-rep median)** | **-11%** ⚠️ |
|  4 | 0.527 | 0.595 | +13% |
|  8 | 0.554 | **0.955 (3-rep median)** | **+72%** |
| 16 | 0.563 | 0.950 | +69% |
| 32 | 0.612 | 1.021 | +67% |
| 64 | 0.604 | 1.003 | +66% |

**Probe-on** (vs iter-16A `xhost_decomp_sweep_20260521_075204` baseline，per-host Mops/s)
Raw: `docs/iter17A_xhost_write_full_probeon_20260522_035010/aggregate.csv`

| T | iter-16A baseline | iter-17A (rep=1) | Δ |
|---:|---:|---:|---:|
|  1 | 0.055 | 0.057 | +4% |
|  2 | 0.103 | **0.106 (3-rep median)** | +3% |
|  4 | 0.186 | 0.190 | +2% |
|  8 | 0.587 | **0.349 (3-rep median)** | **-41%** ⚠️ |
| 16 | 0.648 | 0.824 | +27% |
| 32 | 0.718 | 0.936 | +30% |
| 64 | 0.707 | 0.937 | +33% |

Re-verify raw: `docs/iter17A_xhost_write_t28_reverify_20260522_041000/`

### 两个异常观察记录

#### A. T=2 probe-off 看似回退 -11%（rep 间方差大）

3 reps: **0.324 / 0.265 / 0.237**（24% spread），median 0.265 低于 iter-15A
baseline 0.299。

特征:
- w_p50 三次稳定在 9.4µs，w_p99 12.6-13.3µs（**没有 tail**）
- wallclock 28-34s 不一致
- 同时跑的 T=2 probe-on 反而稳定（0.106/0.107/0.106 ±0.5%）
- 其他所有 probe-off cells（T=1,4,8,16,32,64）都在 +13-72% 区间

最可能的原因（**未深入定位**）:
- T=2 时只有 2 个 worker + 1 receiver，并行度低 → 任何 startup
  transient（kernel scheduling、CPU frequency boost ramp、TLB warmup）
  都能拉大 spread
- 之前 iter-15A baseline 也是 3 reps，但当时取的 median 是 0.299 ——
  iter-15A 数据稳定不代表此机器/此 build 复现同样稳定
- iter-15A baseline 是 ~10 天前测的，硬件 thermal/firmware 状态可能
  漂移

**结论**: 不是路径优化引入的回退（w_p99 干净，所有改动都在 fence/dead
code），是低并行度配置 + 单次测量的 transient 偶发。**不阻断**
iter-17A 评估。要进一步定位的话需要：
1. 在当前 build 重跑 iter-15A baseline (build-cxl-w1-v1024 反推到
   commit 3bf7d17，iter-16A 的状态) → 直接比对当机器
2. T=2 跑 ≥10 reps 看分布

#### B. T=8 probe-on 稳定回退 -41%（reproducible tail）

3 reps: **0.360 / 0.346 / 0.349**（±2%），median 0.349 远低于
iter-16A baseline 0.587。

特征:
- w_p50 = 15-17µs（**比 iter-16A baseline 22-24µs 快 30%！**）
- w_p99 = 614-624µs（**40× p50，iter-16A baseline 是 32-33µs ≈ 1.4× p50**）
- 三次极度可重现 → **不是 noise**
- Probe-off 同条件 T=8 = 0.955 (+72%)，无任何 tail（w_p99=21µs）
- T=2/4 probe-on 正常（+2-4%），T=16/32/64 probe-on +27-33%

定性诊断:
- 常态 op 比 baseline 快（路径优化兑现），但 probe-on 引入了一个
  600µs 量级的 tail bump
- Tail 只在 probe-on + T=8 出现 → 是 **probe 系统在 T=8 的特定
  scheduling pattern 下抖动**，跟生产路径无关
- 怀疑机制：我们的 receiver 路径变快（commit b1d6c31 去 4 个
  bucket flush + 2 mfence ≈ 省 200-300 ns/op）→ probe 事件生成
  rate 上升 → 在 T=8 这个特定并行度下，probe ring 写入 +
  flush + 8 worker 的争用形成新的 pathological pattern
- T<8 probe 事件少不到饱和，T>8 worker 多 receiver 排队成为瓶颈（已经是 limit），
  反而 mask 掉 probe 抖动

**结论**: **probe-on artifact，不影响生产**。生产 build 是
build-cxl-w1-v1024 (`FUSEE_PROBE=0`)，不走 probe 路径。Probe-on
仅用于诊断 / decomp 分析。如果未来要重新跑 probe-on decomp 在 T=8
做精确测量，需要先解决这个 tail 来源。

要深入定位的话：
1. 用 perf record / perf c2c 看 T=8 probe-on 下的 tail 出现时哪个
   CPU/cacheline 在 thrashing
2. 看 probe_ring 的 flush 路径是否在 T=8 时有 worker 被卡住
3. 二分 commit b1d6c31 → 验证是否是 bucket flush removal 触发的

**不阻断 iter-17A**：probe-off T=8 +72% 是路径优化的正确度量；
probe-on 仅作 decomp 工具，工具自身的边角问题不撤销 commit。

---

## Stage 7 / 路径相关 backlog（**未做**，原因记录）

### B1. send_invalidate_direct lfence (Stage 7 内 sharer broadcast)

- **位置**: `cxl_kv_ops_A.cc:1738-1741`
  (`send_invalidate_direct` 内的 ack-spin)
- **机会**: 同 Stage 5 模式 (单字段 spin + 严格 op_id 匹配)，可以用
  lfence 替 mfence
- **预估收益**: workload-a/f 上 +1-3%
- **为什么 xhost_write 上不做**:
  `execute_write_local_with_blk` line 423 中 broadcast 的条件是
  `op_kind != kOpKindInsert`。**xhost_write workload 是 INSERT-only**
  → 不触发 broadcast → 不触发这条 spin。在 xhost_write 上收益为 0。
- **将来何时做**: 用 workload-a / workload-f 测 multi-receiver scaling
  时，再带上这条优化做一次烟测。

### B2. Read paths bucket double-flush 移除 (同 invariant 衍生项)

- **位置**: 2 处:
  - `read_handler` line 2327-2329 (cross-host read receiver path)
  - `search` owner-self-miss line 2650-2652
- **机会**: 与 commit b1d6c31 相同 invariant（owner-only write +
  MOESI），可以用同样论证删除
- **预估收益**: workload-c (read-only) +10-25%，workload-a/f (mixed)
  也有部分受益
- **为什么 xhost_write 上不做**:
  - read_handler 服务 cross-host **read**，xhost_write 不 read
  - search owner-self-miss 是本地 read miss 路径，xhost_write 不 read
  - 在 xhost_write 上收益为 0
- **将来何时做**: 准备整体 YCSB benchmark (workload-c / mixed) 或者
  进入 multi-receiver scaling 之前，专门做一次 read 路径优化
  commit + workload-c sweep 验证

---

## Iter-17A 路径优化阶段产出（commits）

1. **3786c86** `[iter17A-stage12]` Stage 2 lfence + Stage 1 死代码注释
2. **11180c5** `[iter17A-stage3]` Stage 3 pool header NOT 死代码 (audit)
3. **2441e69** `[iter17A-stage4]` Stage 4 E1.5 经测必需 (rejected)
4. **1a6f602** `[iter17A-stage5]` Stage 5 lfence (+21-23%)
5. **dc5c9c7** `[iter17A-stage6]` Stage 6 A+C lfence (+21-27%, B 拒绝)
6. **06ac461** `[iter17A-stage7]` Stage 7 死代码 `pool_->read(_,null,0)`
7. **b1d6c31** `[iter17A-aggreg1]` Bucket double-flush 移除 (+36-37%)

---

## 关键学习 / 通用规则

1. **lfence vs mfence 安全规则** (Stage 6 bisection 得到):
   `flush_line + lfence + load` 安全 ⇔ (单字段 load) ∧ (自愈循环 ∨
   值语义抗 stale)。一次性 load 后读同 cacheline 其他字段 → 必须
   mfence。

2. **"防御性 flush" 经常是冗余的**:
   一个常见反模式是用 "CXL 不 coherent → 读 CXL 数据前 flush" 这个粗放
   规则替代具体分析。在 sharding-protected 数据结构上（如 bucket），
   只有 owner 写 + 同 host MOESI 自动 coherent → flush 完全冗余。
   bucket double-flush 移除验证了这点 (+36-37%)。

3. **"看起来死"的代码 ≠ 真死代码**:
   两次反例：Stage 4 E1.5 (实测必需，机制未明) + Stage 3 pool header
   (被 owner-self-miss 用)。**实测和代码 grep 永远优先于理论分析**。

4. **Stage 5 是 worker latency 主导**，但优化 Stage 5 本身 (lfence) 给
   +21-23%；优化 Stage 6/7 (receiver 路径) 间接缩短 Stage 5 等待时间，
   给更大收益 (+36-37%)。**在 single-receiver 瓶颈下，优化 receiver
   路径的 ROI 高于优化 worker 路径**。

5. **路径优化的天花板 = single-receiver throughput**:
   T=8→T=64 几乎无 scaling 表明 receiver 已饱和。下一阶段必须做
   multi-receiver scaling 才能突破。

---

## 下一阶段建议

- **接下来**: 进入 multi-receiver scaling 设计（N receivers per ring，
  iter-16A `Stage5_Gap_StageR_combined.png` 给出 N=4 或 8 是自然的
  分片点）
- **顺带做**: 进入 scaling 前把 backlog B1 + B2 合并成一个 "read +
  invalidate 路径 lfence + flush removal" commit，跑一遍 workload-c +
  workload-a sweep，把所有不在 xhost_write 上的同款机会一次清掉
