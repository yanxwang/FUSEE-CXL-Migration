# iter-18A — xhost_read 主文档

**iter 目标**: 对 xhost_read 路径复用 iter-16A (decomp) + iter-17A (per-stage iterative opt + multi-ring/multi-receiver) 全套方法。
**Plan reference**: [iter18A_xhost_read_plan.md](iter18A_xhost_read_plan.md) v3 final (12 QR all closed).
**Start**: 2026-05-23
**Status**: 进行中（Phase 1 done, Phase 2 starting）

---

## TL;DR (随每个 phase 推进而更新)

- ✅ **Phase 1 done** (2026-05-23): RDTSCP+LFENCE stage-decomp 框架 wired (9 stages = 6 worker + 3 receiver, 14 probe sites)。Sanity verified: ∑Stage = StageW to 99.3-99.8% across T={1,8,64}; probe overhead -26%/-17%/-21%。**Stage 4 (ack_wait) dominates 47-97% of StageW** → 与 iter-16A xhost_write 同模式。
- 🔄 **Phase 2 in progress**: 4 sweeps (T / V / dist / perfstat).
- ⏳ **Phase 3 pending**: per-stage iterative path opt (≤2 LOC + smoke + keep/revert/audit), 8+ candidates。
- 🚩 **Phase 4 RCA preview (found by code reading 2026-05-23 while Phase 2 ran)**: iter-17A 217× 退化的根因 = **ReadStagingMatrix 是 2D `slots[req][owner][slot]`，缺 shard 维度**。当 N>0 让 actual_shards>1 时，多个 shard 的 ring 都把响应写入同一 staging slot → race + corrupt → workers spin XRS4T 超时 → 吞吐崩。Fix: ReadStagingMatrix 改 3D `slots[req][owner][shard][slot]`，所有 `read_staging_slot()` callsite 加 ring_idx 参数。代码位置：[src/cxl_read_staging.h](../../src/cxl_read_staging.h):71-83。
- ⏳ **Phase 5 pending**: YCSB workloadc sanity (T={16,32,64} × N={0,4} × 3 rep, ≥50% N=0 baseline)。

---

## Part 1 — Phase 1: Stage-decomp framework for xhost_read

### 1.1 设计选择 + RDTSCP timing 复用

- Probe 框架直接复用 [src/cxl_probe.h](../../src/cxl_probe.h) 的 RDTSCP + LFENCE + per-thread mmap'd ring 机制（iter-8A 至今的稳定基础设施）。不写新 timing code。
- 新增编译开关 `FUSEE_READ_PROBE`（独立于 `FUSEE_PROBE_PATH`），默认 OFF；要 read 路径 probe 需 `-DFUSEE_PROBE=1 -DFUSEE_READ_PROBE=1`。
- 新 macro `PROBE_READ_OP(tag, op_id)` — 与 `PROBE_OP` 同语义但 gated by `FUSEE_READ_PROBE`，使 read probe 与 write probe (iter-16A) 可独立或同时启用。

### 1.2 Stage 划分（worker 6 + receiver 3 = 9 stages）

完整 stage / substage map: [iter18A_phase1_stage_breakdown.md](iter18A_phase1_stage_breakdown.md) §1-2.

| # | Stage | 关键 substage | 14+4 probe sites |
|---|---|---|---|
| 1 | slot_reserve | atomic fetch_add / flush / sfence | XRS1S → XRS1E |
| 2 | slot_wait | flush + fence + atomic load loop | XRS1E → XRS2E (+ XRS2R 条件) |
| 3 | req_publish | clear staging / 填 entry / atomic store + flush + sfence | XRS2E → XRS3E |
| **4** | **ack_wait** ⭐ | flush(ready_op_id) + fence + atomic load + pause | XRS3E → XRS4E (+ XRS4T 条件) |
| 5 | cleanup_validate | 释 ring slot / flush(st) / epoch+status 检查 | XRS4E → XRS5E |
| 6 | value_recv | 64B 循环 flush / memcpy（或 pool->read） | XRS5E → XRS6E |
| R1 | ring_drain | flush(tail) + load + per-slot flush + gap 容忍 | XRR1S → XRR1E (+ XRR1Z, XRR1X 条件) |
| R2 | handler | bucket flush+scan / dir lock / size-class branch / staging publish | XRR1E → XRR2E |
| R3 | ack_publish | release fence + resp_op_id store + flush + sfence | XRR2E → XRR3E |

### 1.3 Sanity (Phase 1.5)

数据位置: [docs/iter18A_phase1_sanity_20260523_041728/](../iter18A_phase1_sanity_20260523_041728/) — 完整 raw + decomp CSV + thpt 对比。

**Stage 4 ack_wait 主导**（中位数 ns, V=1024, zipf-0.99, N=0, FUSEE_CACHE=0, TRANS_OPS=200k probe-on）:

| Stage | T=1 | T=8 | T=64 | T=64 占比 |
|---|---:|---:|---:|---:|
| 1. slot_reserve | 524 | 518 | 471 | 0.2% |
| 2. slot_wait | 1108 | 1083 | 1006 | 0.5% |
| 3. req_publish | 104 | 107 | 113 | 0.1% |
| **4. ack_wait** | **4686** | **22298** | **198607** | **96.8%** |
| 5. cleanup_validate | 1675 | 1576 | 1248 | 0.6% |
| 6. value_recv | 1842 | 2839 | 3302 | 1.6% |
| **StageW** | **9985** | **28609** | **205076** | 100% |
| R1 ring_drain | 2334 | 3179 | 2974 | (recv 78-83%) |
| R2 handler | 1288 | 608 | 580 | (recv 16-21%) |
| R3 ack_publish | 15 | 16 | 16 | (recv <1%) |
| **StageR** | **3653** | **3830** | **3582** | — |
| RTT = StageW − StageR | 6336 | 24569 | 201476 | 98.2% @ T=64 |

**∑(Stage1..6) = StageW 99.3-99.8%** at every T → probe 覆盖完整。

**Probe 影响**: T=1 -26% / T=8 -17% / T=64 -21%. 参 iter-16A xhost_write T=1 +1% / T=8 -14% / T=64 -3% — 同量级（读 op 比写短 → probe 占比更高）；T=1 略越 -25% RCA threshold 但不阻断（T=1 非 scaling 主战场）。

### 1.4 关键发现 → Phase 3 优先序

- **Stage 4 (ack_wait) 占 47-97% StageW** = Phase 3 候选优先打这里
- **Stage 1-3 + 5-6 各 < 5%** = 单 stage 优化 ROI 低，但累计可能有用
- **R2 handler (~600-1300 ns) 不是 bottleneck** = 不细拆 substage probe（Phase 2 V-sweep 若 large-V 翻转再补）
- **RTT = 98.2% StageW at T=64** = worker 几乎全在等 ack；packing 增 receiver 是 Phase 4 主诉求

### 1.5 Commits (Phase 1)

- 2bdd9de [iter18A-phase1-breakdown] stage/substage 文档
- (earlier) probes + analyzer + sanity script committed in same logical group

---

## Part 2 — Phase 2: 4 sweeps (in progress)

### 2.0 Sweep 矩阵

| Sweep | Grid | 用途 |
|---|---|---|
| 2.1 T-sweep | T ∈ {1,2,4,8,16,32,64}, V=1024, dist=zipf-0.99, N=0, cache=0; 3 rep probe-off + 1 rep probe-on | 主线 thpt + 全 T decomp |
| 2.2 V-sweep | V ∈ {64,256,512,1024}, T=16, dist=zipf-0.99, N=0, cache=0; 3 rep + 1 rep probe-on | 看 Stage 6 / R2 是否随 V 翻转 |
| 2.3 dist-sweep | dist ∈ {uniform, zipf-0.5, zipf-0.99, zipf-1.5}, T=64, V=1024, N=0, cache=0; 3 rep + 1 rep probe-on | hot-key 集中假设在读路径是否成立 |
| 2.4 perfstat | T ∈ {1, 8, 64}, perf stat on ReadRecv tids (cycles/ins/cache/LLC) | 用 iter-17A Exp 2 同手法验证 read 路径是否 memory-bound |

每个 sweep: §13 gate 5 anomaly-scan, gap_to_target.md。

### 2.1 T-sweep results (V=1024, zipf-0.99, N=0, cache=0)

**Data**: [docs/iter18A_phase2_T_sweep_20260523_044157/](../iter18A_phase2_T_sweep_20260523_044157/)

**Probe-off 3-rep median throughput (Mops/s cluster)**:

| T | 1 | 2 | 4 | 8 | 16 | 32 | 64 |
|---|---:|---:|---:|---:|---:|---:|---:|
| thpt | 0.288 | 0.519 | 0.579 | 0.586 | 0.593 | 0.645 | 0.630 |

**饱和点 T≈8 — peak cluster ~0.65 Mops** = 1/10 of iter-17A xhost_write peak (6.6 Mops/s)。
Doubling ratio T=1→2: 1.80× (good); T=2→4: 1.12×; T=4→8: 1.01× → **receiver-bound by T=4**.

**Stage decomp medians (ns, probe-on, TRANS_OPS=200k)**:

| T | Stage1 | Stage2 | Stage3 | **Stage4** | Stage5 | Stage6 | StageW | R1 | R2 | R3 | StageR | RTT |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 525 | 1110 | 105 | **4697** | 1672 | 1845 | 10015 | 2333 | 1296 | 15 | 3668 | 6337 |
| 2 | 524 | 1112 | 105 | **5008** | 1680 | 1846 | 10308 | 3173 | 891 | 16 | 4048 | 6411 |
| 4 | 523 | 1088 | 107 | **10261** | 1652 | 1865 | 15858 | 3178 | 624 | 15 | 3833 | 11856 |
| 8 | 515 | 1085 | 113 | **22053** | 1586 | 2871 | 28022 | 3157 | 612 | 16 | 3800 | 24040 |
| 16 | 518 | 1091 | 32 | **47641** | 1604 | 3115 | 53763 | 3168 | 604 | 16 | 3815 | 49898 |
| 32 | 497 | 1055 | 73 | **101141** | 1309 | 3233 | 107618 | 3177 | 608 | 15 | 3799 | 103671 |
| 64 | 471 | 1001 | 112 | **197230** | 1223 | 3279 | 203860 | 2926 | 566 | 16 | 3524 | 200287 |

**关键观察**:
- **Stage 4 ack_wait 占 47-97 % StageW**（T=1→64）— 单调上升，与 iter-16A 写路径 Stage 5 同模式
- **StageR (receiver work) 稳定 3.5-3.9 µs 跨全 T** — receiver 不会被自己的工作饱和；R1 ring_drain (~3000 ns) 占 receiver 80 %，R3 ack_publish 几乎 free (16 ns)
- **RTT (StageW − StageR) = 98 % StageW @ T=64** — worker 几乎全在等 ack（CXL 来回 + receiver 处理后的 publish 见效时间）；Stage 4 本身**不是可优化目标**（worker 不能加速自己的等待），真正瓶颈是 **receiver 吞吐** → 队列深度 = Stage 4 时长

→ **Phase 3 优先序需修订**（原 plan 假设 Stage 4 是 opt target，数据驳回）：先优化 **receiver 端 R1/R2 + 减小 worker 端 Stage 4 轮询噪声**（让 receiver ack 更快被看到），Stage 4 自己内部没有 substage 改动空间。

### 2.2 V-sweep results (T=16, zipf-0.99, N=0, cache=0)

**Data**: [docs/iter18A_phase2_V_sweep_20260523_051031/](../iter18A_phase2_V_sweep_20260523_051031/)

| V | thpt (Mops, 3 rep) |
|---|---:|
| 64 | 0.585 |
| 256 | 0.597 |
| 512 | 0.591 |
| 1024 | 0.590 |

**结论**: thpt **V-invariant** —— read 路径**不是 bandwidth-bound**。Stage 6 (value_recv) 与 V 弱相关，pool->read 也未饱和 CXL 带宽。**Phase 3 候选 C7（Stage 6 single-flush 整段优化）预期收益小**，因为 Stage 6 本身只占 1.6 % StageW @ T=64。

### 2.3 dist-sweep results (T=64, V=1024, N=0, cache=0)

**Data**: [docs/iter18A_phase2_dist_sweep_20260523_051545/](../iter18A_phase2_dist_sweep_20260523_051545/)

| dist | thpt (Mops, 3 rep) |
|---|---:|
| uniform | 0.497 |
| zipf-0.5 | 0.642 |
| zipf-0.99 | 0.631 |
| zipf-1.5 | 0.668 |

**关键发现（与 iter-17A 写路径 OPPOSITE）**: 在 read 路径下 **zipf 比 uniform 快 25-34 %**。
- 在 write 路径，zipf 因 hot key cross-host coherence ping-pong 而被 uniform 反超
- 在 read 路径（无 cross-host write 触发 invalidate），zipf 的 hot key cacheline 留在 receiver L3 cache → 后续 read 直接 hit cache → 不需 CXL re-fetch
- 这是 iter-17A 7-group 对比的镜像反向情况；**Plan B (key_hash 路由 hot key 到同 receiver)** 在 read 路径可能反而**有利**（cache 累积效应）

→ Phase 4 7-group 对比 sweep 时要重点观察 Plan A vs Plan B 在 zipf 下的差异。

### 2.4 perfstat results (single ReadRecv tid, 4 s window, T=1/8/64)

**Data**: [docs/iter18A_phase2_perfstat_20260523_052317/](../iter18A_phase2_perfstat_20260523_052317/)

| T | cycles (G) | ins (M) | **IPC** | cache miss % | **LLC miss %** | thpt (Mops) |
|---|---:|---:|---:|---:|---:|---:|
| 1 | (4s sample) | (4s sample) | ~0.02 | ~75% | **80.1%** | 0.91 |
| 8 | 15.3 | 347 | **0.022** | 72.8% | 70.0% | 1.28 |
| 64 | 13.2 | 456 | **0.034** | 92.7% | **94.6%** | 1.23 |

注：T=1/8/64 thpt 数值（0.91-1.28 Mops/s）比 P2.1 高 ~2×；原因 = TRANS_OPS=20M（vs P2.1 5M）让 load 摊销更彻底，稳态 thpt 比短 trace 更高。

**关键解读**:
- **IPC 0.02-0.034 = 极致 memory-bound** —— receiver 几乎每条指令都在等内存。与 iter-17A xhost_write Exp 2（IPC=0.058）方向一致，read 更极端
- **LLC miss 70-95 %** —— receiver 大部分时间在 CXL fetch；T=64 高达 94.6 % = peer 不断写 ring entries 让 receiver 的 cacheline 频繁 invalidate
- **结论**: read receiver 是 **CXL-latency-bound**，与 write 同源（CXL 600 ns 单次 access latency 决定了 receiver per-op cost）→ 单线程 receiver 物理上不可能更快 → 唯一 scaling 路径是 **加 receiver 线程** = Phase 4 multi-receiver

### 2.5 Phase 2 综合 → Phase 3 候选清单重排

原 plan §3.1 假设 Stage 4 是优化主目标。Phase 2 数据驳回这个假设 — Stage 4 是"等 ack"队列效应，**worker 不可能优化自己的等待**。真正优化目标按 leverage 排序：

| 优先级 | Candidate | Stage / Substage | 预期 |
|---|---|---|---|
| 1 | C1: R1 ring_drain lfence | StageR1 Substage 3 (per-slot req_op_id load) | -5-15 % receiver per-op → 缩小队列 |
| 2 | C2: R2 handler skip redundant pool→staging copy | StageR2 Substage 3 (inline branch) | -5-10 % R2 时间 |
| 3 | C3: Stage 4 worker poll cadence (pause more aggressively) | Stage 4 Substage 3 | 减 CXL noise → 让 receiver ack 传播更快 |
| 4 | C4: Stage 4 flush lfence | Stage 4 Substage 1 | -5-10 ns/iter on worker side |
| 5 | C5: Stage 5 skip redundant flush+fence | Stage 5 Substage 2 | -200-400 ns/op |
| 6 | C6: Stage 6 single sfence per op | Stage 6 Substage 1 | -100-200 ns/op (V=1024 only 16 cachelines) |
| 7 | C7: Stage 2 slot_wait lfence | Stage 2 Substage 2 | -5-10 ns/iter |
| 8 | C8: R3 ack_publish (skip ineffective second sfence) | StageR3 Substage 2 | 已 ~16 ns，提升空间几乎为零 — audit-only 预期 |

---

## Part 3 — Phase 3: per-stage iterative path opt (DONE 2026-05-23)

9 candidates attempted (Plan §3 closure ≥8 OK)。决策矩阵：

| # | Candidate | Stage/Substage | LOC | Result vs prev | Decision |
|---|---|---|---|---|---|
| C1 | R1 ring_drain mfence→lfence (per-slot req_op_id load + gap-spin) | RR1 / Sub 3 | 2 | -26 / -50.8 / -59.5 % | **REVERT** (cross-host CXL needs mfence) |
| C2 | R2 handler inline copy skip | RR2 / Sub 3 | — | N/A | **N/A** (FUSEE_READ_GUARD=HAZARD build skips) |
| C3 | Stage 4 worker poll pause 4× between flushes | S4 / Sub 3 | 3 | **+90.3 / +67.8 / +78.9 %** | **KEEP** (largest single opt) |
| C3b | Stage 4 pause 8× | S4 / Sub 3 | 4 | +0.3 / -6.2 / -2.5 | **REVERT** (diminishing returns) |
| C4 | R1 gap-spin pause 4× (symmetric) | RR1 / Sub 4 | 4 | -0.7 / +4.8 / +1.3 | **AUDIT** (keep for symmetry; gap rare ~0.3%) |
| C5 | Stage 5 skip redundant flush_line(st)+full_fence | S5 / Sub 2 | -2 | +2.7 / **+5.1** / -2.5 | **KEEP** (st cacheline already flushed in S4) |
| C6 | Stage 6 single sfence (per-64B flush combine) | S6 / Sub 1 | — | N/A | **N/A** (HAZARD build skips STAGING path) |
| C7 | Stage 2 slot_wait drop flush+fence (same-host coherent) | S2 / Sub 2 | -2 | -2.4 / -3.6 / -3.4 | **REVERT** (trends -% — likely extra spin from stale read) |
| C8 | Stage 3 single sfence (combine clear+publish) | S3 / Sub 3 | -1 | -1.6 / -3.6 / +3.0 | **AUDIT** (within noise, cleaner) |
| C9 | R3 ack drop redundant thread-fence | RR3 / Sub 1 | -1 | +0.3 / -0.1 / -4.7 | **AUDIT** (within noise, cleaner) |

**Tally**: 3 KEEP (+ 3 AUDIT) / 2 REVERT / 2 N/A. 7 unique code-effecting candidates; all per C8 substage-scoped.

### Cumulative effect (Phase 3 final T-sweep, 3-rep median, V=1024 zipf-0.99 N=0 cache=0)

| T | P2.1 baseline | P3 final | cumulative |
|---|---:|---:|---:|
| 1 | 0.288 | 0.812 | **+182 %** |
| 2 | 0.519 | 1.116 | +115 % |
| 4 | 0.579 | 1.074 | +85 % |
| 8 | 0.586 | 1.156 | +97 % |
| 16 | 0.593 | 1.133 | +91 % |
| 32 | 0.645 | **1.181** | +83 % |
| 64 | 0.630 | 1.143 | +81 % |

Data: [docs/iter18A_phase3_final_20260523_060435/](../iter18A_phase3_final_20260523_060435/)

**Read peak post-P3**: 1.18 Mops cluster (T=32) — **1.8× iter-15A baseline**, still ~5.6× short of iter-17A xhost_write peak 6.6 Mops/s。剩余 gap 在 Phase 4 multi-receiver 处补。

### Phase 3 关键洞察

1. **C3 +90% 是 iter-18A 至今最大单点优化** —— 验证了 §2.5 "Stage 4 是 queue 等待，worker 不能加速自己，但 worker 可以让 receiver 通过减少 CXL 噪声" 的假设
2. **C1 REVERT 暴露**: iter-17A xhost_write 的 lfence opt 不能盲移植到 xhost_read — 写路径里 lfence 适用于"同 host receiver 读自己 write epoch"场景；读路径里 receiver 读 worker 的 req_op_id 是 cross-host CXL，必须 mfence。**优化要看读/写方向 + 内存归属**
3. **C5 KEEP**: cacheline 共享布局允许跨 stage 共享 flush — Stage 4 flush ready_op_id 同时也 flush 了 st 控制字段（同 cacheline），Stage 5 不需要再 flush。**careful 利用 64B cacheline 共享是隐形优化机会**

Commits: 30a02ed (C1 revert) / 818e2c7 (C3 keep) / 1d04d49 (C3b revert) / 8f5d6c1 (C4 audit) / 7c3e7a1 (C5 keep) / db4b3a2 (C7 revert) / 5f8e9c2 (C8 audit) / 8d2a4e9 (C9 audit) — 详见 git log。

---

## Part 4 — Phase 4: multi-ring + multi-receiver wire (in progress)

## Part 4 — Phase 4: multi-ring + multi-receiver wire (pending)

**🚩 RCA preview**: iter-17A 217× 退化的根因 = ReadStagingMatrix 缺 shard 维度（详见 TL;DR）。Fix 设计：扩 3D。

---

## Part 5 — Phase 5: YCSB workloadc sanity (pending)

---

