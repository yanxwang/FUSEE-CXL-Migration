# Protocol A 实现 + 优化的 5 分钟介绍 (中文 script)

> 演讲脚本，目标时长 5 分钟 ± 30 秒。中文正常语速约 250-300 字/分钟，
> 全文约 1500 字。每段前的 `[~Xs]` 是建议时长。
>
> 适合场景：supervisor present 开头铺垫 / 项目年度汇报 / 新成员上手介绍。

---

## [~30s] 开场

各位老师好，今天我用 5 分钟左右介绍一下我们项目里 protocol A 的核心实现，以及这一年迭代过程中**几个关键优化分别解决了什么问题**。

Protocol A 的设计目标是 **strict-A linearizable**，意思是任何 reader 在任何时刻看到的 key-value 状态，都必须等价于某个全局串行执行的结果。这是分布式系统里最严格的一致性模型，**在 CXL 跨 host 共享内存上实现它的代价也是最大的**。今天讲的所有优化，本质都是在和这个代价做斗争。

---

## [~45s] 基础架构

A 的实现有 3 个 pillar。

**第一个是 sharded ownership**：每个 key 通过 `hash(K)` 路由到固定的 owner host，**只有 owner host 能写**。这一步就把跨 host 写者的同步从"全局共享 mutex"降级到"消息传递"，避免了 protocol C 那种 LFM 跨 host 锁竞争的瓶颈。

**第二个是 CXL ring 通信**：每对 host 之间有 3 条独立的 SPSC ring —— WriteRing 跑写请求、ReadRing 跑跨 host 读、InvalRing 跑缓存失效广播。每条 ring 有专门的 receiver 线程消费。

**第三个是两层 DRAM cache**：worker 私有的 TLS L1 在最上层，同 host worker 共享的 KvCachePool L2 在中间，CXL 上的 authoritative bucket array 是真理之源。

跨 host 一次写涉及 3 个线程（worker、host 1 WriteReceiver、host 0 InvalReceiver）协作，是 protocol C 单一 worker 模型的 3 倍复杂度 —— 但这是 strict-A 的入场券。

---

## [~50s] 优化 1：3-ring 分离（iter-9A，解决死锁）

**第一个关键优化是 iter-9A 的 3-ring 分离**。

之前 write forward 和 invalidate 共用一条 ring。问题在哪？WriteReceiver 处理一个跨 host 写时，自己要在 owner 这边广播 invalidate 给其他 sharer host。如果 invalidate 走同一条 ring + 同一个 consumer 线程，writer 在等 receiver 处理、receiver 又要等自己继续 —— 形成**循环等待死锁**。

**iter-4A-redo 就栽过这个坑**：workload-a 跑到 50k 操作就整个 cluster 卡死。

解法是把 InvalRing 抽出来独立成一条 channel，配 InvalReceiver 单独的线程消费 —— **打破死锁环**。同时引入了 6 个 CPU-pinned 的 system 线程：3 个 receiver always-on，3 个 sender opt-in。还引入了 ForwardStaging CXL arena —— value bytes 不再 inline 在 128-B ring entry 上，而是单独的 staging slot，**满足 C2 不变性，同时支持变长 value**。

---

## [~60s] 优化 2：TLS L1（iter-10A，解决 MESI ping-pong）

**第二个、也是收益最大的优化是 iter-10A 的 TLS L1 cache**。

iter-9A redo Phase 3 path_decomp 量化出在 workload-A KV=1024 T=64 这个场景下，读路径的 R1 stage 平均 **7 微秒**。我们查了一下：`KvCachePool` 的 entry 大小是 17 个 cacheline，其中 16 个是 1024-B 的 value_bytes 自身。这块 entry 是 `MAP_SHARED` 跨 64 个同 host worker 的。

在 Zipf 热点 + 高并发场景下，writer 频繁写这块 entry 会触发 **MESI invalidate**，所有 reader core 的本地副本失效，下次读时每个 reader 都要跨 core 重新拉 16 个 cacheline —— **这就是 MESI ping-pong**。

解法很简单也很经典：**每个 worker 分一份私有 DRAM 缓存**，不 MAP_SHARED，**别的 core 永远碰不到**。把热 key 复制进去。读热 key 时不再碰共享内存，只读私有 DRAM —— 80 纳秒返回，**比之前 7 微秒快 87 倍**。

一致性怎么保？每个 bucket 加一个 atomic `bucket_epoch` 计数器。Writer 写时 bump 一次，TLS reader 比较 `observed_epoch` vs `bucket_epoch`，不匹配就 fall through 到 L2 重新取。**关键 trade**：以前每次读要跨 core 拉 16 个 cacheline，现在只跨 core 读 1 个 epoch cacheline，**MESI 流量降到 1/16**。

---

## [~40s] 优化 3：seqlock CAS cache_pool（iter-10A，解 reader-writer 互锁）

**iter-10A 同一个 iter 还做了 seqlock CAS cache_pool**。

原来 L2 entry 用 per-bucket spinlock 保护，**reader 也要抢锁**。在 hot bucket 上，64 个 reader 排队抢同一把锁，吞吐被严重限制。

改成 seqlock 模式：每个 entry 加 atomic `seq` counter，writer 用 CAS 把 seq 从偶数翻到奇数表示进入临界区，写完 store 回偶数+1。**reader 完全不抢锁**：load seq → memcpy fields → 再 load seq；如果两次 seq 不同，说明 mid-update，retry，最多 8 次。

**Writer 不再阻塞 reader**，reader 即使遇到 concurrent write，最坏也就是 retry 几次，绝大多数情况下一次 pass。

---

## [~50s] 优化 4：forwarder-pool-direct（iter-11A，省一次 CXL roundtrip）

**第四个是 iter-11A Phase 1 的 forwarder-pool-direct**。

iter-10A 的 cross-host miss 读路径是 **3 个 CXL roundtrip**：worker 发 ReadRing 请求 → owner WriteReceiver 处理 + 写 ACK → worker 收到 ACK 后再 `pool->read` 拉 value bytes。

我们改成：owner 的 `read_handler` 直接把 value bytes 写到 CXL 上**新加的 ReadStaging arena**（per-(req_host, owner, slot_idx)），然后 publish 一个 `ready_op_id` 信号；worker poll 到 ready_op_id 后**直接从 staging memcpy 取值**，**省掉第三个 roundtrip**。

但同时引入了**新的一致性风险**：staging slot 是复用的，concurrent invalidate 可能让 reader 缓存到 owner 旧视角下的 stale value。

我们加了 **C13 epoch validation** 来防：reader 在发送请求**之前**记下当前 `my_epoch_at_send`，owner 在 staging slot 里 tag 自己 lookup 时的 `lookup_epoch`，reader 收到时校验 `lookup_epoch >= my_epoch_at_send`，**不通过就拒绝缓存并 retry**。这保留了 strict-A。

---

## [~30s] 还没解决的问题

但 iter-11A 的 sweep 也暴露了新问题：**13 个 cell 出现 bimodal failure**，集中在小 KV 高并发的 Zipf workload。我们高度怀疑是 forwarder-pool-direct 的 epoch retry storm 在 hot bucket 上失稳。这是 iter-12A 的第一任务。

另外 W10 stage —— `cache_pool_insert` + `bucket_epoch` bump —— 还是 6.22 µs mean，**依然是写路径 dominant**，需要 RCU 或 hot-key replication 进一步改善。

跨 host invalidate 的并行化也试过（iter-11A Phase 2），但 w_p99 出现 26× regression 被 revert，需要重设计成 bucket-affinity batching。

---

## [~25s] 总结

总结一下，**Protocol A 这一年优化的主轴是 "把跨 core 共享数据的代价降下来"**：

- 3-ring 分离解决了 protocol 内部 deadlock；
- TLS L1 把读路径上的 MESI ping-pong 移除；
- seqlock CAS 让 writer 不阻塞 reader；
- forwarder-pool-direct 把跨 host 读从 3 个 roundtrip 降到 2 个。

但 strict-A linearizability 的代价依然在 —— 每次写都要经历 **W9 slot CoW commit + 每个 sharer 的 cache_pool stale + W10 directory state reset** 这 3 个 linearization point —— 这是 protocol 设计本身决定的，不是优化能消除的。

所以 protocol A 当前 peak 13 Mops/s 跟 protocol C 在大 KV 上的 16-17 Mops/s 仍有差距 —— 这就是 strict-A vs LRC 的 **correctness budget**。我们花 µs 换"读者永远看不到 order 违反"的保证。

谢谢。

---

# 配套数据卡（讲解时随时可调出来引用）

| 优化 | 目标 stage | before | after | 提升 |
|---|---|---:|---:|---:|
| 3-ring 分离 | deadlock | 整集群卡死 | 不死锁 | qualitative |
| TLS L1 (hit path) | R1 | 7.16 µs | 0.08 µs | **~87×** |
| seqlock CAS | L2 read under contention | spinlock 排队 | retry-only | reader/writer 解耦 |
| forwarder-pool-direct | cross-host read | 3 roundtrips | 2 roundtrips | ~33% less CXL traffic |

| Path | 当前 typical latency |
|---|---:|
| A read TLS L1 hit | 80 ns |
| A read L2 hit | 7 µs |
| A read owner-self miss | 10 µs |
| A read cross-host miss | 22 µs |
| A write (no inval) | ~16 µs |
| A write (with inval) | ~22 µs |
| **A write W10 (dominant)** | **6.22 µs mean** |

| 当前状态 | 数字 |
|---|---:|
| YCSB target | 20 Mops/s |
| A iter-11A peak (workload-a) | 13.69 Mops/s |
| iter-11A bimodal cells | 13 (>8 gate-12) |
| 0 of 5 workloads | hit target |
