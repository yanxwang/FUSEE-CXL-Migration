# Protocol A 实现 + 优化的 5 分钟介绍 (中文 script)

> 演讲脚本，目标时长 5 分钟。中文技术演讲常态语速约 300-350 字/分钟，
> 全文约 1500 字 + 必要英文术语。各段前 `[~Xs]` 是建议时长。
>
> 适合：supervisor present 开头铺垫 / 项目年度汇报 / 新成员上手。

---

## [~25s] 开场

老师好，我用 5 分钟介绍 protocol A 的核心实现，以及这一年迭代下来**几个关键优化分别解决了什么问题**。

Protocol A 是 **strict-A linearizable** 的设计 —— 任何 reader 看到的状态都必须等价于一个全局串行执行。这是分布式系统里最严格的一致性，**在 CXL 跨 host 共享内存上做这个，代价非常大**。今天讲的所有优化，本质都是在和这个代价做斗争。

---

## [~35s] 基础架构（3 个 pillar）

A 有 3 个 pillar：

第一，**sharded ownership**：每个 key 哈希到固定的 owner host，**只有 owner 能写**。把跨 host 写者同步从"全局锁"降级为"消息传递"，避免 protocol C 那种全局 LFM lock 瓶颈。

第二，**CXL ring 通信**：每对 host 之间 3 条独立 SPSC ring —— WriteRing 写、ReadRing 跨 host 读、InvalRing 缓存失效广播，每条 ring 有专属 receiver 线程。

第三，**两层 DRAM cache**：worker 私有 TLS L1，同 host 共享 KvCachePool L2，加上 CXL 真理之源。

一次跨 host 写涉及 3 个线程协作 —— **是 protocol C 单 worker 模型的 3 倍复杂度**。这是 strict-A 的入场券。

---

## [~40s] 优化 1：3-ring 分离（iter-9A，解死锁）

第一个关键优化是 iter-9A 的 **3-ring 分离**。

之前 forward 和 invalidate 共用一条 ring。问题是 WriteReceiver 处理写时自己也要广播 invalidate；如果共用 channel，writer 等 receiver、receiver 又等自己，**循环依赖死锁**。

iter-4A-redo 就栽过：workload-a 跑 50k 操作整个 cluster 卡死。

解法：InvalRing 独立成 channel，InvalReceiver 单独线程消费，**打破死锁环**。同时引入 6 个 CPU-pinned 系统线程 + ForwardStaging CXL arena，value bytes 从 ring entry 里挪出来，**满足 C2 不变性 + 支持变长 value**。

---

## [~50s] 优化 2：TLS L1（iter-10A，解 MESI ping-pong）

**收益最大的优化**是 iter-10A 的 TLS L1 cache。

iter-9A redo 测出 workload-A KV=1024 T=64 读路径 R1 stage **7 µs**。原因是 `KvCachePool` entry 是 17 个 cacheline，1024-B value_bytes 占 16 个；entry 是 `MAP_SHARED` 跨 64 worker。Zipf 热点 + 高并发下，writer 写 entry 触发 **MESI invalidate**，64 个 reader 每次都跨 core 重拉 16 个 cacheline —— **这就是 MESI ping-pong**。

解法：每个 worker 一份**私有 DRAM 缓存**，别的 core 永远碰不到。热 key 复制进去，读热 key 不再碰共享内存，**80 纳秒返回，比 7 µs 快 87 倍**。

一致性靠 atomic `bucket_epoch`：writer 写时 bump，TLS reader 比较 `observed_epoch`，不匹配就 fall through。**关键 trade-off：MESI 流量从 16 cacheline / 读降到 1 cacheline / 读**。

---

## [~35s] 优化 3：seqlock CAS cache_pool（iter-10A，解 reader-writer 互锁）

iter-10A 同时做了 **seqlock CAS cache_pool**。

原来 L2 entry 用 per-bucket spinlock，**reader 也要抢锁**。hot bucket 上 64 个 reader 排队抢同一把锁，吞吐被限制。

改成 seqlock：每个 entry 加 atomic `seq`，writer CAS 偶数→奇数→偶数+1，**reader 不抢锁，只是 load seq、memcpy、再 load seq**，两次不一致就 retry。

**Writer 不再阻塞 reader**，reader 最坏 retry 几次。

---

## [~45s] 优化 4：forwarder-pool-direct（iter-11A，省 1 个 CXL roundtrip）

iter-11A Phase 1 的 **forwarder-pool-direct**。

iter-10A cross-host miss 读是 **3 个 CXL roundtrip**：worker 发请求 → owner 发 ACK → worker 再 pool->read 取 value。

改成：owner 的 read_handler **直接把 value bytes 写到新的 ReadStaging arena**，publish `ready_op_id`；reader poll 到信号后**直接 memcpy from staging**，**省掉第三次 roundtrip**。

但引入了新风险 —— staging 是复用 slot，concurrent invalidate 可能让 reader 缓存到 stale 值。我们加 **C13 epoch validation**：reader 发请求前记下 `my_epoch_at_send`，owner 在 staging 里 tag `lookup_epoch`，reader 校验 `lookup_epoch >= my_epoch_at_send`，**不通过就拒绝缓存并 retry**。保留 strict-A。

---

## [~25s] 还没解决的问题

iter-11A sweep 暴露了 **13 个 bimodal cell**，集中在小 KV 高并发 Zipf workload，怀疑是 forwarder-pool-direct 的 epoch retry storm 失稳。iter-12A 第一任务。

**W10 stage 还是 6.22 µs**，写路径 dominant，需要 RCU 或 hot-key replication。

跨 host invalidate 并行化试过（iter-11A Phase 2），w_p99 26× regression 被 revert，需要 bucket-affinity batching 重设计。

---

## [~20s] 总结

Protocol A 这一年优化的主轴是 **把跨 core 共享数据的代价降下来**：3-ring 分离解死锁、TLS L1 解 MESI、seqlock CAS 让 writer 不阻塞 reader、forwarder-pool-direct 省 CXL roundtrip。

**但 strict-A 的代价依然在** —— 每次写都要 3 个 linearization point。Protocol A 当前 peak **13 Mops/s** 跟 protocol C 大 KV 上 **16-17 Mops/s** 仍有差距 —— 这就是 strict-A vs LRC 的 **correctness budget**：我们花 µs 换"读者永远看不到 order 违反"的保证。

谢谢。

---

# 配套数据卡（Q&A 时随时引用）

| 优化 | 解决什么 | 关键数字 |
|---|---|---|
| 3-ring 分离 | forward + invalidate 共用 channel → 死锁 | iter-4A-redo workload-a 50k ops 卡死 → 不死锁 |
| TLS L1 | 64-core 抢共享 L2 entry 的 MESI ping-pong | R1 stage **7 µs → 80 ns**, 87× |
| seqlock CAS | reader 抢 per-bucket spinlock 排队 | writer 不阻塞 reader |
| forwarder-pool-direct | cross-host read 3 个 CXL roundtrip | 3 → 2 roundtrip |

| Path | typical latency |
|---|---:|
| A read TLS L1 hit | **80 ns** |
| A read L2 hit | ~7 µs |
| A read owner-self miss | ~10 µs |
| A read cross-host miss | ~22 µs |
| A write no inval | ~16 µs |
| A write with inval | ~22 µs |
| **W10 (dominant)** | **6.22 µs mean** |

| 当前状态 | 数值 |
|---|---:|
| YCSB target | 20 Mops/s |
| A iter-11A peak (workload-a) | **13.69** |
| iter-11A bimodal cells | 13 (> gate-12 = 8) |
| 命中 target 的 workload | **0 / 5** |
