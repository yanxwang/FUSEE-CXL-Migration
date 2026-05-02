# Task plan — iter-2A-revised — N:1:1:N + atomic_store invalidation (strict A)

**Author**: Claude
**Date drafted**: 2026-04-27
**Status**: DRAFT v1 — awaiting user review
**Branch**: `feat/cxl-migration` (commit prefixes
`[iter2A-rev-revert]`, `[iter2A-rev-arch]`, `[iter2A-rev-sender]`,
`[iter2A-rev-recvr]`, `[iter2A-rev-sweep]`, `[iter2A-rev-decomp]`)

---

## What this plan does

Rewrites iter-2A from scratch with the **architecture you actually
wanted from the start**: N producer threads → 1 sender thread per
host (CXL out) → CXL ring → 1 receiver thread per host → atomic
invalidation via x86 coherence. **Preserves protocol A's strict
linearizability** (which iter-2A's wire silently broke).

iter-2A wire was already source-level reverted:
`src/cxl_kv_ops_A.cc` and `.h` are back to iter-1A end state
(1ab30d7); cxl_per_host_ring.h declarations remain as dead code
to be redesigned in Phase 1.

---

## Open Qs (please decide before Phase 1)

| # | Question | Default | User position |
|---|----------|---------|---------------|
| QR1 | DramInvalQueue 模块——是删还是保留？ | **保留**（B 协议 `cxl_kv_ops_B.cc:160, 536` 在用 Phase 4 same-host bypass 路径；删它会破坏 B）。**只让 A 不再调用它**。`cxl_same_host_queue.h` 和 `DramInvalMatrix` 数据结构原地保留。 | ✅ user-confirmed |
| QR2 | iter-2A summary doc 怎么处理？ | **保留并加 "REVERTED" 顶部 banner** | ✅ user-confirmed |
| QR3 | cxl_per_host_ring.h 重新设计为 SPSC 版本 | **重新设计**：`PerHostInvalEntry` (64B integer cacheline; `op_id` 编码 `(host_id<<56) | (worker_slot_idx<<48) | op_seq`，0 = empty sentinel) + `PerHostSpscRing` (head/tail in separate cachelines, depth=256, op_id sentinel for "ready") + `AckChannel` (atomic seq counter)；新建 `src/cxl_a_local_aggregator.h` (DRAM MPSC queue, bounded with backpressure)；invalidate 路径上 sender 通过 op_id 提取 worker_slot_idx 索引 worker_ack_buf | ✅ user-confirmed (用 ring buffer) |
| QR4 | Reader 路径加 atomic acquire-load | **是**。`cache_epoch_arr[B]` 改成 `std::atomic<uint64_t>`；reader 用 `.load(std::memory_order_acquire)`；writer/receiver 用 `.store(NEW, std::memory_order_release)`。**x86 上无额外指令成本**（TSO 天然有 acquire/release 语义；annotation 只防编译器 reorder）。语义上保证：reader 看到 NEW epoch 时，writer 在 release-store 之前的所有写（包括 hash slot 更新、bucket cacheline flush）都对 reader happens-before. **strict A linearizability 的硬要求**。 | ✅ user-clarified |
| QR5 | Sender batching K + 是否 sweep K 参数 | **K default 4**, env `FUSEE_SENDER_BATCH_K`; **加 timeout** `FUSEE_SENDER_BATCH_T_US` default 20 µs ("K filled OR T_us elapsed" 谁先到 flush). **K sweep 加为 Phase 6.5**：T ∈ {64, T_peak from Phase 6} × K ∈ {1, 2, 4, 8, 16, 32, 64} cache=on，约 10-14 cells，选最佳 K 写入 default. K_max ≤ T 因为 strict A 下任意时刻 in-flight ops ≤ T；超过 K=T 必死锁（除非 timeout 补救）。 | ✅ user-confirmed (K 范围修订为 ≤T) |
| QR6 | iter-2A-revised deadline + 自主模式 | undecided。CLAUDE.md §1.5 适用——所有 phase 必须 ship；spare time → 更多验证。 | ⏳ |

---

## 1. Motivation（为什么推倒重来）

iter-2A wire（commit `4f05b83`）有**两层错误**：

### 1.1 语义违反（最严重）

iter-2A 的 `replicator_loop` PHR=1 分支：
```
push N-1 个 DramInvalEntry 到本机各 client 的 DramInvalQueue
立刻 publish ack_seq    ← 不等本机 client 真消费 inval
```

writer 看到 ack_seq advance 就 commit。但**本机其它 client 此时可能还没 poll DramInvalQueue**——它们的本地 DRAM cache 仍是旧 epoch。
后续 read 命中旧 cache 不 refetch CXL → **strict A linearizability 被破坏**。

iter-2A summary 完全没文档化这个语义降级。**A 协议的存在意义就是 strict
linearizability 而不是给 C 加变种**——这条路径下 A 协议已无 raison d'être。

### 1.2 架构 mis-implementation

用户原意 ("per-host ring") 是 **N producer → 1 sender thread → CXL → 1 receiver thread → 1:N invalidation**——一个干净的 N:1:1:N 聚合架构。

iter-2A 实际实现是：
- 没有 sender thread。N 个 producer 直接做 `tail.fetch_add` + 自己 clflushopt 写 CXL
- 没有 aggregator queue
- receiver 端虽然有 1 thread，但用 DramInvalQueue 串行 push N-1 次

**既没拿到 aggregation 收益（CXL 写仍多 producer 抢）也没保 strict 语义**。是个两不讨好的半成品。

### 1.3 修正方向

本 iter 实现真正的 N:1:1:N + 利用 x86 硬件 coherence 让 invalidation
消除 ACK 收集瓶颈（承上次讨论的设计）。每写一次 commit 序列见 §3。

---

## 2. Scope

### 2.1 In-scope

#### 架构组件（Phase 1-4）

- **重写 `src/cxl_per_host_ring.h`** 为 **SPSC** ring
  (1 sender thread per src-host writes; 1 receiver thread per dst-host
  reads)。**不再是 MPSC**。
- **新建 `src/cxl_a_local_aggregator.{h,cc}`** —— per-host 本地 DRAM
  aggregator queue (MPSC; N producers → 1 sender)
- **新建 sender thread**：每 host 1 个 pthread，drain aggregator
  queue，batch K 条进 1 次 CXL 写
- **改写 receiver thread (`replicator_loop`)** 为 atomic_store
  invalidation 路径（不再 push DramInvalQueue）
- **改 dispatch_and_wait**：
  - same-host invalidation → writer self atomic_store 到本机各 client
    的 cache_epoch (release-store)
  - cross-host → enqueue 到本地 aggregator queue, spin 等 sender 标记
    我的 op_id 已 ACK
- **改 reader 路径** (cache=on 路径里所有 cache_epoch 访问)：从 plain
  load 改成 atomic acquire-load
- **A 完全停用 DramInvalQueue**——B 协议保留不动
- **保留 LFM bucket lock** (Phase 1 lock acquire 不变)

#### 验证 + 数据收集（Phase 5-7）

- correctness battery（smoke 多 cell + 2-host consistency check）
- A-only **完整** scaling_ycsb sweep（80 cells = 5 wl × 8 T × 2 cache）
- 完整 N:1:1:N decomp 实验：
  - sender 侧 instrumentation（aggregator wait / batch fill / CXL write / ack wait）
  - receiver 侧 instrumentation（CXL read / apply / atomic_store fanout / ack publish）
  - writer 侧 instrumentation（lock / local apply / same-host atomic_store / aggregator enqueue / ack wait / unlock）
  - 4 个 T cells (2/4/8/16) × 2 PHR modes × 2 reps = 16 decomp runs

### 2.2 Out-of-scope

- **B 协议改动**——B 仍走 legacy PendingRingMatrix + DramInvalQueue。B 的同样
  rewire 留给后续单独 iter。
- **C 协议**——已闭，不动。
- **multi-receiver V2**——atomic_store invalidation 让 receiver per-entry
  cost 降到 ~5 ns × N（不是 50-200 ns × N），单 receiver thread 大概率够。
  万一 decomp 显示 receiver 仍是 bottleneck，**这是 iter-3A 议题**。
- **Sender batching K 参数 sweep**——本 iter default K=4，先 ship。K
  扫描留给 iter-3A。
- **PerHostOutEntry 进一步压缩到 16B**——iter-2A 原 plan 的 Solution 2，本 iter
  scope 不包括。先把架构拉正再谈优化字节。
- **Variable KV size for A**——iter-4 没 port 到 A，本 iter 不做。

---

## 3. Architecture spec（N:1:1:N + atomic_store invalidation）

### 3.1 Thread 角色（per host）

```
N forked client processes (cid 0..N-1):

  primary client process (cid=0):
    main thread:    worker (跑 YCSB ops, 自己也 dispatch + invalidate)
    pthread #1:     sender   (NEW; spawn at attach; join after all-done barrier)
    pthread #2:     receiver (REUSE replicator_loop, rewrite to atomic_store path)

  other client processes (cid 1..N-1):
    main thread:    worker only (无 pthread)
```

**Spawn pattern** (matches iter-3 phase-3 multi-flusher convention):
- Primary client 在 `attach()` 完成后 spawn 两个 pthread。Sender/receiver 通过 mmap MAP_SHARED 看到所有 worker 共享的 LocalAggregatorQueue / cache_epoch_arr / worker_ack_buf。
- Primary main thread 跑完自己 YCSB ops 后 **不立刻 join sender/receiver**——必须先 `barrier_wait_all_done()`（等所有 N 个 client 都标 done）；只有所有 worker stop dispatch 后，sender/receiver 才能干净 join。FUSEE runner 已有此 barrier。
- Crash 行为：primary 挂掉 → sender/receiver 都死 → 其它 client lockup. 单点故障，与 multi-flusher 一致；靠 crash-recover-test 兜底。

**CPU pinning**:
- Worker cid=K → core K (default; 已有 affinity 设置 from iter-1A)
- Sender pthread → core T (env: FUSEE_SENDER_CORE)
- Receiver pthread → core T+1 (env: FUSEE_RECEIVER_CORE)
- T=64 → 64+2=66 cores ≤ 86 ✓；T=86 → sender/receiver 落 HT sibling cores，仍 OK

### 3.2 Data structures (CXL + DRAM)

**Shared-memory API**: 所有 DRAM-side shared 结构用 **`mmap(MAP_SHARED | MAP_ANONYMOUS)`** pre-fork 分配（primary client 在 fork children 之前 mmap，children 自动继承同一物理页）。**不用 sysv shm**——避免 key 冲突，与 FUSEE 现有 `dram_mat` (Phase 4) + `batch_shm` (iter-3 phase-3) 模式一致。CXL-side 结构走 cxl_region 既有 attach 路径。

| 结构 | 位置 | 类型 | 大小 |
|---|---|---|---|
| `LocalAggregatorQueue` | DRAM mmap MAP_SHARED|ANON, pre-fork | **MPSC**, N producers + 1 consumer (sender thread); **bounded ring** with backpressure: enqueue spin on slot-free with 5 ms timeout (return -1 on timeout, writer retries) | 16 KB / host |
| `PerHostSpscRing[H][H]` | CXL region | **SPSC**, 1 producer (src-host sender) + 1 consumer (dst-host receiver); ring buffer depth=256; head/tail in separate cachelines; entry.op_id = 0 sentinel for "ready" | 4 × 4 × 256 × 64 B = 256 KB |
| `AckChannel[H][H]` | CXL region | atomic uint64 seq counter (writer = receiver, reader = sender); receiver advances seq after processing entry, sender spins on advance | 64 B × 16 = 1 KB |
| `cache_epoch_arr` | DRAM mmap MAP_SHARED|ANON, pre-fork | per-bucket `std::atomic<uint64_t>`; align each bucket to its own cacheline to avoid false sharing between adjacent buckets' atomic stores | num_buckets × 64 B = 65536 × 64 = 4 MB / host |
| `worker_ack_buf` | DRAM mmap MAP_SHARED|ANON, pre-fork | array indexed by `worker_slot_idx` (= host-local cid 0..N-1)；each slot is `std::atomic<uint64_t> seq` cacheline-padded; sender stores `entry.op_seq` after receiving cross-host ACK; worker spins on `worker_ack_buf[my_cid].seq.load(acquire) >= my_expected_seq` | N × 64 B = ~5 KB / host |

**Backpressure design (LocalAggregatorQueue)**:
```
enqueue(entry):
  for i in 0..max_spin:
    if slot[tail % depth].op_id == 0:    # slot free
      write entry; sfence; slot.op_id = entry.op_id; advance tail
      return 0
    pause; if (timed > 5ms): return -1
  return -1  # writer retries; surface as op-level retry
```
**Why option 1 (spin + timeout)**: drop violates strict A; error-return forces writer retry path which integrates naturally with the LFM lock release/reacquire we already do on conflicts.

`cache_epoch_arr` 是新的——current code 把 cache_epoch 存在每个
client process 的私有内存。改到共享 mmap MAP_SHARED|ANON 后，receiver 一次
atomic_store 就让所有同 host client 看见（x86 coherence）。

### 3.3 Writer commit 序列（writer thread = worker thread itself）

| # | 操作 | 位置 | 估 latency |
|---|---|---|---|
| 1 | LFM bucket lock acquire | LFM | ~µs (existing) |
| 2 | 写 hash slot 到 CXL + clflushopt + sfence | CXL | ~600 ns |
| 3 | bump CXL bucket epoch (atomic) | CXL atomic | ~600 ns |
| **4** | **同 host invalidation: 写 release-store 到 cache_epoch_arr[B]** | DRAM | **~5 ns**（**1 个 store 让所有同 host client 下次 acquire-load 看到**；x86 coherence 自动同步）|
| 5 | mfence (确保 step 4 全局可见) | local | ~5 ns |
| 6 | enqueue cross-host inval request 到 LocalAggregatorQueue（entry 携带 `worker_slot_idx = my_cid` + `op_seq = my_next_op_seq++`） | DRAM (MPSC enqueue) | ~50 ns |
| 7 | spin 等 `worker_ack_buf[my_cid].seq.load(acquire) >= my_op_seq`（sender 设此值在收到 cross-host ACK 后） | DRAM | RTT (~1.2 µs) + 排队 |
| 8 | LFM bucket lock release | LFM | ~ns |
| 9 | commit return | — | — |

**strict A 语义保证**：
- step 4 完成 + mfence 后，**任何同 host client 下次 acquire-load
  cache_epoch_arr[B]** 必看到新 epoch → 必 refetch CXL → 必看到新 slot 值
- step 7 完成 = 跨 host 的 receiver 已经做了它的 step R3（atomic_store 给
  对端本机所有 client 的 cache_epoch_arr[B]）→ 对端 client 同样看到
- writer step 9 commit return ⇒ 所有 client（同 + 跨 host）下次 read 看到新值
- linearizability ✓

### 3.4 Sender thread（per host，1 个）

```
loop:
  S-1: drain LocalAggregatorQueue:
       collect entries until (count == K) OR (oldest entry age > T_us)
       (K = FUSEE_SENDER_BATCH_K default 4; T_us = FUSEE_SENDER_BATCH_T_US default 20)
  S-2: 按 dst_host 分组 (≤ H-1 组, H=2 时 1 组)
  S-3: for each dst_host:
         memcpy K_actual 条 PerHostInvalEntry 进 ring (SPSC; no atomic claim needed)
         clflushopt × ⌈K_actual * sizeof(entry) / 64⌉
         sfence
         tail.store(tail + K_actual, release)  # publish
  S-4: for each dst_host:
         spin 等 AckChannel[me][dst].seq advance to my last batch_seq
  S-5: for each entry just acked:
         worker_ack_buf[entry.worker_slot_idx].seq.store(entry.op_seq, release)
         (worker thread sees this on its spin in step 7;
          monotone seq makes it safe even if sender batches across multiple
          ACKs on the same worker — though strict A guarantees only 1 in-flight
          per worker so this is defensive)
  goto loop
```

**Sender 是 SPSC writer to CXL**——没有 fetch_add 抢占，没有跨 core CXL 写竞争。
**Batching 是核心优化**：K 条共享 1 个 RTT。
**Timeout** 防止 K 不够而 sender 长 starve（特别是 low T 或 burst-y workload）。

**CPU pinning**:
- Sender thread `sched_setaffinity` 钉到 isolated core (default = core T，i.e., 紧邻 worker pool 之后)
- env override: `FUSEE_SENDER_CORE`
- 防止 sender 与 worker 抢 schedule 引发 cache 冷启 / 调度抖动

### 3.5 Receiver thread（per host，1 个；改写 replicator_loop）

```
loop:
  R-1: clflushopt + load PerHostSpscRing[*][me].next_unread
  R-2: 若有新 entry: clflushopt + load entry payload (K 条一次)
  R-3: for each entry:
         apply update to local hash table cache (CXL-backed; ~50 ns DRAM)
         atomic_store(cache_epoch_arr[entry.bucket_idx], NEW_EPOCH, release)
           ← 1 个 release-store, x86 coherence 让本机所有 client 下次看到
  R-4: mfence
  R-5: AckChannel[entry.src_host][me].seq = batch_seq (atomic store + clflushopt)
  R-6: advance next_unread cursor + clear ring entries
  goto loop
```

**Receiver per-entry 成本**：
- R-2 `load entry payload`: ~600 ns CXL load
- R-3 `apply + atomic_store`: ~50 ns + 5 ns = 55 ns
- R-5 `ack publish`: amortise per batch K → ~600 ns / K

**At K=4, per-entry receiver cost ≈ 600+55+150 = 805 ns**——单
receiver thread throughput ≈ **1.24 M entries/sec/host** = 2.48 M entries/sec aggregate。

**CPU pinning**:
- Receiver thread `sched_setaffinity` 钉到 isolated core (default = core T+1)
- env override: `FUSEE_RECEIVER_CORE`

### 3.6 Reader 路径变化（FUSEE_CACHE=1）

```c
// OLD (current):
if (cache_epoch_local_[B] == cache_epoch_seen_at_last_fetch_[B]) {
    return local_dram_bucket_[B];
}

// NEW (strict A under atomic_store invalidation):
uint64_t cur = cache_epoch_arr[B].load(std::memory_order_acquire);
if (cur == cache_epoch_seen_at_last_fetch_[B]) {
    return local_dram_bucket_[B];
}
// else refetch CXL...
```

`cache_epoch_arr` 改成共享 mmap MAP_SHARED|ANON 数组 (atomic uint64 per bucket)。
其余 reader 逻辑不变。**唯一额外 cost：每次 read 多一次 acquire-load
DRAM atomic ≈ 5 ns**。

### 3.7 同 host vs 跨 host 区分

writer 在 step 6 enqueue cross-host request 时，**只 enqueue 真正跨 host 的
dst**——本机内的 invalidation 已经在 step 4 做完了 (atomic_store)。
H=2 时，每条 cross-host request 只对应 1 个对端 host (= dst_host=other)。

**没有 DramInvalQueue。完全消除**。

---

## 4. Phased breakdown

| # | Phase | Prefix | Deliverable | Verify gate |
|---|-------|--------|-------------|--------|
| 0 | Plan review | — | This doc + QR1-QR6 决议 | user sign-off |
| 1 | Data structure design + commit revert formalize | `[iter2A-rev-revert]` | `cxl_per_host_ring.h` 重写为 SPSC ring buffer (depth=256, head/tail 分 cacheline, op_id sentinel); 新建 `src/cxl_a_local_aggregator.{h,cc}` (MPSC + **bounded backpressure: spin + 5ms timeout**); 新建 `src/cxl_a_cache_epoch_arr.h` (shared mmap MAP_SHARED|ANON `std::atomic<uint64_t>` 数组); commit 1 个 "revert iter-2A wire + redesign data structures" | builds; cxl_kv_ops_A.cc 仍用 iter-1A end-state（dispatch 走 legacy）; `static_assert(sizeof(PerHostInvalEntry) == 64)` |
| 2 | Sender thread 实现 + CPU pinning | `[iter2A-rev-sender]` | `start_sender()` / `sender_loop()` with **batch K + timeout T_us** drain (FUSEE_SENDER_BATCH_K default 4, FUSEE_SENDER_BATCH_T_US default 20); `worker_ack_buf` 通讯; **`sched_setaffinity` to FUSEE_SENDER_CORE (default=T)**; backpressure tested via synthetic burst | unit smoke: sender drain test (synthetic enqueue 100 条，sender 100% drain 且按 batch K 标 ACK; timeout path 在 partial-fill 触发) |
| 3 | Receiver atomic_store invalidation + CPU pinning | `[iter2A-rev-recvr]` | `replicator_loop` 重写：atomic_store 替代 DramInvalQueue push; AckChannel publish; **`sched_setaffinity` to FUSEE_RECEIVER_CORE (default=T+1)** | smoke: 1 host 上 1 worker enqueue 1 entry，receiver atomic_store + ack；reader 之后 read 看到新 epoch (acquire-load) |
| 4 | Writer dispatch + same-host atomic_store | `[iter2A-rev-arch]` | `dispatch_and_wait` 重写：step 1-9 序列；atomic_store 同 host inval; aggregator enqueue cross-host (with backpressure error path); spin worker_ack_buf | smoke: 2-host T=2，writer commit 后 reader 必看到新值（same + cross host） |
| 5 | Reader 路径改 acquire-load + integration battery + **B regression smoke** | `[iter2A-rev-arch]` | `cxl_kv_ops_A` cache=on read path: `cache_epoch_arr[B].load(std::memory_order_acquire)`; A consistency spot-check 100K ops T=2/4/8 cross-host diff = 0; **B 协议 smoke**: `cxl_ycsb_runner_B` workload C T=4 cache=on 100K ops，与 iter-1A 数 ±10 % 内（确认 attach plumbing 调整未顺手破坏 B）；crash-recover smoke (fork-kill-restart 不 segfault) | A: 0 consistency violation 跨 5 runs; B: 数字 ±10 % 内 |
| 6 | A-only **full scaling_ycsb sweep** | `[iter2A-rev-sweep]` | 80 cells (workload {a,b,c,d,f} × T={1,2,4,8,16,32,64,86} × cache={on,off}); chained overnight; logs/g34_iter2A_rev_sweep_<ts>/ + finalize 30-plot deliverable per scaling_ycsb_spec §6 (Style B mandatory) | OK count ≥ 70/80; FAIL pattern recorded |
| **6.5** | **Sender batching K sweep** | `[iter2A-rev-sweep]` | T ∈ {64, T_peak from Phase 6} × K ∈ {1, 2, 4, 8, 16, 32, 64} × cache=on (≈ 10-14 cells, 2 reps each); also add 1 cell K=128 + T_us=50 to验证 timeout 路径; output: K-vs-throughput 曲线 (Style B), best K per T 选定写入 default | sweep 完成；选定 best K 写 commit，下游 Phase 7 用此 K |
| 7 | Full N:1:1:N decomp 实验 + **queue depth probe** | `[iter2A-rev-decomp]` | writer 6-stage + sender 5-stage + receiver 6-stage instrumentation (gated FUSEE_LATENCY_DECOMP=1); 16 cells (T={2,4,8,16} × PHR={legacy,N11N} × 2 reps); **queue-depth probe**: 独立线程每 100ms 采样 LocalAggregatorQueue + PerHostSpscRing 深度，输出 `queue_depth.csv`; raw csv 归档; per-stage 表 + queue depth 时序 + **Little's law 自洽性 check**（arrival × wait_time ≈ queue_depth）| every stage non-zero counts; sum vs wall-clock within 5%; Little's law 不偏离 > 30 % 否则 flag |
| 8 | Summary + plots + indexes + **methodology §9.1 update** | `[iter2A-rev-sweep]` | `docs/iters/iter2A_revised_summary_<date>.md`; 4-5 张 Style B plots; progress.md tail; runs_index 行; memory digest; **回头 update `docs/refs/optimization_methodology.md` §9.1 "Aggregate-before-CXL"** ——把 corollary 写成有 3 个 confirmed 实例 (iter-5 multi-flusher V2 / iter-2A wire mis-impl 反例 / iter-2A-revised 正例) 的 stable pattern + atomic_store-via-coherence 推论 (来自本 iter 实测) | 所有 deliverables + index updates 都 ship; methodology §9.1 多了 3rd-confirmed-example block |

**Scope notes**: 此 plan 涵盖 9 个 phase（含新增 Phase 6.5），属常规 iter 范围；**不超出 typical iter scope**。CLAUDE.md §1.5 适用——所有 phase 必须 ship；descope 仅在物理不可能时允许，且必须先问 user。

**Heaviest / highest-risk phases** (qualitative, used for risk attribution only):
- Phase 7（decomp 16 cells × 17 stage instrumentation + queue probe + Little's law check）—— 最重的实验+分析阶段
- Phase 1（数据结构 + 跨 process shm 协调）—— 最复杂的同步 / lifecycle 设计
- Phase 5（reader path 改写涉及 read path 上每个 cache_epoch 访问点）—— 改动面积大、回归风险

---

## 5. Files touched

### 5.1 NEW files

- `src/cxl_a_local_aggregator.{h,cc}` — MPSC queue (bounded + backpressure 5ms timeout) + sender thread + worker_ack_buf
- `src/cxl_a_cache_epoch_arr.h` — shared mmap MAP_SHARED|ANON cache_epoch atomic array
- `docs/iters/iter2A_revised_summary_<date>.md` — iter summary
- `docs/iters/decomp_n11n_<date>.md` — Phase 7 detailed decomp data + Little's law check
- `docs/iters/k_sweep_<date>.md` — Phase 6.5 K batching sweep results
- `docs/sweeps/g34_scaling_ycsb_A_only_iter2A_rev_<ts>/` — Phase 6 sweep deliverable
- `docs/sweeps/g34_iter2A_rev_kbatch_<ts>/` — Phase 6.5 K-sweep deliverable

### 5.2 REWRITE

- `src/cxl_per_host_ring.h` — MPSC → SPSC; new entry layout; AckChannel struct
- `src/cxl_kv_ops_A.cc::dispatch_and_wait()` — N:1:1:N path (no FUSEE_PER_HOST_RING env; this IS the path now)
- `src/cxl_kv_ops_A.cc::replicator_loop()` — atomic_store path
- `src/cxl_kv_ops_A.cc` cache_epoch read sites — atomic acquire-load

### 5.3 MODIFY (small)

- `src/cxl_kv_ops_A.h` — add aggregator + cache_epoch_arr members; remove per_host_rings_enabled_ + dram_mat_ for A (B keeps dram_mat_)
- `tests/cxl_ycsb_runner.cc` — wire FUSEE_SENDER_BATCH_K; bytes_for() account for new structures
- `scripts/run_g34_scaling_sweep.sh` — env passthrough for new knobs

### 5.4 NOT TOUCHED

- `src/cxl_kv_ops_B.{h,cc}` — B 协议不动；继续用 PendingRingMatrix + DramInvalQueue
- `src/cxl_kv_ops_C.{h,cc}` — C 已闭
- `src/cxl_same_host_queue.h` (DramInvalQueue) — 保留，B 在用
- `src/cxl_pending_ring.h` (legacy PendingRingMatrix) — 保留，B 在用

### 5.5 DELETE

- 无（不删任何 module。A 不再 reference DramInvalQueue 即可）

---

## 6. Verification gates

### Phase 1 (data structure)

- builds with `-DCONSENSUS_OPT=1` (A binary)
- builds with `FUSEE_LATENCY_DECOMP=0` byte-for-byte same as commit `1ab30d7` (post-revert state)

### Phase 2 (sender thread)

- standalone unit test: 1 thread enqueue 1000 ops；sender drain；check (a) all 1000 reach worker_ack_buf, (b) batch K worked (count batches), (c) 0 race / 0 lost

### Phase 3 (receiver atomic_store)

- 1-host smoke: writer thread atomic_store cache_epoch_arr[B]; reader thread acquire-load → 必看到新值
- 2-host smoke: receiver atomic_store after CXL load → 同 host reader 看到 within ~10 ns

### Phase 4 (writer dispatch)

- workload A T=2 cache=on：100 K ops，writer commit 后立即 same key search → 必返回新值（never stale）
- 跨 5 runs no consistency violation

### Phase 5 (integration)

- workload A T=8 cache=on, 100 K ops, 2 hosts: dump final hash table state both hosts; diff = 0
- 跨 5 runs no violation

### Phase 5 (B regression smoke)

- B `cxl_ycsb_runner_B` workload C T=4 cache=on 100K ops 完成
- 跟 iter-1A baseline 同 cell 数 ±10 % 内
- 任何 attach plumbing 改动未顺手破坏 B 路径

### Phase 6 (sweep)

- 80/80 OK 是 stretch goal; **70/80 也算成功**（高 T 即便 fail 也 archive）
- summary line 解析 OK 必有 `value_size=8 num_hosts=2 threads=N`

### Phase 6.5 (K sweep)

- 10-14 cells 全 OK
- K-vs-throughput 曲线单峰（或单调），如果 chaotic → flag 重测
- best K 选定写入 default + commit message 引用支持数据
- timeout 路径 cell (K=128 + T_us=50) 验证 sender 不死锁，throughput ≥ K=64 cell（因 timeout 自然降级）

### Phase 7 (decomp)

- 16 cells 全 OK; 每 cell 6+5+6 stage 累加和 vs 总 wall-clock within 5%
- queue_depth.csv 每 cell 至少 50 sample (≥ 5s run × 100 ms cadence)
- Little's law 检查：(arrival_rate × producer_wait_p50) ≈ queue_depth_median 偏差 < 30 % ；超过则 summary doc 解释为什么

### Phase 8 (summary)

- methodology §7 7-deliverable checklist 全 ✓
- methodology §9.1 update commit landed; 包含 3 个 confirmed example 的列表 + atomic_store-via-coherence corollary

---

## 7. Success criteria

### 7.1 必须达成

1. iter-2A wire 真正 revert 落 commit
2. N:1:1:N + atomic_store invalidation 完整实现，A 走新路径
3. **strict A linearizability 保**：Phase 4+5 consistency check 0 violation
4. **B 协议无 regression**：Phase 5 smoke ±10 % 内
5. A-only full scaling_ycsb 80 cells 完整 sweep + 标准 30-plot 交付
6. **K batching sweep 完成**：best K 实测选定，commit message 引用数据
7. N:1:1:N decomp 16 cells 完整数据 + queue depth probe + Little's law check，每 stage 量化
8. iter-2A-revised summary doc，**每个 throughput 数字必带 stage 归因**（"X Mops/s 受 stage Y 限制因为 Y 占 Z µs"），不再有 hand-wave
9. **methodology §9.1 update**：corollary 写成 3 个 confirmed example 的稳定 pattern + atomic_store-via-coherence 推论

### 7.2 性能目标（Phase 6 quantitative）

- workload A peak ≥ **5 Mops/s** at some (T, cache) cell
  - 基线 iter-1A baseline workload A peak = 0.54 Mops/s
  - 5 Mops/s = 9× gain；理论 N:1:1:N + batching K=4 + atomic_store invalidation 应当达到
  - 这是 §4.5 "criteria 来自下一步实质进步" 的 honest 数字；20 Mops/s 是 iter-3A 议题
- workload C/D/F (read-heavy 或低写) **不退化**: cache=on T=86 cell ≥ 2× legacy iter-1A 数

### 7.3 数据驱动 iter-3A

- Phase 7 decomp 输出 **明确指认下一个 dominant stage**（e.g.,
  "在 T=16+ 时 sender batching 已饱和，下一步要么加 sender thread
  分片，要么扩大 K"）
- 至少 3 个 ranked iter-3A 候选方向，**每个有量化 expected gain**

---

## 8. Risk

| Risk | Lik | Mitigation |
|---|---|---|
| `cache_epoch_arr` shm 分配跨 process race（fork order vs shm attach） | Med | `bytes_for()` 计入 + primary client init region；其它 client attach 前 spin 等 magic |
| atomic_store + acquire-load 在跨 socket NUMA 下延迟 > 5 ns 预期 | Low-Med | Phase 3 smoke 实测；若 > 50 ns 需重新评估 vs DramInvalQueue 优劣 |
| sender thread CPU contention（与 64 worker 抢核） | Med | Phase 2 + 3 都用 `sched_setaffinity` 钉专用 core；env override 让用户调 |
| **LocalAggregatorQueue backpressure 长 timeout 引发 op-level 卡死** | Low-Med | Phase 1 enqueue timeout 5 ms; writer 见 -1 重试；max retry 加 LFM lock release-reacquire 路径，避免持锁等 |
| **B regression 不易察觉**（attach plumbing 调整内存布局，可能撞坏 B 的 PendingRingMatrix offset） | Med | Phase 5 显式 B smoke gate；不过即便没破坏，建议 attach 完后 `bytes_for()` 与历史一致或显式 magic 升级 |
| **K batching K=64 在 strict A 下死锁**（K > 任意时刻 in-flight ops） | Med | Phase 6.5 sweep K ≤ T，且 sender 始终启用 timeout T_us=20 兜底 |
| **Queue depth probe 自身扰动测量** (额外线程占核 + cacheline 抢) | Low | probe 钉 isolated core (FUSEE_PROBE_CORE); 100ms 采样足够稀疏 |
| K=4 batching latency 加重 writer wait（writer 等 sender 凑齐 K） | Med-High | sender 加 timeout：T_us > 50 即便不满 K 也 flush；FUSEE_SENDER_BATCH_T_US env |
| crash-recover-test 真破：OpLog 仍 record per-bucket transitions，但 worker_ack_buf 重启丢失 | Med | Phase 5 smoke 必跑 crash-recover-test 即便 RDMA-era 不直接适用——至少 fork-kill-restart 不 segfault；真的 recovery 完整性留 iter-3A 单独评估 |
| Phase 7 decomp 仪表化扰动测量 (clock_gettime cost ≈ 25 ns × 17 stages × 200K ops = 85ms ≈ 1% perf) | Low | 比较 FUSEE_LATENCY_DECOMP={0,1} 同 cell；若差 > 5% 报告 caveat |
| **Methodology violation**：deadline 紧时再次诱发"显然不重要、跳过"判断 | **N/A — banned by §1.5** | spare time → 更多验证；不能砍 phase；deadline 真不够先问 |

---

## 9. iter-3A teaser (DEFERRED — no commit)

iter-3A 候选（**等本 iter Phase 7 decomp 数据**）：

a. **Sender batching K 调优 sweep** — K ∈ {1,4,16,64}，找 latency-vs-throughput sweet spot
b. **Multi-sender thread**（如果 single sender CXL write 撞 BW 上限）
c. **Multi-receiver V2**（如果 receiver atomic_store fanout 在 N=64 仍是 bottleneck）
d. **B 协议同样 N:1:1:N rewire**（iter-2A-revised 只做 A）
e. **Variable-KV value-size for A**（iter-4 work port 到 A）
f. **strict A 替代物**：考虑 vector-clock-based async ACK（writer 不等 RTT）

iter-3A 选择**严格基于本 iter 数据**——不再像 iter-2A 那样 hand-wave 选方向。

---

## Appendix — methodology cross-reference

- `CLAUDE.md` §"Iter execution discipline" — all phases ship
- `optimization_methodology.md` §1.2 diagnose first — Phase 7 decomp 给出 iter-3A 数据
- §1.3 hypothesis revise — iter-2A wire 已被 falsified（写在 §1.1 motivation）
- §4 hypothesis discipline — primary hypothesis "N:1:1:N + atomic_store invalidation 让 A peak ≥ 5 Mops/s"，falsifiable，量化目标
- §6.5 no compounding — 6 个 phase prefix 区分 commits，可 bisect
- §6.8 不要"显然"跳过 — applied
- §9.1 Aggregate-before-CXL pattern — 第 4 个落地实例（前 3：iter-5 multi-flusher V2 / iter-1A Phase 5a / iter-2A wire 错版）；本 iter 是**真正 N:1:1:N 实例 + atomic_store corollary 的实证**
- methodology §9.1 corollary 写法待 Phase 7 数据出来后定式 update
- `scaling_ycsb_spec.md` §7 — Style B plots mandatory
