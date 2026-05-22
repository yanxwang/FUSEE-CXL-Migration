# iter-17A: Multi-Ring + Multi-Receiver Scaling 设计

**Date drafted**: 2026-05-22
**Branch**: `feat/cxl-migration`
**Iter 归属**: iter-17A 第二部分（接续路径优化）
**前序**: [iter17A 路径优化总结](iter17A_xhost_write_path_opt_summary.md)
（commits 3786c86 → b1d6c31，xhost_write T=64 +66% over iter-15A）
**Goal**: 突破 single-receiver throughput ceiling（当前 ~1 Mops/s/host），
向 20 Mops/s 设计目标迈进

---

## 1. Background

iter-17A 路径优化把 xhost_write T=64 单 host throughput 从 0.604
Mops/s 抬到 1.003 Mops/s（+66%），但 **T=8→T=64 几乎无 scaling
(+12%)**，明确表明 single-receiver 是新的瓶颈。

iter-16A `Stage5_Gap_StageR_combined` 分析给出 N=4 或 N=8 是 receiver
shard 的自然分片点 —— 单 receiver 处理 ~16 个 worker 时进入饱和，
更多 worker 排队不能换来 throughput 提升。

本 iter 把 W/R/I 三类 ring + 对应 receiver 改成多 shard 架构。

---

## 2. 设计要点速览

| 决定 | 值 | 备注 |
|---|---|---|
| 第一版策略 | **Plan A**: `ring_idx = client_id / block_size` | 静态分片，最简实现 |
| 后续策略 | **Plan B**: `ring_idx = key_hash % shards` | A 完成测试后单独实现单独测 |
| 三类 ring scale | W/R/I **全部** 同一套 N 机制 | 一个 env var 控制三个 |
| N 语义 | **每 N 个 worker 配 1 个 ring+receiver shard** | N=4 或 N=8 测试 |
| N 来源 | 启动 env var `FUSEE_RING_SHARDS_FACTOR=N` | 默认 1 = 兼容当前 |
| Actual shards 公式 | `actual_shards = ceil(T/N)`，下界 1 | 见 §4 表 |
| Worker→ring 映射 | `block_size = ceil(T/actual_shards)` `ring_idx = client_id / block_size` | 均分压力，T 不整除 N 时也均衡 |
| Receiver 线程分布 | 3 类 receiver 平分 22 CPU，**余数给 write** | `W_threads = floor(22/3)+(22%3)` |
| CPU pin 起点 | **`start_cpu = T`**（紧跟 worker） | 顺序 W → R → I；动态自检 nproc |
| CPU 紧时的处理 | **软件 packing (方案 Z)**：1 线程轮询多 ring，无 OS context switch | 绝不依赖 OS time-slice |
| Sender 线程 | **全部移除**：从启动路径 + pinning 中摘掉 | iter-9A scaffold 实际未在 hot path |

---

## 3. Ring matrix 数据结构改造

### 3.1 当前布局

```cpp
// cxl_write_ring.h
struct WriteRingMatrix {
  WriteRing rings[kWriteMaxHosts][kWriteMaxHosts];   // 4×4 = 16 rings
};
```

### 3.2 新布局

```cpp
constexpr int kRingShardsMax = 16;   // 上限：对应 T=64/N=4 = 16 shards

struct WriteRingMatrix {
  WriteRing rings[kWriteMaxHosts][kWriteMaxHosts][kRingShardsMax];
};
```

ReadRingMatrix / InvalRingMatrix 同款扩成 3D。

### 3.3 内存预算

- WriteRing ≈ 32 KB (256 entries × 128 B + headers)
- WriteRingMatrix: 4 × 4 × 16 × 32 KB ≈ **8 MB**
- ReadRing 接近，~8 MB
- InvalRing 较小，~2 MB
- 三类合计 ~18 MB CXL devdax → 容量充足（设备 512 GiB）

实际只用 `actual_shards` 个 shard，剩余 `kRingShardsMax - actual_shards`
保留不动（init 期间一次 memset 即可）。

---

## 4. Actual shards 公式 + 实例

```c
int actual_shards = (T + N - 1) / N;     // ceil(T/N)
if (actual_shards < 1) actual_shards = 1;
```

| T | N=4 → shards | N=8 → shards |
|---:|---:|---:|
| 1 | 1 | 1 |
| 2 | 1 | 1 |
| 4 | 1 | 1 |
| 6 | 2 | 1 |
| 8 | 2 | 1 |
| 12 | 3 | 2 |
| 16 | 4 | 2 |
| 32 | 8 | 4 |
| 64 | 16 | 8 |

边界处理：
- T<N (e.g., T=2, N=4)：actual_shards=1，所有 worker 共享一个 ring →
  等价 baseline
- T 偶数非 2^n（e.g., T=6, N=4）：shards=2，6 worker 3+3 均分到 2 ring
- T 奇数：**不在 benchmark 范围**（per 用户要求）

---

## 5. Worker → ring 映射（Plan A）

### 5.1 算法

```c
int block_size = (T + actual_shards - 1) / actual_shards;  // ceil(T/shards)
int ring_idx = client_id / block_size;
WriteRing *ring = &wr_->rings[host_id_][owner][ring_idx];
```

### 5.2 验证均分（关键 case）

| T | actual_shards | block_size | client_id → ring_idx |
|---|---|---|---|
| 6, N=4 | 2 | 3 | 0,1,2 → ring 0; 3,4,5 → ring 1 |
| 8, N=4 | 2 | 4 | 0-3 → ring 0; 4-7 → ring 1 |
| 12, N=4 | 3 | 4 | 0-3 → ring 0; 4-7 → 1; 8-11 → 2 |
| 64, N=4 | 16 | 4 | 0-3 → ring 0; 4-7 → 1; ...; 60-63 → 15 |
| 64, N=8 | 8 | 8 | 0-7 → 0; 8-15 → 1; ...; 56-63 → 7 |

每个 ring 服务恰好（或差 1）`block_size` 个 worker。✓

### 5.3 关键性质

- Worker 一辈子用同一个 ring → ring tail cacheline 只在 block_size 个
  worker 间共享（vs 当前 T=64 个全员争用）→ tail `fetch_add` 大幅减少
  ping-pong
- Receiver i 永远只 drain ring_idx=i 上来的 op → receiver L1 静态 warm

---

## 6. Receiver 线程 + CPU pinning

### 6.1 CPU 池（动态）

```
total_cpus = sysconf(_SC_NPROCESSORS_ONLN)   // 86 (运行时自检)
receiver_pool_start = T                       // 紧跟 worker
receiver_pool_size  = total_cpus - T          // = 86 - T
```

Worker 占 [0, T)，Receiver 占 [T, T+R)。T 越小 receiver pool 越大。

**Capacity 检验**：

| T | receiver pool size | T=N=4 needed | T=N=8 needed |
|---:|---:|---:|---:|
| 1 | 85 | 3 | 3 |
| 8 | 78 | 6 | 3 |
| 16 | 70 | 12 | 6 |
| 32 | 54 | 24 | 12 |
| 64 | 22 | **48 ✗** | **24 ✗** |

只有 T=64 触发 packing（48>22 或 24>22）。T≤32 任何 N 都 fit 1-to-1。

### 6.2 3 类 receiver CPU 分配（动态）

```c
int pool_size = total_cpus - T;
int budget_base  = pool_size / 3;
int budget_extra = pool_size % 3;          // 给 write

int threads_w = min(actual_shards, budget_base + budget_extra);
int threads_r = min(actual_shards, budget_base);
int threads_i = min(actual_shards, budget_base);

// CPU 范围（紧跟 worker）：
//   write threads:  [T,            T + threads_w)
//   read threads:   [T + threads_w, T + threads_w + threads_r)
//   inval threads:  [T + threads_w + threads_r,
//                    T + threads_w + threads_r + threads_i)
```

T=64 时 pool=22 → budgets = (8/7/7)，跟之前 hard-code-64 同布局；
T=8 时 pool=78 → budgets = (26/26/26)，但实际 shards 都 ≤ 2，所以
threads_w/r/i 都 = 2，1-to-1 pin。

### 6.3 Ring → thread 分配（一类内部）

某类有 `S` 个 ring，`K` 个 thread (`K ≤ S`)：

```c
// thread i 处理 [i*S/K, (i+1)*S/K) 区间的 ring
int ring_start = i * S / K;        // 整数除
int ring_end   = (i+1) * S / K;
// thread i 跑 ring_idx ∈ [ring_start, ring_end)
```

特点：
- 连续切（cache-friendly）
- 当 S 不能被 K 整除时，"加班"的 thread 在序列中均匀分布
- 当 S ≤ K 时（K 取了 min(S, budget)），每 thread 恰好 1 ring（1-to-1）

### 6.4 具体配置实例（start_cpu = T）

| T | N | shards/type | threads (W/R/I) | rings/thread | CPU 范围 |
|---|---|---|---|---|---|
| 1 | 4/8 | 1 | 1/1/1 | 1 each | 1, 2, 3 |
| 2 | 4/8 | 1 | 1/1/1 | 1 each | 2, 3, 4 |
| 4 | 4/8 | 1 | 1/1/1 | 1 each | 4, 5, 6 |
| 6 | 4 | 2 | 2/2/2 | 1 each | 6-11 |
| 8 | 4 | 2 | 2/2/2 | 1 each | 8-13 |
| 8 | 8 | 1 | 1/1/1 | 1 each | 8-10 |
| 12 | 4 | 3 | 3/3/3 | 1 each | 12-20 |
| 16 | 4 | 4 | 4/4/4 | 1 each | 16-27 |
| 16 | 8 | 2 | 2/2/2 | 1 each | 16-21 |
| 32 | 4 | 8 | 8/8/8 | 1 each | 32-55 |
| 32 | 8 | 4 | 4/4/4 | 1 each | 32-43 |
| 64 | 4 | 16 | **8/7/7** packed | W 2/t, R/I 2-3/t | 64-85 |
| 64 | 8 | 8 | 8/7/7 packed | W 1/t, R/I 7t × {2,1,1,1,1,1,1} | 64-85 |

**关键观察**：动态 start_cpu = T 后，**只有 T=64 触发 packing**。
T=32 + N=4 现在 fit 1-to-1（pool=54，需要 24）。其他配置都干净 1-to-1
绑定。

### 6.5 T=64, N=4 完整 CPU 映射（唯一 packing case，最重）

start_cpu = 64，pool_size = 22。

```
write threads (8 个，每个 2 ring):
  thread 0 → CPU 64, rings 0, 1
  thread 1 → CPU 65, rings 2, 3
  thread 2 → CPU 66, rings 4, 5
  thread 3 → CPU 67, rings 6, 7
  thread 4 → CPU 68, rings 8, 9
  thread 5 → CPU 69, rings 10, 11
  thread 6 → CPU 70, rings 12, 13
  thread 7 → CPU 71, rings 14, 15

read threads (7 个):
  thread 0 → CPU 72, rings 0, 1            (2)
  thread 1 → CPU 73, rings 2, 3            (2)
  thread 2 → CPU 74, rings 4, 5            (2)
  thread 3 → CPU 75, rings 6, 7, 8         (3) ← "加班"
  thread 4 → CPU 76, rings 9, 10           (2)
  thread 5 → CPU 77, rings 11, 12          (2)
  thread 6 → CPU 78, rings 13, 14, 15      (3) ← "加班"

inval threads (7 个):  CPU 79-85, 同 read 模式
```

CPU 64-85（22 个）满用 0 浪费。

### 6.6 T=64, N=8 配置（packing 较轻）

start_cpu = 64，pool_size = 22，shards=8。

```
write threads (8 个，每个 1 ring): CPU 64-71
read threads (7 个，6t × 1ring + 1t × 2ring): CPU 72-78
  thread 0-5: 1 ring each (rings 0-5)
  thread 6:   2 rings (6, 7)
inval threads (7 个): CPU 79-85，同 read 模式
```

---

## 7. Software packing receiver loop（方案 Z）

### 7.1 当前 loop（1 ring/thread）

```cpp
void CxlKvStoreA::write_receiver_loop() {
  probe_ring();
  while (!stop) {
    bool did_work = false;
    for (int src = 0; src < num_hosts_; src++) {
      if (src == host_id_) continue;
      WriteRing *ring = &wr_->rings[src][host_id_];     // 当前：单 ring
      // ... drain inner while (head < tail)
    }
    if (!did_work) pause();
  }
}
```

### 7.2 新 loop（packed，每 thread 自己的 ring_idx 列表）

```cpp
struct ReceiverThreadCfg {
  int cpu;                     // pin 目标 CPU
  std::vector<int> ring_indices;  // 这个 thread 负责的 ring shard 列表
};

void CxlKvStoreA::write_receiver_packed_loop(ReceiverThreadCfg cfg) {
  pin_to_cpu(cfg.cpu);
  probe_ring();
  while (!write_receiver_stop_.load(acquire)) {
    bool did_work = false;
    for (int ring_idx : cfg.ring_indices) {
      for (int src = 0; src < num_hosts_; src++) {
        if (src == host_id_) continue;
        WriteRing *ring = &wr_->rings[src][host_id_][ring_idx];
        did_work |= drain_some(ring, src, ring_idx);
      }
    }
    if (!did_work) __builtin_ia32_pause();
  }
}
```

- `drain_some`：非阻塞，一次最多 drain 一段 (head→tail snapshot)，
  返回是否有动作
- 同 cfg 内多 ring 是 round-robin 轮询 → **零 context switch cost**
- 每个 ring 实际 throughput ≈ `single_ring_thpt / cfg.ring_indices.size()`
  （在 1 个 CPU 内 cooperative 摊薄）
- 若 cfg 只 1 个 ring，行为等同 baseline 1-to-1 pin

### 7.3 Read / Inval receiver 同款改造

只是 ring 类型和 `drain_some` 的处理函数不同。

---

## 8. Sender 线程移除

### 8.1 现状

iter-9A Phase 2 引入了 aggregator + sender 线程 scaffolding：
- worker → aggregator slot（per-worker SPSC）→ sender thread → CXL ring

但 xhost_write workload 走 `forward_write_direct` 直接 fetch_add(tail) +
写 ring entry + flush，**不经过 sender**。Sender 线程目前只占 CPU
pinning 槽位，对 hot path 无贡献。

### 8.2 移除范围

- `tests/protocol_a_ycsb.cc` 中 sender 线程 spawn 代码：删除
- `src/cxl_kv_ops_A.cc::start()` 中 sender 启动 + stop：删除
- CPU pinning 中 sender 的槽位：删除
- Sender 相关代码标记 `// iter-9A scaffold, removed iter-18A` 或直接删

### 8.3 Aggregator 数据结构

`send_invalidate()` 仍有可能路由经过 aggregator（line 850-868）。
保留 aggregator 数据结构（CXL 内存仍预留），仅删除 sender 线程；
`send_invalidate` 在 `g_aggr_worker_id < 0` 时已 fallback 到
`send_invalidate_direct`，**确保 worker 永远走 direct 路径**。

具体：在 iter-18A 启动时不设置 `g_aggr_worker_id`，所有 worker 自动 direct。

---

## 9. 启动时配置 (env vars)

| Env | 含义 | 默认 | 测试值 |
|---|---|---|---|
| `FUSEE_NUM_THREADS` | T (workers) | 1 | 1,2,4,8,16,32,64 |
| `FUSEE_RING_SHARDS_FACTOR` | N | 1（兼容 baseline） | 4, 8 |
| `FUSEE_RING_ROUTING` | (Plan B) `worker_id` (A) or `key_hash` (B) | `worker_id` | iter-18A 用 A，iter-18B 用 B |

N=1 ⇒ actual_shards 永远 = 1 ⇒ 行为完全等同 baseline（向后兼容）。

---

## 10. 实施分阶段

### Phase 1: 基础设施
1. 改 `cxl_write_ring.h` / `cxl_read_ring.h` / `cxl_inval_ring.h` 为 3D 矩阵
2. attach 函数初始化 N-维 rings
3. `ReceiverThreadCfg` 数据结构 + 计算逻辑 (`compute_receiver_layout()`)
4. `pin_to_cpu()` helper（已存在则复用）

### Phase 2: Worker side (Plan A)
1. `forward_write_direct`：computed `ring_idx`，取 ring shard
2. `forward_read_direct`（read 路径同款）
3. `send_invalidate_direct`（inval 路径同款）
4. Worker 的 `client_id` 来源已在 attach/start 路径，直接复用

### Phase 3: Receiver side (软件 packing)
1. `write_receiver_packed_loop` / read / inval 三个
2. `start()` 中 spawn N 组 thread（每类 N 个）
3. Stop signal 改成数组（或 atomic flag 共享）
4. Probe ring 初始化注意：probe_ring 也要分 thread

### Phase 4: Sender removal
1. 删除 sender thread spawn + stop
2. 验证 `send_invalidate` 永远走 direct 路径

### Phase 5: Test（按 §11.2 矩阵执行）
1. **Smoke**: Plan A + N=4 + T=8 on xhost_write，预期 ~1.5-2 Mops/s/host
2. **Full sweep A4 + A8**: T=1..64, 3 reps each (vs iter-15A baseline)
3. **Plan B impl** + smoke
4. **Full sweep B4 + B8**: 同上
5. **Pick winner** + full YCSB scaling (5 workloads × 80 cells × 5 reps)
6. 收尾报告写到 iter17A 总结里

---

## 11. Test 计划

### 11.1 Baseline 对照

**Baseline**: iter-15A Phase 2 xhost_write variable T sweep
（`docs/iter15A_microbench_phase2_20260520_063314/grid.csv`）

参数（与 baseline 严格一致）:
- V=1024, cache_pct=10% (131072 buckets)
- keydist=zipf-0.99
- TRANS_OPS = 5,000,000 per host (10M cluster)
- NUM_BUCKETS = 8,388,608
- T ∈ {1, 2, 4, 8, 16, 32, 64}
- **3 reps per cell**
- build = `build-cxl-w1-v1024`（probe-off）

### 11.2 测试矩阵

每个 plan × N 组合跑同一套 T sweep（7 cells × 3 reps = 21 runs）：

| Plan | N | 标签 |
|---|---|---|
| A (worker_id 静态) | 4 | A4 |
| A (worker_id 静态) | 8 | A8 |
| B (key_hash) | 4 | B4 |
| B (key_hash) | 8 | B8 |

总计 4 × 21 = 84 runs。Plan A 实施 + 测完后再做 Plan B 实施 + 测。

### 11.3 三方对比预期

预期所有 4 组都 ≥ baseline；A 和 B 在不同 dimensions 占优：

| T | iter-15A baseline | iter-17A path opt 完成 (单 receiver 上限) | 期望 A4/A8/B4/B8 |
|---:|---:|---:|---|
| 1 | 0.198 | 0.225 | ≥ 0.225 |
| 2 | 0.299 | 0.265† | ≥ 0.265 |
| 4 | 0.527 | 0.595 | ≥ 0.595 |
| 8 | 0.554 | 0.900 | **shards=2 时 显著突破** |
| 16 | 0.563 | 0.951 | **shards=2-4 时显著突破** |
| 32 | 0.612 | 1.021 | shards=4-8 接近 2× 当前 |
| 64 | 0.604 | 1.003 | shards=8-16，**目标 3-5× 当前**（→ 3-5 Mops/s/host）|

† T=2 baseline 在 iter-17A 测出 -11% rep 间不稳，不作硬上限

### 11.4 A vs B 预期差异

| 比较点 | A (worker_id) | B (key_hash) |
|---|---|---|
| Worker → ring cacheline 争用 | 低（同 worker 永远同 ring）| 高（每 worker 触全部 ring）|
| Receiver bucket cacheline locality | 中（hot key 可能分散）| 高（同 key 同 receiver）|
| Zipf hot key 处理 | hot bucket cacheline 在 receiver 间 ping-pong | hot bucket 串行在 1 receiver，其他 receiver 闲 |
| 实施复杂度 | 低 | 低（仅 routing 逻辑变）|

预期：
- **uniform dist**：A 应该略胜（worker locality 优势直接兑现）
- **zipf-0.99**：B 的 receiver-side cacheline locality 与 A 的 worker-side
  cacheline locality 互换，需实测判断
- **scale 上限**：A 受 N receivers 并行能力限制；B 在 zipf 下受 hot key
  单 receiver 限制 —— 两个都可能不能完全线性 scale

### 11.5 Winner pick + 完整 YCSB scaling

A/B/N 四组结果出来后，按以下规则选 winner：

1. **如果 A 和 B 在 xhost_write zipf-0.99 上接近**（差异 < 10%）：选 **A**
   （实施更简单）
2. **如果 B 显著占优**（特别在高 T zipf 下 >20%）：选 **B**
3. **N=4 vs N=8** 在 winner 内选 thpt 更高的，作为后续 scaling
   benchmark 的 default

确定 winner 后，跑完整 **scaling_ycsb** 套件（docs/scaling_ycsb_spec.md）：
- 5 workloads (a, b, c, d, f)
- 80 cells × 5 reps = 400 runs
- 验证 doubling-ratio gate (§13 gate 5 all workloads)
- 比较 vs iter-15A 同等 ycsb baseline
- 检查 20 Mops/s 目标进度

YCSB scaling 结果作为本 iter 收尾报告。

---

## 12. 风险 + 缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| 多 receiver 在 hot bucket 上 cacheline ping-pong（Plan A 特有）| Zipf 工作负载下 receiver 实际不能并行 | Plan B 作 alternative；按 workload 选 routing |
| Receiver packing 时单 ring throughput 摊薄 | T=64+N=4 极重 packing 下单 receiver pack 2-3 ring → 单 ring 吞吐降至 1/2-1/3 | CPU 是 hard limit；若需更高吞吐要增加节点 |
| 22 CPU 仍是 hard ceiling | Total receiver thpt ≤ 22 × single | scaling 上限 ~22 Mops/s（理论），实测可能 10-15 |
| 现有 invariant（H1-H4 gate）| 3D ring 索引会触发 invariant assert？| 实施前先 grep H1-H4 trip wires 看是否依赖二维 |
| Probe ring 配 N 个 receiver | 每 receiver 独立 probe_ring 还是共享？| 独立（避免争用）；probe_dump 文件名加 ring_idx 后缀 |
| 当前 `cxl_kv_ops_A.cc` 大量 `wr_->rings[*][*]` 二维访问点 | 改 3D 后所有访问都要加 ring_idx | grep 找全；编译错误会暴露遗漏 |
| 启动 60+ receiver 线程的 init 顺序 | g3/g4 双 host 都要 N×3 个 thread 准备好才能 trans_go | 现有 init_barrier 仍生效（bit-based），但要确认所有 receiver ready 后再 publish |

---

## 13. Open / Future work（不阻断本 iter）

- **Plan B (key_hash routing) 单独 iter**：iter-18A 完成 + 测后再做
- **NUMA awareness**: 节点是 1 socket，暂不涉及；将来如果跨 socket，receiver
  应优先 pin 到 worker 所在 socket 的 sibling
- **动态 shard 数**：当前 startup-fixed。将来若需自适应可考虑（但复杂度大）
- **Receiver per ring throughput characterization**：测出每 ring 单 receiver
  的实际 Mops/s，作为 capacity planning input
- **CPU 65/66 ... 给 read/inval 的 split**：当前 write 拿余数；workload-a
  下 inval 可能更需要资源，将来可加 env var 调

---

## 14. RAP（§XIII Reviewer Attack Process）

待 Phase 1 实施前完成。本设计涉及：
- 新架构（multi-shard ring）
- 新数据结构（3D ring matrix）
- 默认值（N 来源、CPU 起点、packing 策略）
- 已知冲突（Plan A 的 hot-bucket ping-pong vs single-receiver 序列化）

按 §XIII 模板写 RAP 单独文件 `iter17A_scaling_RAP.md`，包含 STATE、
ATTACK VECTORS（≥6 类）、ABLATION CHECK、PRIOR ART CHECK、VERDICT、
DECISION。

---

## 15. Backward compat

- `FUSEE_RING_SHARDS_FACTOR=1` → actual_shards 永远 1 → 单 ring + 单
  receiver per type → **行为等同 iter-17A baseline**
- 现有 unit test / hash-diff battery 不变（除非测试本身 hard-code 了
  二维 ring，需要改成三维兼容）
- Memory layout 变（CXL region size 涨 ~16 MB），需要 reformat dax 或
  接受新 layout
- attach() 签名不变（额外参数从 ring matrix size 隐含派生）

---

## 16. Phase 1 ready 时再开 RAP

本文档作为设计 reference。实施前先做 RAP（attack vector 评估），通过
后进入 Phase 1 编码。

---

## 17. iter-17A 总结归并

本设计 + 实施 + 测试结果作为 iter-17A 第二部分，最终把以下三个文档
合并到 iter-17A 整体总结：

1. [iter-17A 路径优化总结](iter17A_xhost_write_path_opt_summary.md)
   — 已完成（commits 3786c86 → b1d6c31）
2. iter17A_multi_ring_receiver_scaling_design.md（本文档）
3. iter17A_scaling_RAP.md（待写）
4. iter17A 总结收尾（待写）— 包含三方对比 + YCSB scaling 结果 +
   20 Mops/s 目标进度评估

iter-17A 完整范围 = 路径优化 + multi-ring scaling + winner-based YCSB
scaling。
