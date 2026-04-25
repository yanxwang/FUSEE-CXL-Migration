# CXL-FUSEE Documentation Index

本目录汇总了 FUSEE → CXL 重构项目的所有设计、实现、实验数据。

## 核心文档（按阅读顺序）

| 文件 | 描述 | 长度 |
|---|---|---|
| **`consensus_transformation_explained.md`** | 完整设计文档：RDMA→CXL 转变逻辑 + LFM 方案 + 实验验证 | 814 行 |
| **`cxl_architecture_plan.md`** | 架构计划（含 REVISION 2 LFM 章节） | 572 行 |
| **`cxl_abc_results.md`** | 真实 CXL 硬件实验结果（g3 + g4） | 284+ 行 |
| **`cxl_implementation_guide.md`** | 从 mini-bench 到完整 FUSEE 集成的路线图 | 163 行 |
| **`implementation_log.md`** | 2026-04-19 session 时间轴 | 332+ 行 |

## PPTX 幻灯片（12+ 页面）

| 文件 | 内容 |
|---|---|
| `cxl_abc_results_summary.pptx` | **Real CXL experimental results (8 slides, MAIN)** |
| `hashtable_architecture.pptx` | RACE hash 的 bucket/slot 结构 |
| `cxl_lfm_consensus_steps.pptx` | LFM-based KV 操作 7 步流程 |
| `cxl_staging_oplog.pptx` | Staging Buffer + OpLog 设计 |
| `lfm_substeps.pptx` | LFM 锁 acquire 内部步骤 (8 slides) |
| `rc_discussion.pptx` | Eager vs Lazy RC 对比 (7 slides) |
| `consensus_comparison.pptx` | RDMA vs Old CXL vs New CXL 对比 |

## Code References

`code_references/` 目录包含所有 mini-bench 源码和实验数据：

### 源码
- `abc_bench.c` — 原型 ABC bench（slot-only, ~650 行）
- `ycsb_abc_bench.c` — **FUSEE-style ABC bench**（real hash + KV, ~700 行）
- `measure_latency.c` — CXL latency 微基准
- `cross_node_test.c` — 跨节点共享验证

### 运行脚本
- `run_ycsb_abc_cxl.sh` — 在 CXL 上跑 YCSB A/C 矩阵
- `run_wr_scan_cxl.sh` — 扫 write ratio (0% - 100%)
- `run_thread_scan.sh` — 扫 thread count (1, 2, 4, 8)

### 绘图脚本
- `plot_from_summary.py` — 从 abc_summary.txt 出图
- `plot_wr_scan.py` — write-ratio 趋势
- `plot_thread_scan.py` — thread scaling 趋势

### 实验结果 (真实 CXL)

**abc_bench on g3**:
- `abc_summary_cxl_g3.txt`, `abc_results_cxl_g3.png`

**ycsb_abc_bench on g3**:
- `ycsb_abc_results_cxl_g3.log`, `ycsb_abc_summary_cxl_g3.txt`, `ycsb_abc_results_cxl_g3.png`

**Write-ratio scan on g4**:
- `wr_scan_cxl_g4.log`, `wr_scan_cxl_g4.png`

**Thread scaling on g4**:
- `thread_scan_cxl_g4.log`, `thread_scan_cxl_g4.png`

## 外部依赖

- `/home/yanwang/cxl_shm_profiling/` — LFM mutex 库（本地）
- `g3:/root/cxl_shm_profiling/` — 部署在 g3，binary 已 build
- `g4:/root/cxl_shm_profiling/` — 部署在 g4，binary 已 build

## 关键结论（TL;DR）

1. **Option C (Lazy RC) 是推荐方案**: 写延迟最低 (~10-20μs 真 CXL), 最稳定的性能曲线，读略慢但可接受。
2. **Option A 不可用**: 写延迟 ~2000 μs，吞吐仅为 C 的 1/300。
3. **Option B 在读为主 workload 下优于 C**: 因为 C 的 strict read 每次付 CXL load 代价。
4. **真实 CXL vs tmpfs 性能比**: 约 2-5x 差距，趋势完全一致。
5. **跨节点设计验证**: g3 和 g4 的 `/dev/dax0.0` 映射到同一物理地址（0x4080000000），硬件层面已满足共享条件。
