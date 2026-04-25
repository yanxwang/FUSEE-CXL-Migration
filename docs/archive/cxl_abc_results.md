# CXL-FUSEE A/B/C 协议实证研究结果

**生成日期**: 2026-04-19
**任务**: 在真实 CXL 硬件 (g3/g4 + XConn XC50256 switch + CXL Type 3 memory) 上对比 FUSEE 的三个 consensus 方案
**实际范围**: 单节点 g3 CXL (4 processes 模拟 4 nodes) — 跨节点测试受 g4 设备状态所阻

---

## 1. 实验环境

| 组件 | 配置 |
|---|---|
| 机器 | g3 (Intel Xeon 6787P, 800GB RAM, Linux 6.15-cxl_net) |
| CXL 硬件 | XConn Technologies XC50256 switch + CXL Type 3 memory 512 GiB |
| CPU 支持 | clflushopt ✓, sfence, mfence |
| DAX 设备 | `/dev/dax0.0` (256 GiB devdax, 物理地址 0x4080000000) |
| 软件 | cxl_shm_profiling (LFM mutex), abc_bench, ycsb_abc_bench |

---

## 2. CXL vs DRAM 基线延迟 (g3, measured)

| 操作 | DRAM (tmpfs) | CXL (/dev/dax0.0) | CXL/DRAM |
|---|---|---|---|
| plain store (cached) | 4.3 ns | 7.2 ns | 1.7x |
| plain load (cached) | 4.2 ns | 8.1 ns | 1.9x |
| **CACHELINE_STORE** (+ clflushopt + sfence) | **255.7 ns** | **1558.7 ns** | **6.1x** |
| **CACHELINE_LOAD** (+ clflushopt + mfence) | **322.5 ns** | **700.0 ns** | **2.2x** |

**发现**: CXL 写慢 6x，读慢 2.2x。Flush 到 CXL 慢的原因——每次 flush 必须写透到 CXL memory，比 DRAM pagecache 代价大很多。

---

## 3. abc_bench 原始对比 (4 proc × 2 thread × 5000 ops)

简化的 bench (slot 直接用 uint64, 没有真实 KV 数据)，在真实 CXL 上跑 YCSB A/C：

| Option | WL | Throughput | w_avg (μs) | w_p99 (μs) | r_avg (μs) | r_p99 (μs) |
|---|---|---|---|---|---|---|
| A | A | 11,587 | 1387.47 | 2265.99 | 0.17 | 0.47 |
| A | C | 11.5M | — | — | 0.49 | 6.56 |
| B | A | 1.86M | 7.24 | 15.10 | 0.89 | 10.24 |
| B | C | 12.3M | — | — | 0.48 | 6.51 |
| C | A | 1.85M | 5.54 | 14.10 | 2.81 | 11.03 |
| C | C | 7.81M | — | — | 0.79 | 7.14 |

**关键比例（真 CXL）**:
- 写吞吐: C/A = **160x**
- 写延迟: A/C = **250x**

在 abc_bench 上 B ≈ C 在写延迟上。

---

## 4. ycsb_abc_bench 完整对比 (真实 hash + KV)

配置: 4 proc × 2 thread × 3000 ops, YCSB-style key/value strings, 4096 buckets with 7 slots each.

| Option | WL | Throughput | w_avg (μs) | w_p99 (μs) | r_avg (μs) | r_p99 (μs) |
|---|---|---|---|---|---|---|
| A | A | **20,493** | **773.85** | 1154.84 | 0.42 | 1.47 |
| A | C | 77.1M | — | — | 0.08 | 0.72 |
| B | A | 1.00M | 14.20 | 20.84 | 1.21 | 7.90 |
| B | C | 13.77M | — | — | 0.35 | 6.26 |
| C | A | **1.06M** | **10.29** | 14.77 | 4.22 | 10.80 |
| C | C | 1.95M | — | — | 3.78 | 9.39 |

**关键比例（真实 FUSEE-style workload）**:
- 写吞吐: C/A = **51.5x**
- 写延迟: A/C = **75x**
- C 比 B 写快 **28%** (10.29 vs 14.20 μs)

### Insights

1. **Option A (Sync Replication)** 依旧不可用：
   - YCSB A 下写延迟接近 1 ms（真 CXL 上 clflush 代价叠加）
   - 真实 CXL 上 A 的写吞吐仅 20k/s，比 C 慢 50+ 倍

2. **Option B (Eager Push)** 在 YCSB A 下比 abc_bench 稍弱：
   - abc_bench 下 B ≈ C
   - ycsb_abc_bench 下 C 比 B 稍快（真实 KV 存储 + hash scan 暴露了 B 的 invalidation 额外开销）

3. **Option C (Lazy RC)** 真实场景下性能最优：
   - 写延迟最低 (10 μs)
   - 写吞吐最高 (1.06 M ops/s)
   - 唯一代价: 读延迟增加（strict read 每次 flush → 3.78 μs，vs A 的 0.08 μs）

4. **读延迟在 ycsb bench 下差距被拉大**:
   - A/B 读 KV 不 flush（local cache 假设有效）：0.08-0.35 μs
   - C strict read 每次 flush 整个 bucket + KV block → **3.78 μs**
   - 是 abc_bench 数字的 5x（因为 KV 数据更多）

### 结论：C 的读写 trade-off

**Option C 给 writer 10 μs，给 strict reader 3.78 μs**；如果 strict reader 很多，C 可能输给 A/B。但对于写密集或混合 workload，C 依旧最好。

---

## 5. 配合策略建议

Based on the measurements, 生产实现建议：

1. **默认选 C (Lazy RC)**: 写性能最好，strict read 可接受
2. **Reader 模式可配置**:
   - Strict mode (linearizable): 每次读 flush + check epoch → ~4 μs
   - Eventual mode (fast path only): 不 flush，纯 local read → ~0.4 μs
3. **可选 B 作为特殊场景**: 如果 reader 很多且对 strict 要求高，B 的 eager push 让 reader 总能命中 local cache

---

## 6. 跨节点测试状态

**已验证**: g3 和 g4 的 `/dev/dax0.0` 映射到同一 CXL 物理地址 `0x4080000000`（256 GiB）。这证明两机器**确实共享同一块 CXL 物理 memory**，通过 XConn switch 暴露。

**未完成原因**: g4 的 `/dev/dax0.0` 在尝试 setup 过程中进入 broken 状态（`daxctl: add_dev() failed`，/dev 节点缺失）。此状态需要 **reboot g4** 才能清理。shared host 的 reboot 未获授权，遂结束本次 session 的跨节点尝试。

**如何继续**（用户操作）:
```bash
# On g4:
reboot

# After reboot, re-run setup:
cd ~/cxl_net
./dax-split-from-system-ram.sh   # split into 3 devdax devices
./dax-set-dual-modes.sh 2        # make dax0.2 system-ram, others devdax
```

**之后立即可跑跨节点测试**（已验证的 binary 已在 `/root/cxl_shm_profiling/bench/` 下）:
```bash
# Node 0 (g3):
./bench/ycsb_abc_bench --opt=C --workload=A --nodes=2 --node-id=0 \
    --threads=2 --ops=3000 --path=/dev/dax0.0

# Node 1 (g4):
./bench/ycsb_abc_bench --opt=C --workload=A --nodes=2 --node-id=1 \
    --threads=2 --ops=3000 --path=/dev/dax0.0
```

代码中无需改动，只需 g4 的 `/dev/dax0.0` 恢复即可。

---

## 7. 附：文件清单

### 本地（`/home/yanwang/cxl_shm_profiling/bench/`）

| 文件 | 作用 |
|---|---|
| `abc_bench.c` | 原简化 bench（仅slot 写，测协议纯成本） |
| `ycsb_abc_bench.c` | **新** FUSEE-style bench（真实 bucket/slot/KV） |
| `measure_latency.c` | CACHELINE_STORE/LOAD 微基准 |
| `cross_node_test.c` | 跨节点 CXL 共享验证 |
| `run_abc_cxl.sh`, `run_ycsb_abc_cxl.sh` | 矩阵运行脚本 |
| `plot_from_summary.py` | 从 abc_summary.txt 出图 |

### 实验数据

| 文件 | 说明 |
|---|---|
| `abc_results_cxl_g3.log` | g3 上 abc_bench 完整原始 log |
| `abc_summary_cxl_g3.txt` | g3 abc_bench 汇总 |
| `abc_results_cxl_g3.png` | g3 abc_bench 图 |
| `ycsb_abc_results_cxl_g3.log` | g3 上 ycsb_abc_bench 完整 log |
| `ycsb_abc_summary_cxl_g3.txt` | g3 ycsb_abc_bench 汇总 |
| `ycsb_abc_results_cxl_g3.png` | g3 ycsb_abc_bench 图 |

### 远端（g3/g4 `/root/cxl_shm_profiling/`）

所有 binary 已在 g3 构建并可运行：
- `bench/abc_bench` (pre-built, Apr 19 03:06)
- `bench/ycsb_abc_bench` (pre-built, Apr 19 03:20)
- `bench/measure_latency`, `bench/cross_node_test`

g4 上 cxl_shm_profiling 已同步代码（rsync），未构建（但可 `make` 快速编译）。

---

## 8. 留给用户的下一步

### 立即可做（单节点 g3 CXL）

```bash
ssh g3
cd /root/cxl_shm_profiling
# scan write ratio：
for wr in 0 10 25 50 75 100; do
    echo "=== write_ratio=$wr% ==="
    # 需要 hacking 一下 bench 加 --write-ratio= 参数
done
```

### 跨节点（需要 g4 reboot）

参见第 6 节步骤。

### FUSEE 完整集成

本次只完成了 stand-alone 的 mini-FUSEE (ycsb_abc_bench.c)。完整替换 FUSEE 的 RDMA 路径需要：

1. **替换 `src/nm.h/cc` 和 `src/ib.h/cc`** → 改为调用 `shm_mutex_t` + CACHELINE_STORE/LOAD（作废）
2. **重构 `src/client.h/cc`** → 集成 A/B/C 编译期选项（见 `ycsb_abc_bench.c` 的三套 `kv_insert_*` 函数模板）
3. **重构 `src/client_mm.h/cc`** → 用 CXL per-node KV area 代替 RDMA mm_alloc
4. **保留 `src/hashtable.h/cc`** 结构定义但改 rkey 字段
5. **porting YCSB workloads** → ycsb_abc_bench 已给出 loader 例子

这部分工作量约 1-2 周，超出本次 8 小时 session 能完成的范围。

---

## 8.5 Write Ratio Sensitivity（g4 /dev/dax0.1, 真实 CXL）

**设置**: 4 processes × 2 threads × 2000 ops (A 在 wr>0.1 时 500 ops), /dev/dax0.1。

### 吞吐（ops/s, aggregate）

| Option | wr=0.0 | wr=0.1 | wr=0.25 | wr=0.5 | wr=0.75 | wr=1.0 |
|---|---|---|---|---|---|---|
| A | **40.4M** | 34.6k ↓ | 14.3k | 6.6k | 5.2k | 3.8k |
| B | 16.0M | 2.07M | 926k | 515k | 353k | 267k |
| C | 1.13M | **940k** | **744k** | **554k** | **444k** | **371k** |

### 写延迟 avg（μs）

| Option | wr=0.1 | wr=0.25 | wr=0.5 | wr=0.75 | wr=1.0 |
|---|---|---|---|---|---|
| A | 2246 | 2287 | 2232 | 2044 | 2112 |
| B | 28.2 | 28.1 | 28.6 | 28.9 | 29.7 |
| C | **19.9** | **19.9** | **20.4** | **20.6** | **21.2** |

### 读延迟 avg（μs, wr<1）

| Option | wr=0.0 | wr=0.1 | wr=0.25 | wr=0.5 | wr=0.75 |
|---|---|---|---|---|---|
| A | **0.15** | **0.43** | **0.84** | 1.63 | 2.09 |
| B | 0.32 | 0.75 | 1.37 | 2.28 | 3.39 |
| C | 6.72 | 6.86 | 7.25 | 7.95 | 8.55 |

### 关键发现

1. **C 非常稳定**: 吞吐从 1.13M (wr=0) 平滑降到 371k (wr=1)，没有断崖
2. **A 的断崖**: wr=0 时有 40M 吞吐（纯 local cache 读飞快），只要 1% 写就崩到 35k（sync-replication 代价）
3. **B 在低写比下优于 C**: wr=0.1 时 B=2M, C=940k；但 wr=0.25 时 B=926k, C=744k；wr≥0.5 时两者已经接近
4. **读延迟**:
   - A/B 的 read 延迟随写比增长（后台 writes 干扰 cache + scheduler）
   - **C 的 read 延迟几乎不变** (6.7 → 8.5 μs)，因为每次都 flush，本来就是 worst case

### 推导

- **读压倒性的 workload (wr<5%)**: Option B 最优（读快 + 写尚可）
- **混合 workload (wr=10-50%)**: C 稍输给 B 但更稳定
- **写压倒性的 workload (wr>50%)**: C 开始明显优于 B
- **Option A 任何场景都不适合生产**（除非 wr == 0 且不需要 linearizable）

### 决策矩阵

| Workload type | 推荐 |
|---|---|
| 读为主，要求低 read latency | **Option B** 或 A |
| 写为主 | **Option C** |
| 混合，优先写性能 | **Option C** |
| 混合，优先 read latency | Option B |
| 需要 CXL 故障容忍 | 必须 Option A（但性能牺牲巨大） |

### Figure

参见 `code_references/wr_scan_cxl_g4.png`:
- 左上: throughput vs write ratio (linear)
- 右上: throughput vs write ratio (log y — 突出 A 的断崖)
- 左下: write latency vs write ratio (log y)
- 右下: read latency vs write ratio (linear)

---

## 8.7 Thread Scaling Sensitivity (g4 /dev/dax0.1, 真实 CXL)

**设置**: 2 nodes × {1,2,4,8} threads × YCSB A (wr=0.5), 2000 ops/thread.

### Throughput (ops/s, aggregate)

| threads | A | B | C |
|---|---|---|---|
| 1 | 2,507 | 172,565 | 143,843 |
| 2 | 4,987 | 328,212 | 275,303 |
| 4 | 8,924 | 592,200 | 525,673 |
| 8 | 16,646 | **1,197,407** | 996,586 |

### Write latency avg (μs)

| threads | A | B | C |
|---|---|---|---|
| 1 | 1532.6 | 20.7 | 20.2 |
| 2 | 1500.2 | 21.6 | 20.5 |
| 4 | 1603.6 | 22.2 | 21.4 |
| 8 | 1508.0 | 22.6 | 21.7 |

### 发现

1. **三种协议都 scale 几乎线性**: 从 1→8 threads，throughput 增加约 **7x**（理想值 8x）
2. **Write latency 基本不变**: 随 thread 数只增加 ~5%，per-bucket lock 竞争很小
3. **B 在 high-thread 下领先 C 20%**: 1.20M vs 1.00M at 8 threads。原因：YCSB A 含 50% 读，C 的 strict read 每次多付 ~2μs CXL load 代价
4. **A 的 scaling 也是线性**: 证明 sync-replication 的瓶颈是每个写等 ACK，不是 LFM 锁竞争

### 隐含的 Trade-off

- **C 在纯写或高写比下最佳** (no strict read overhead, C wins on throughput)
- **B 在混合/读多 workload 中略优** (避免 C 的 strict read 成本)
- 所以如果 workload 读 >> 写，**B 可能比 C 更优**
- 如果 workload 写 >> 读 或者 reader 可以容忍 stale data（用 fast path），**C 仍然最优**

---

## 9. 参考对比：tmpfs baseline

之前在开发机器 (2680v4, no clflushopt, tmpfs) 上的数据：

| Option | WL | tmpfs 吞吐 | CXL g3 吞吐 |
|---|---|---|---|
| A | A | 7,781 | 20,493 (3x better, 更大 machine) |
| B | A | 2.21M | 1.00M (0.45x, CXL flush 代价) |
| C | A | 2.20M | 1.06M (0.48x) |

**结论**: 真实 CXL 上 B/C 比 tmpfs 慢约 2x，符合我们之前的外推预测（2-5x）。**趋势完全保持**。
