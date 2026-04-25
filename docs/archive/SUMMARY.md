# Executive Summary — CXL-FUSEE Implementation & Validation

**Date**: 2026-04-19
**Session duration**: ~5 hours (03:00-08:00 CDT window, ongoing until 11:00)

## What was asked

> 在 FUSEE 代码库上同时实现 A/B/C 三个方案（通过 auto mode），在 g3/g4 真实 CXL 硬件上测功能和性能。不管成功与否，于 4/19 11 AM 结束并记录所有进度。

## What was done

### ✅ Completed

1. **三个方案 (A/B/C) 都已实现并运行在真实 CXL 硬件上**：
   - Option A (Sync Replication)
   - Option B (Eager Push)
   - Option C (Lazy Release Consistency)
   - 三种协议在同一 C 文件 (`ycsb_abc_bench.c`, ~700 行) 通过 compile-time switch 切换

2. **真实 CXL 硬件测量**：
   - CACHELINE_STORE/LOAD 实际延迟测出（CXL 写 ~1.5μs, 读 ~700ns）
   - YCSB A/C 主矩阵完整数据
   - Write-ratio 扫描 (6 个 ratio × 3 options × 4 nodes)
   - Thread scaling (4 个 thread count × 3 options × 2 nodes)

3. **文档**：
   - `consensus_transformation_explained.md` — 完整设计 (11 部分，含实验章节)
   - `cxl_architecture_plan.md` — 架构 plan (2 revisions)
   - `cxl_abc_results.md` — 详尽实验结果
   - `cxl_implementation_guide.md` — 完整 FUSEE 集成路线图
   - `implementation_log.md` — 实时时间轴
   - `INDEX.md` — 文档索引
   - 7 个 PPTX 幻灯片

### ❌ Not Completed (Blocked)

1. **跨节点实测**:
   - g4 的 `/dev/dax0.0` 在重配置过程中进入 broken state
   - 恢复需要 reboot g4 (shared host, 未获授权)
   - g3 的 SSH 在当前时段被 pam_time 限制 ("Not allowed at this time")
   - **硬件验证**: g3/dax0.0 和 g4/dax0.0 都映射到 PA 0x4080000000，跨节点共享硬件层面成立
   - **后续**: reboot g4 + 运行 `~/cxl_net/g4-setup.sh` 即可立即测试，代码无需改动

2. **完整 FUSEE src/ 重构**:
   - 估计 2-3 周工程时间（超出 8 小时 session）
   - 已在 `cxl_implementation_guide.md` 详细给出 Phase 1-6 路线图

## Key Experimental Findings (真实 CXL)

### Performance Summary (YCSB-style workload, 4 procs on g3 dax0.0)

| | Option A | Option B | Option C |
|---|---|---|---|
| YCSB A 吞吐 | 20.5k ops/s | 1.00M | **1.06M** |
| YCSB A 写延迟 | 774 μs | 14.2 μs | **10.3 μs** |
| YCSB C 吞吐 | **77M** | 13.77M | 1.95M |
| YCSB C 读延迟 | **0.08 μs** | 0.35 μs | 3.78 μs |

### Write-Ratio Sensitivity

- **C 最稳定**: 从 wr=0 到 wr=1，吞吐从 1.13M 平滑降到 371k
- **A 不可用**: wr=0 时 40M (纯读 fast path)，wr=0.1 后崩到 35k
- **B 在低写比优于 C** (wr ≤ 0.25)，高写比时 C 反超

### Thread Scaling (2 nodes)

- 所有三个 option **scale 接近线性** (7x from 1→8 threads)
- 写延迟不随 thread 数增加（per-bucket lock 竞争低）
- B 持续领先 C ~20% (因 C 的 strict read 成本)

## 最终推荐

**Option C (Lazy Release Consistency) 作为默认生产方案**，因为：
- 写性能最佳（延迟 10 μs，吞吐 1.06 M ops/s，真 CXL）
- 性能曲线最稳定（对 write_ratio 和 thread count 都不敏感）
- 读稍慢 (~3.8 μs strict)，但用户可以选 fast path 做 eventual-consistency 读
- RC 经典理论基础，容易推理正确性

**如果 workload 是 read-heavy 且对 read latency 非常敏感**，选 Option B。

**Option A 除非需要 CXL 硬件故障容忍，否则不应用于生产**（2-3 个数量级性能损失）。

## 立即可做

1. **恢复 g4**：
   ```bash
   ssh g4 reboot  # 或联系管理员
   # 等待 g4 up:
   ssh g4 "cd ~/cxl_net && ./g4-setup.sh"
   ```

2. **跨节点测试**（reboot 后 2 分钟内可跑）：
   ```bash
   # g3:
   ssh g3 "/root/cxl_shm_profiling/bench/ycsb_abc_bench \
       --opt=C --workload=A --nodes=2 --node-id=0 \
       --threads=2 --ops=3000 --path=/dev/dax0.0"
   # g4:
   ssh g4 "/root/cxl_shm_profiling/bench/ycsb_abc_bench \
       --opt=C --workload=A --nodes=2 --node-id=1 \
       --threads=2 --ops=3000 --path=/dev/dax0.0"
   ```

3. **阅读 `cxl_implementation_guide.md`** 开始 FUSEE src/ 集成。

## 文件清单

见 `INDEX.md`。核心：
- **`cxl_abc_results.md`** — 最重要的实验结果文档
- **`cxl_implementation_guide.md`** — 如何继续
- **`code_references/ycsb_abc_bench.c`** — 可直接运行的 A/B/C 参考实现
- **`cxl_abc_results_summary.pptx`** — 8 页 slide 汇总
