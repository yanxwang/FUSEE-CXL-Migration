# iter-17A Multi-Ring + Multi-Receiver Scaling — RAP

**Per `docs/design_goals.md` §XIII Reviewer Attack Process**

**Designs under review**: see
[iter17A_multi_ring_receiver_scaling_design.md](iter17A_multi_ring_receiver_scaling_design.md)

---

## STATE

把 W/R/I 三类 ring 从 (src,dst) 二维矩阵扩到 (src,dst,shard) 三维，
每类提供 `actual_shards = ceil(T/N)` 个 ring shard。Worker 按
`client_id / block_size` 静态分配到 shard（Plan A）；Plan B 后续用
`key_hash % shards`。每个 ring shard 由一个 receiver thread drain，
CPU pinning 起点动态 `start_cpu = T`；当 CPU 不够时（仅 T=64 触发）
进入软件 packing —— 单个 receiver thread cooperatively 轮询多个
ring shard，避免 OS time-slice。Sender thread scaffold 整组移除。

预期：在 xhost_write zipf-0.99 上，T=64 throughput 从 ~1 Mops/s/host
提升 3-5× 至 3-5 Mops/s/host。

---

## ATTACK VECTORS

### A1. PERFORMANCE — Plan A hot-bucket ping-pong 会否吃掉所有 scaling

**Attack**: Zipf-0.99 下，top 1% 的 key 占 50% 流量。Plan A 静态分片
按 worker_id 路由 → 一个 hot key 来自不同 worker 时落到不同 receiver
shard。每个 receiver 写同一个 bucket cacheline，跨 same-host MOESI 不
直接 ping-pong（同 host 内 coherent），但 `slot_directory_lock`
(pthread_spinlock_t) 会成为强串行化点 —— N 个 receiver 物理上
不能 parallel 处理同 hot bucket。

**Counterevidence**: 
- Hot key 仅占 ~50% 流量，其余 50% 是长尾 key，长尾 receiver 之间
  无 lock 竞争 → N receiver 至少在长尾上线性 scale
- 即使 hot key 完全串行（worst case），整体 throughput 上限 ≈
  N × tail_thpt + 1 × hot_thpt，仍显著优于单 receiver
- Plan B 作 alternative，把 hot key 强制路由到单 receiver，可直接
  对比

**Verdict**: ⚠️ 已知 risk，**不阻断本方案**。Plan A 是 baseline，B
作 alternative，实测三方对比。

---

### A2. PERFORMANCE — packing 在 T=64 把单 ring 吞吐摊薄

**Attack**: T=64 N=4 时，write thread budget=8 但 shards=16，每 thread
负担 2 ring；read/inval budget=7 但 shards=16，每 thread 平均 ~2.3
ring（5×2+2×3）。一个 thread 在 2-3 ring 之间 round-robin 轮询，
**单 ring 的等待 latency 增加**（被其他 ring 的 drain 时间挤掉）。

**理论上限**:
- 单 receiver thpt ≈ 1 Mops/s（iter-17A 路径优化后测出的上限）
- T=64 N=4: write 8 thread × 1 Mops = **8 Mops/s 理论**，但每 thread
  跑 2 ring 摊薄 → 实际可能 4-6 Mops/s
- T=64 N=8: write 8 thread × 1 ring/thread = 完全 unpacked → 接近 8 Mops/s

**Counterevidence**: 这只是 22-core hard ceiling 的自然表现，不是
设计缺陷。如要超越 22 core ceiling，需要增加 receiver pool（牺牲
worker count）或加节点。

**Verdict**: ✓ 不阻断 —— packing 是 CPU 受限下的最优选择（cooperative
轮询 vs OS time-slice）。N=8 比 N=4 在 T=64 packing 更轻，测试矩阵
两个都跑能验证 trade-off。

---

### A3. CORRECTNESS — Plan A 跨 ring 的 ordering invariant

**Attack**: 同一个 key 的多个 op 来自不同 worker 时，按 Plan A 走
不同 ring → 不同 receiver。同 host 内 receiver 都加 LFM 锁，OK；
**但跨 receiver 的全局 ordering 不保证** —— Worker 1 的 write 在 ring 0，
Worker 2 的 read 在 ring 1，两个 receiver 不同时间处理，外部观察的
顺序可能与 program order 不一致。

**协议本身要求**: §I9 strict-A linearizability —— 一个 op 在 receiver
ack 之后对所有 host 立即可见。多 receiver 不破坏这一点：每个 op
仍由单一 receiver 处理，加锁 + bucket flush + 单写 invariant 仍然
成立。跨 worker 的 ordering 在原协议里就没保证（worker 1 的 op N
和 worker 2 的 op M 可以任意顺序到达 owner），多 receiver 只是把
这种"已有的非确定性"放大。

**Counterevidence**:
- 协议层面允许这种非确定性
- LRC/sharer_bitmap 等 directory 状态依然在 owner host 集中维护，
  per-slot LFM 锁保证 atomic 转换
- Worker 不依赖跨 receiver 的 ordering（forward_write_direct 等
  ack 是 per-op）

**Verdict**: ✓ Correctness preserved。Test by hash-diff battery
(unchanged) + workload-a 0-error verification。

---

### A4. CORRECTNESS — N receiver 同时写 directory 的锁竞争

**Attack**: `slot_directory_lock` 是 `pthread_spinlock_t`（host-local
memory，host 内 multi-core MOESI coherent）。N 个 receiver 在
不同 core，竞争同一个 spinlock 时：
- 锁是 host-local，没有跨 host coherence issue
- pthread_spinlock 在 contended 情况下是 CAS loop，cacheline 在 N
  个 core 间 ping-pong
- 但同 host 内 MOESI 自动处理，没有 correctness issue，只是
  performance

**Verdict**: ✓ 跟 iter-3A per-slot LFM 优化相同的 trade-off —— 锁
争用是 perf cost，不是 correctness 问题。Hot bucket 会被 lock 串
行化（同 A1 attack），实测后判断是否需要 Plan B。

---

### A5. COMPLEXITY — N×3 个 thread 启动 + stop 的同步

**Attack**: 当前单 receiver 用 `write_receiver_stop_.store(false)`
+ `pthread_create(&write_receiver_thread_)`。改成 N 个 thread 时：
- 需要 `std::vector<pthread_t> write_receivers_;` 数组
- Stop signal 共享 atomic_bool（不变）
- Join 时遍历 vector
- attach() 时 init barrier 要等待 N×3 个 thread 都 ready

**Mitigation**:
- 简单的 vector 管理，无 race
- Stop signal 单 bool 仍然有效（store release / load acquire）
- Init barrier 由 host_id=0 publish init_done bit 0 触发，**不依赖
  receiver thread 数量**（worker 才依赖此 bit）
- Test by simple smoke (T=8, N=4) → 验证 spawn + stop 路径

**Verdict**: ✓ Complexity 可控。Phase 3 实施重点。

---

### A6. IMPLEMENTATION FEASIBILITY — 现有 `wr_->rings[*][*]` 全代码 grep

**Attack**: 现有代码 `cxl_kv_ops_A.cc` 多处直接索引 `wr_->rings[a][b]`，
改 3D 后所有访问点都要加 ring_idx。漏一处 → 编译错误。但有些
间接访问可能逃过编译：`flush_line(&wr_->rings[a][b].tail)` 形式
看起来仍然合法（取 tail 地址），其实是取了 shard[0] 的 tail —— 
**silent bug**。

**Mitigation**:
- 强制 grep `wr_->rings\[` / `rr_->rings\[` / `ir_->rings\[`，全部
  显式 audit
- 改写 access helper：`write_ring_shard(src, dst, idx)` 等
  inline functions，强制所有 access 走 helper，避免裸下标
- 留 deprecation marker: 删除二维 access 形式

**Verdict**: ✓ 严格 grep + helper function 是充分 mitigation。

---

### A7. PRIOR ART — Multi-receiver sharded design 是否新颖

**Attack/CounterAtt**: 这种 worker-sharded ring + dedicated receiver
模式在网络栈（DPDK RX queue per core, Linux kernel RPS/RFS, Mellanox
RDMA QP sharding）和数据库系统（Scylla / Seastar 的 per-core
shared-nothing 架构）里非常成熟。FUSEE protocol A 之前因为以
"单 receiver 简化协议状态" 起步，现在 scale 到 multi-receiver 是
自然演化，不是新发明。

**Risk**: 我们没有完全复刻"per-core sharded everything"模式
（bucket 仍由 owner host 集中，多 receiver 仍共享 bucket lock）。
跟 Seastar shared-nothing 不同。但这是 protocol A 自身的设计选择，
不是 multi-receiver 模式的问题。

**Verdict**: ✓ 设计模式有充分前置案例。

---

### A8. GENERALITY — N 的硬上限 kRingShardsMax=16 是否限制

**Attack**: N=4 + T=64 → 16 shards，触顶 `kRingShardsMax=16`。如果
将来想 N=2 + T=64 → 32 shards，会编译错。

**Mitigation**:
- T=64 是当前 benchmark 上限，kRingShardsMax=16 覆盖现需求
- 真正 scale 到更大 T 时同步调整 kRingShardsMax（编译期常量，
  零运行时 cost）
- 或者直接用 32 作上限（CXL memory 充足，~16MB 额外不算事）

**Verdict**: ✓ kRingShardsMax = 16 起步，必要时改 32。

---

### A9. PERFORMANCE — Cross-host CXL bandwidth 是真的 bottleneck？

**Attack**: 即便单 receiver 不再是瓶颈，**CXL link bandwidth** 才是
真正的 hard limit。iter-15A M1 测得单 host CXL write 12.5 GB/s，
双 host 25 GB/s。每个 xhost_write op 写 ~1.1 KB（value 1024B +
WriteEntry 128B + ack）→ 25 GB/s / 1.1KB ≈ **23 Mops/s 集群
理论上限**。

**Counterevidence**: 23 Mops/s 集群 = ~11 Mops/s/host，仍然显著
高于当前 1 Mops/s/host。N=4 + Plan A 估计 3-5 Mops/s/host 在 CXL
带宽 limit 之下，scaling 空间是真实的。CXL 真成瓶颈时再做 W3 small
inline 优化。

**Verdict**: ✓ 在 CXL 带宽限制内有充分 scaling 空间。

---

### A10. COMPLEXITY — Packed receiver loop 的 backpressure / fairness

**Attack**: 一个 receiver 在 2-3 ring 间轮询，假设 ring 0 持续有
负载，ring 1 间歇有 op。轮询逻辑必须保证 ring 1 不被 starve（一直
处理 ring 0）。

**Mitigation**:
- `drain_some` 每次最多 drain N 个 op（cap）就让出，确保 ring 间
  fairness
- 实现 round-robin index，绝不 favor 某一个 ring
- 即便 starve 也是同 thread 自己的问题，不会影响其他 thread

**Verdict**: ✓ drain_some 限流 + RR 切换是标准 fairness pattern。

---

## ABLATION CHECK

如果一个一个去掉本设计的某个 element，看 net effect:

| 去掉 | 影响 |
|---|---|
| Multi-shard ring（只多 receiver 共享 1 ring） | Worker ring tail 仍 64 worker 争用，单 ring 64 slot 不够 → 排队 → 没有 scaling |
| Multi-receiver（多 shard ring 但单 receiver 轮询全部） | Receiver 仍是单线程瓶颈 → 没有 scaling |
| CPU pinning（让 OS 调度 receiver）| 受 worker 干扰 + 跨 NUMA migration → tail latency 抖动严重 |
| Sender 移除（保留 iter-9A scaffold） | 占用 CPU pin 槽位，挤占 receiver core，但 hot path 无用 |
| Plan A 静态分片（改成随机/动态） | Worker 不再有固定 ring → ring tail cacheline 全员争用 |
| 软件 packing（用 OS time-slice） | T=64 case tail latency 严重退化 |

每个 element 都是 load-bearing。设计无冗余。

---

## PRIOR ART CHECK

| 设计 | 我们方案 | Prior art | 差异 |
|---|---|---|---|
| Sharded ring | rings[src][dst][shard] | DPDK RSS 多 RX queue | DPDK 每 core 自己 NIC queue，硬件路由；我们软件路由 |
| Per-CPU receiver | thread pin → ring shard | Linux kernel RPS, Mellanox RDMA QP | 我们额外增加 packing 处理 CPU 不够的边界 |
| Worker→ring routing | static modulo (Plan A) | DPDK flow-director, Seastar | 我们简化为 client_id 路由，无 flow steering |
| Cooperative ring polling | drain_some per ring round-robin | epoll multi-fd polling | 模式一致 |

我们的方案是 prior art 的合理组合，没有 unprecedented 的危险尝试。

---

## VERDICT

**Approved with caveats**:
- ✓ Correctness preserved（A3/A4）
- ✓ Performance scaling 有真实空间（A1 worst case 仍优于 baseline；
  A2 packing 是 CPU-limit 自然结果；A9 CXL 带宽不是 immediate
  bottleneck）
- ✓ Complexity 可控（A5/A6 有清晰 mitigation）
- ⚠️ Plan A 在 Zipf hot bucket 上 receiver-side lock 竞争是已知
  trade-off — Plan B 作 alternative 实测对比，**不阻断本设计先实施**

**Open after first results**:
- Plan A vs Plan B winner 取决于 workload distribution
- N=4 vs N=8 在 T=64 packing trade-off

---

## DECISION

**进入 Phase 1 实施**，按 [设计文档 §10](iter17A_multi_ring_receiver_scaling_design.md#10-实施分阶段)
分阶段推进：

1. Phase 1: 3D ring matrix 数据结构 + ring access helper
2. Phase 2: Worker 端 routing (Plan A)
3. Phase 3: Receiver packed loop + CPU pinning + multi-thread spawn
4. Phase 4: Sender thread removal
5. Phase 5: Plan A smoke + full sweep (A4 + A8 × T=1..64 × 3 reps)
6. Plan B impl + sweep (B4 + B8)
7. Winner pick → 完整 YCSB scaling

每个 phase 通过 smoke 验证再进下一个 phase。Phase 1+2 不破坏现有
路径（N=1 退化等于 baseline）。
