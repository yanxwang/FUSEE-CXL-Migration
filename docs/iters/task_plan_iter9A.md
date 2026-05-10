# iter-9A constraint document — variable KV + N:1:1:N + 3-ring architecture + verification

**Author**: Claude (per user instruction 2026-05-04)
**Date drafted**: 2026-05-04
**Deadline**: **2026-05-10 18:00 CDT** (5+ day window — 时间无比充足，完全不需要为时间妥协任何实现选择)
**Branch**: `feat/cxl-migration`
**Predecessor**: `docs/iters/iter8A_summary_20260504.md`
**Spec refs**: `docs/design_goals.md §I-XIII` (esp. §I11 N:1:1:N, §VI MESSAGE PAYLOAD POLICY), `docs/path_decomp_spec.md`, `docs/scaling_ycsb_spec.md`, `docs/protocol_a_architecture_blueprint.md`

---

## TL;DR

iter-8A 的 4-Sol 诊断暴露了三个结构性瓶颈：(1) 跨 host fetch_add MPSC contention (1435 ns/op),
(2) single-thread responder/dispatcher 在 588k/s LD-CXL 轮询天花板,
(3) "测试 KV=1024 实际只跑 8B" 的长期 silent test bug。
iter-9A 用 4 个相互依赖的阶段一次性把这三个解掉：

1. **Variable KV size**——彻底告别 inline u64 假象
2. **N:1:1:N + 3-ring**（WriteRing / ReadRing / InvalRing）——pointer-only message + 命名 sender/receiver + CPU pin
3. **path_decomp on workload-A**——验证新架构没引入新瓶颈
4. **Full ycsb scaling sweep at all KV sizes**——iter-9A 的 official perf snapshot

**核心约束**：value bytes **永远**不进 message ring；3 个 ring 各有独立命名的 sender + receiver 线程；**所有线程都 CPU pinned** —— worker 在 cpu 0 至 (T-1)，6 个 system threads 从 cpu 64 起；blueprint + spec 文档**实时**随每 phase 更新（不是 Phase 5 一次性补）。

---

## Scope

### In scope
- variable-length value 数据路径（API + blockpool + cache_pool）
- 3-ring N:1:1:N message-passing 架构（WriteRing / ReadRing / InvalRing）
- 命名 sender + receiver thread per ring (6 system threads total)
- CPU pinning for all 6 system threads, starting from cpu 64
- workload-A path_decomp（per `path_decomp_spec.md`）
- in-iter simple fixes only if path_decomp 暴露的问题不改架构（e.g. K-shard sender/receiver）
- Full scaling_ycsb sweep at KV ∈ {256, 512, 1024}

### Out of scope (defer to iter-10A+)
- **Forwarder-pool-direct**（跳过 staging copy 这个 idea）—— per user 2026-05-04 instruction "押后备忘"
- Lock-free hashmap for cache_pool（解 R1 22× over expected 的优化项；iter-8A B.6 类别 2）
- Hot-bucket sharding within owner（iter-5C 落空的 lever）
- 真正的 lazy-free GC for blockpool（Protocol C iter-4 留下的 stub；non-blocker for benchmark）
- Variable-length **keys**（独立后期工作）

### Memo for future
- **Forwarder-pool-direct + cross-host pool generation + free-back ring** 设计已讨论，节省 ~10 µs/forward at KV=1024，但需要解 cross-host block lifetime + reader failure mode + pool capacity 重分配。详见对话记录 2026-05-04 末尾段。当 iter-10A 启动时取出。

---

## Hard constraints (这些违反 = iter 重做)

| 约束 | 验证手段 |
|---|---|
| **C1** Test KV=N 必须真的跨 host 传 N bytes（N ∈ {256,512,1024}）| Phase 1 加一个 cross-host BW microbench：单 cell N writes × N bytes 应等于 N×byte 实测 BW within 10% |
| **C2** Message ring entries 内含 value bytes = 编译期 reject | `static_assert(sizeof(WriteEntry::payload) == 0)` 或 `payload` 字段不存在 |
| **C3** **所有线程 CPU pinned**: T 个 worker pinned to cpu 0..(T-1); 6 个 system threads pinned to cpu 64..69 | startup-time `pthread_setaffinity_np` + log 出实际 pin 后 cpu_id；运行中 perf sched 验证 sched delay <100 µs；no overlap between worker 区 (0..T-1) 和 system 区 (64..69) |
| **C4** N:1:1:N 不再是 runtime no-op (iter-3A Finding-1) | startup-time assert: `phys_hosts_pr_ ≥ 2` if num_hosts_ ≥ 2，不满足直接 abort + log |
| **C5** G1 hash-diff 在 Phase 1 + Phase 2 后各跑一次，必须 PASS | per `scaling_ycsb_spec §13 gate 1` |
| **C6** path_decomp 在 Phase 3 必须按 `path_decomp_spec` 5 phase 全跑完 | `<dir>/per_stage_decomp.md` 32 stage 全覆盖 |
| **C7** **Living docs 实时更新**: 每完成一个 sub-phase 就同步更新对应章节，不留到 Phase 5 一次性补。受影响文档清单见下方 §"Living docs to update" | 每个 commit 必须同时包含代码改动 + 对应 doc 改动；`git diff --stat` 每次 review 一次 |

---

## Living docs to update (per C7)

每个 phase 完成时**同步**更新这些文档对应章节，不准囤到 Phase 5：

| 文档 | iter-9A 中要改的章节 | 触发 phase |
|---|---|---|
| `docs/protocol_a_architecture_blueprint.md` | Part II §II.3 (Send invalidate I1..I8) → 拆成新 InvalRing sub-stage 描述 | Phase 2 wire 完 |
| `docs/protocol_a_architecture_blueprint.md` | Part II §II.4 (Forward to owner) → 拆成 WriteRing + ReadRing 两个独立 sub-section + 重写 F1..F7 | Phase 2 wire 完 |
| `docs/protocol_a_architecture_blueprint.md` | Part II §II.5 (CacheDispatcher loop D1..D5) → 改为 InvalReceiver loop | Phase 2 wire 完 |
| `docs/protocol_a_architecture_blueprint.md` | Part II §II.6 (ForwardResponder loop) → 拆成 WriteReceiver + ReadReceiver 两个 loop sub-section | Phase 2 wire 完 |
| `docs/protocol_a_architecture_blueprint.md` | Part III.1 cheat sheet → 加 N:1:1:N + staging arena 物理布局更新 | Phase 2 wire 完 |
| `docs/protocol_a_architecture_blueprint.md` | Part II read path R3 + write path W1..W12 → 修正每个 stage 的 source code line number 引用（因为 ring 改了 + 新 sender thread）| Phase 2 wire 完 |
| `docs/protocol_a_architecture_blueprint.md` | Part II value bytes 流向部分 → 明确 op 4 reader 直接 LD-CXL pool, op 1/2 走 staging copy（区分清楚两种 data plane） | Phase 1 + Phase 2 各一次 |
| `docs/protocol_a_architecture_blueprint.md` | Snapshot version line | 每完一个 phase 一次 |
| `docs/design_goals.md` | §I9 / §I11 → 加 "message ring entry MUST NOT contain value bytes" 不变式（C2 的 spec 形式）| Phase 2 wire 完 |
| `docs/design_goals.md` | §X → 加 P6 "system thread CPU pinning is HARD requirement" + worker thread pinning convention | Phase 2 wire 完 |
| `docs/design_goals.md` | §II 物理布局 → ForwardStaging[H] 实施细节落地 | Phase 2.B 完 |
| `docs/design_goals.md` | §VI MESSAGE PAYLOAD POLICY 与 op 4 register-only / reader-direct read 明确对齐 | Phase 2 完 |
| `docs/scaling_ycsb_spec.md` | §3 Parameters 表 → KV grid 加注脚 "now enforced by C1: byte pattern verify" | Phase 1 完 |
| `docs/scaling_ycsb_spec.md` | §13 gate 6 → SOFT WARN → HARD FAIL（path_decomp 必须 ship） | Phase 5 |
| `docs/path_decomp_spec.md` | §11 reference instances → 加 iter-9A path_decomp 输出作第二个 sample | Phase 3 完 |
| `CLAUDE.md` | 加新 cautionary precedent (if iter-9A 暴露 anything novel) | Phase 5 |
| `docs/fusee_cxl_progress.md` | iter-9A 进度行（每 phase 各一次累加更新） | 每 phase 完 |

**强制规则**：每个 commit 的 staged file 列表里必须**至少**包含上面表格中**触发 phase 匹配**的 doc 改动，否则 commit 不应 merge。Phase 5 只做 `iter9A_summary_*.md` 撰写 + 上面 living doc 的最终一致性 review，**不补遗**。

---

## Phase 0 — pre-flight (≤30 min)

**目的**：确认 iter-8A 末状态可重建，cleanup 任何残留 process / mount。

**Steps**:
1. `git status` clean，rebase 到 main 最新
2. `bootstrap_slave.sh g3 g4` 重新同步代码 + 重建 `~/FUSEE_CXL/build-cxl/`
3. 跑一次 quick smoke test: `protocol_a_ycsb workload-d kv=8 T=4 cache=on` 应该健康
4. 确认 perf + bpftrace + bpfcc-tools 仍可用
5. 跑 `path_decomp` Phase 0 (µbench baseline) 一次，作 iter-9A 的初始 baseline

**Exit**: smoke test PASS + baseline.md 保存到 `docs/path_decomp_iter9A_pre_<ts>/`

---

## Phase 1 — Variable KV value implementation

**目的**：彻底告别"FUSEE_KV_SIZE 是骗局"。

### 1.A — API change

**当前** (`cxl_kv_ops_A.h`):
```cpp
int insert(uint64_t key, uint64_t value);   // ← value 8B u64 only
int update(uint64_t key, uint64_t value);
int search(uint64_t key, uint64_t *out);
```

**改为**:
```cpp
int insert(uint64_t key, const void *value, uint32_t value_len);
int update(uint64_t key, const void *value, uint32_t value_len);
int search(uint64_t key, void *out_buf, uint32_t buf_len, uint32_t *out_len);
```

API 上 source-compat helper 保留：
```cpp
inline int insert_u64(uint64_t key, uint64_t v) { return insert(key, &v, 8); }
```
方便老 test code 不改。

### 1.B — Blockpool dual-path port from Protocol C iter-4

参考 `src/cxl_kv_blockpool.{h,cc}`（已存在，Protocol C 用过）。Protocol A 此前 attached 但**没 wire 进 read/write path**。

需要做的：
1. `execute_write_local` W7-W8 改为：
   - 真 alloc `value_len` bytes
   - `pool_->write(blk_off, value_ptr, value_len)` (实际 write `value_len` bytes，不再硬编码 `sizeof(uint64_t)`)
2. `responder_handle` `kOpKindCacheRegister` 路径：
   - 读 slot encoded → decode size_class + blk_off
   - **不再** call `pool_->read(..., sizeof(v))`；下面 N:1:1:N + ReadRing 改完后**owner 不读 value**，只回 pointer
3. `cache_pool` 改为存 variable-length value bytes（之前是 `sizeof(uint64_t)` 固定）。可能要换成 `(value_ptr, len)` pair 或 inline buffer

### 1.C — Test runner change

`tests/protocol_a_ycsb.cc`:
- `FUSEE_KV_SIZE=N` → 真的传 N bytes value 给 insert/update（用确定性 byte pattern，e.g., `byte[i] = (key ^ i) & 0xff`）
- search 后 verify 整个 N bytes 匹配 expected pattern（不只检查首 8B）
- 这是 **C1 的实施**

### 1.D — Hash-diff G1 with variable KV

`tests/protocol_a_v2_invariant_check.cc` 加变种：每个 KV size × 每个 workload 跑一次 hash-diff，确认 cross-host bucket hash 在 load + trans 后两 host 一致。

### Phase 1 Exit Criteria
- [ ] G1 hash-diff PASS at KV ∈ {8, 256, 512, 1024} × workload {a, b, c, d, f}
- [ ] cross-host BW microbench: N writes × N bytes ≈ N²×byte/sec ± 10%
- [ ] smoke run workload-d kv=1024 T=4 cache=on healthy
- [ ] **C7 living doc**: blueprint Part II value bytes 流向段 + scaling_ycsb_spec §3 注脚 同 commit 落地

**预期 LOC**: ~400-600 (API 改动 + blockpool wire + cache_pool variant + test runner)

---

## Phase 2 — N:1:1:N + 3-ring architecture

### 2.A — Three rings on CXL

新建：
- `src/cxl_write_ring.h`：carries op 1/2/3 (Forward UPDATE/INSERT/DELETE)
  - Request: `(key, op_kind, staging_host, staging_off, len)` — **no value bytes inline**
  - Response: `(status,)` — owner copy 完成 + W7-W12 完成后 ACK
  - Data plane: forwarder 写 value 到 `ForwardStaging[forwarder]` CXL 区；owner 从 staging 读 + 复制到自己 KvBlockpool（**iter-9A 保留 staging copy 模式；forwarder-pool-direct 推 iter-10A**）
- `src/cxl_read_ring.h`：carries op 4 (CacheRegister)
  - Request: `(key,)` — pure register-only, no value request
  - Response: `(status, slot_pointer)` where `slot_pointer = (host_id_of_owner, blk_off, len, generation?)` — **only the pointer, not the value bytes**
  - Data plane: reader 拿到 slot_pointer 后**自己**直接 LD-CXL `KvBlockpool[owner].block_at(blk_off)`，不经过 ring。owner 端 `ReadReceiver` 完全不读 value bytes，只更新 sharer_bitmap + 回 pointer
- `src/cxl_inval_ring.h`：carries op 5 (Invalidate) — 沿用现有结构（已经 control-only，无 value）

**编译期 assert**（C2）:
```cpp
static_assert(!has_field<WriteEntry, value>::value, "WriteEntry must not contain value bytes");
```
或者直接看 sizeof：`static_assert(sizeof(WriteEntry) == ...)` 限制为 control-only size。

### 2.B — Per-host ForwardStaging region

`docs/design_goals.md §II` 已 spec：
```
[align 64]       ForwardStaging[H]    ← per-forwarder-host staging
```

实现：每 host 1 MB CXL staging arena，bump-cursor allocator + circular reuse（slot 在 forwarder 收到 ACK 后 free）。

Forwarder 端 op 1/2 流程：
1. `staging_off = staging[me].alloc(value_len)`（fail loud if 满）
2. `memcpy(staging[me].base + staging_off, value_ptr, value_len) + flush + sfence`
3. enqueue WriteRing message `(key, op_kind, staging_off, len)`
4. wait ACK
5. `staging[me].free(staging_off)`

Owner 端 op 1/2 receiver:
1. drain WriteRing message
2. read forwarder's staging: `pool_->copy_from_cxl(staging[forwarder].base + staging_off, value_len)` 
3. continue normal W7-W12 path with the read value bytes
4. ACK (releases forwarder's staging slot)

注：iter-9A 仍用 staging copy（owner copies from staging into own pool），跳过 forwarder-pool-direct（per user）。staging-direct + forwarder-pool-direct 是两个独立设计，押后那个不影响 iter-9A 实现。

### 2.C — Per-thread DRAM aggregator queue (producer side)

每个 worker thread 有自己的 `LocalAggregatorQueue` → 没有 worker-worker contention.
每条 ring 有一个 sender thread 轮询所有 worker 的 queue，drain → batch → write 到 CXL ring。

Per-host 一共 3 个 sender (one per ring type)。

参考 `src/cxl_a_local_aggregator.{h,cc}` 现成 impl, **现在终于要 wire 进 production path**（之前只在 test 里用）。

### 2.D — Single receiver thread per ring (consumer side)

每条 ring 有一个 receiver thread:
- `WriteReceiver`: drain WriteRing, dispatch to handler
- `ReadReceiver`: drain ReadRing, dispatch to handler
- `InvalReceiver`: drain InvalRing, dispatch to handler (== current CacheDispatcher 重命名)

**Receiver 的 handler 是同 receiver 线程跑还是 hand-off 给 worker pool**？iter-9A 第一版**保持 inline**（同 receiver 跑 handler），跟 iter-8A responder_handle 模式一致。

如果 Phase 3 path_decomp 显示 receiver-bound，Phase 3.1 加 K-shard handler。

### 2.E — Naming convention (C3 标识用)

| Ring | Sender thread name | Receiver thread name | Handler 名（inline）|
|---|---|---|---|
| WriteRing | `WriteSender` | `WriteReceiver` | `handle_write_op` |
| ReadRing | `ReadSender` | `ReadReceiver` | `handle_read_register` |
| InvalRing | `InvalSender` | `InvalReceiver` | `handle_invalidate` |

线程 `pthread_setname_np()` 用上面的名字（`top -H` / `htop` 直接看到）。

### 2.F — CPU pinning（**所有线程**）

g3/g4 是 86 core/host. T_max=64 → 22 cores 给 system thread + spare.

**Worker pinning**：T 个 worker 各自 pinned to cpu 0..(T-1).
- T=1: worker[0] → cpu 0
- T=64: worker[0..63] → cpu 0..63
- 一对一，不留多线程争同核

**System thread pinning**（不论 T）：
- cpu 64: WriteSender
- cpu 65: WriteReceiver
- cpu 66: ReadSender
- cpu 67: ReadReceiver
- cpu 68: InvalSender
- cpu 69: InvalReceiver
- cpu 70-85: spare（K-shard 扩展用 / future K-handler）

**实现**：
- worker spawn 时 `pthread_setaffinity_np(t, CPU_SET={t-th-cpu})`
- system thread 在 ring init 时同样 `pthread_setaffinity_np`
- 启动 log 必须打印每个线程的 `pthread_self() → name → cpu_id` 一览表，方便 `top -H` 验证

**反例 / 防错**：
- 不允许任何 worker pin 到 cpu 64-69 区（启动 log 出 cpu_id 后人工/script verify）
- 不允许 system thread overflow 到 worker 区
- 反例只 abort 一次，不 retry — 启动失败比中途 sched conflict 好诊断

### 2.G — Startup-time invariant assertion (C4)

```cpp
int CxlKvStoreA::attach(...) {
  ...
  if (num_hosts_ >= 2 && phys_hosts_pr_ < 2) {
    fprintf(stderr, "FATAL: phys_hosts_pr_=%d < 2 in multi-host mode "
            "(iter-3A Finding-1 silent no-op)\n", phys_hosts_pr_);
    abort();
  }
  ...
}
```

### Phase 2 Exit Criteria
- [ ] 3 rings + 6 system threads spawned + named + pinned (verified via `top -H` + log line)
- [ ] T 个 worker thread also pinned to cpu 0..(T-1); startup log dumps full pinning table; no overlap with system threads
- [ ] G1 hash-diff PASS (variable KV from Phase 1) at all KV sizes × workloads
- [ ] WriteEntry / ReadEntry / InvalEntry 编译期 assert no value bytes
- [ ] Startup assert C4 verified by deliberately running with phys_hosts_pr=1 → must abort
- [ ] **C7 living doc**: blueprint Part II §II.3-II.6 全部重写完毕 + design_goals.md §I9/§I11/§II/§VI/§X 同步更新，与 wire 改动同 commit

**预期 LOC**: ~800-1200 (3 ring impl + sender/receiver threads + aggregator wire + staging arena + naming/pinning) + ~300-500 doc

---

## Phase 3 — path_decomp on workload-A (validate new architecture)

按 `docs/path_decomp_spec.md` 5 phase 全跑：

### 3.0 - 3.5 (per spec)

**Cell**: workload-A KV=1024 T=64 cache=on (历史最难 cell)

**Outputs to `docs/path_decomp_iter9A_<ts>/`**:
- baseline.md (re-use iter-8A 的，only update primitives that changed)
- per_stage_expected.md (re-derive, must include WriteSender / ReadSender / receiver stages)
- probes_healthy/ + probes_anomaly/ (capture, anomaly retry up to 12×)
- per_stage_decomp.md (consolidated table)
- perf_capture/ (Sol-2 + Sol-3 only if anomalies flagged)
- rap.md (only if Phase 3.1 触发)

### 3.1 — In-iter simple fix (条件触发)

如果 path_decomp Phase 3 暴露 issue **AND** 满足以下两个条件：

(a) 不破坏 iter-9A 已 ship 的架构（不改 message 是否带 value 这种事，不改 ring 数量）
(b) 改动 ≤ 200 LOC

那么直接在 iter-9A 内修。允许的 fix 范围：

| Fix 类型 | LOC 估 | 例 |
|---|---|---|
| K-shard 某个 sender / receiver | ~150 | 如果某 ring receiver 仍是单线程 588k/s ceiling |
| Aggregator queue depth tune | ~20 | 如果 sender drain 跟不上 |
| Pinning policy 调整 | ~50 | 如果某线程 sched delay 异常 |
| Staging arena size tune | ~30 | 如果 staging fill rate 高 |

不允许的 fix（必须 defer 到 iter-10A）:
- 重构 ring 数量
- 改 value 是否 inline
- 改 sharding policy
- 任何引入新跨 host data flow 的改动

### 3.2 — Phase 3 decision tree

```
path_decomp Phase 2 输出: 32-stage 表
├─ 0 stage flagged → 直接 Phase 4
├─ 1-3 stage flagged AND 都符合 in-iter fix 条件 → Phase 3.1 fix → 再跑 Phase 3 verify → Phase 4
├─ ≥4 stage flagged OR 任一不符合 in-iter fix 条件
│   → STOP, 写 iter-10A backlog item
│   → 仍跑 Phase 4（不 fix，作 iter-9A 实测 baseline）
```

### Phase 3 Exit Criteria
- [ ] `path_decomp_iter9A_<ts>/per_stage_decomp.md` 32-stage 全覆盖
- [ ] 0 unjustified `✱ no data` row
- [ ] 任何 flagged stage 有 Phase 3.1 fix 或明确 iter-10A backlog 条目
- [ ] **C7 living doc**: `docs/path_decomp_spec.md §11 reference instances` 加 iter-9A 输出条目；blueprint Part III.3 加任何新发现的 failure mode

---

## Phase 4 — Full scaling_ycsb sweep at KV ∈ {256, 512, 1024}

按 `docs/scaling_ycsb_spec.md` 标准跑。

**Sweep matrix**:
- protocols: A only
- workloads: a, b, c, d, f (5)
- T: 1, 2, 4, 8, 16, 32, 64 (7)
- cache: on, off (2)
- KV: 256, 512, 1024 (3)
- reps: 1 (per spec §3 standard)

**Total**: 5 × 7 × 2 × 3 = **210 cells × 1 rep**

### 4.A — Anomaly scan

运行 `scripts/probe_anomaly_scan.py` 自动扫 `gap_to_target.md`. 任何 cell 命中 §13 gate 5 阈值（< 0.1 Mops/s OR < neighbor-geomean / 10）必须 5-rep verify 或 carve-out.

### 4.B — Doubling-ratio gate (per workload, per KV)

per `CLAUDE.md` 2026-05-03 update: 5 个 workload **每个**都要 T-doubling pre-saturation ≥ 1.5×.

### 4.C — Comparison vs iter-8A

每 cell 对比 iter-8A 末 healthy 数（[A.5 grid](../iter8A_phase1_ubench/residual_grid.csv)）：

| 期望 | 验证方式 |
|---|---|
| iter-8A 通过 cell 不能回退 | per-cell ratio iter9A / iter8A ≥ 0.9 |
| iter-8A residual collapse cell（A.5 表里 10 个）应该 fix | 这 10 个 cell 在 iter-9A 应 healthy（≥ 5 Mops/s minimum）|
| 新架构应有真正的 perf gain | 平均 ≥ 1.3× over iter-8A across all cells |

### Phase 4 Exit Criteria
- [ ] 210 cells × 1 rep = 210 runs done
- [ ] Anomaly-scan zero unexplained outliers
- [ ] Doubling-ratio gate PASS for all 5 workloads × 3 KV sizes
- [ ] 0 regression vs iter-8A
- [ ] iter-9A summary 写完
- [ ] **C7 living doc**: `docs/fusee_cxl_progress.md` iter-9A 数字行就位；blueprint snapshot version 推到 "end of iter-9A"

---

## Phase 5 — Final consistency review + summary

**注意：**§"Living docs to update" 表格中的所有更新**应该已在 Phase 1-4 各自完成时同步落地**（per C7）。Phase 5 的工作**不是补遗**，是：

### 5.A — Cross-doc consistency review
- `git log --since="iter9A start"` 列出所有 commit
- 每个 living doc 看一眼：所有该改的章节都改过了？术语一致？code line number 引用正确？
- 任何 inconsistency: 修复 + 加一个 `[iter9A-summary]` commit

### 5.B — `scaling_ycsb_spec.md` §13 gate 6 转 HARD
- 之前 SOFT WARN，iter-9A 完成后转 HARD FAIL（per iter-8A spec codify）
- 这是 iter-9A 末才能做的（前提是 path_decomp 已经被 iter-9A 用了一次）

### 5.C — Write `iter9A_summary_<date>.md`

按 `iter8A_summary_20260504.md` 模板：
- TL;DR
- Phase deliverables 表
- Headline measurements vs iter-8A
- Per-stage attribution table (from Phase 3 path_decomp)
- iter-10A backlog（包括 forwarder-pool-direct memo + 任何 Phase 3 暴露但 in-iter 没 fix 的项）
- Process retrospective

### 5.D — Memo iter-10A items
新建 `docs/iters/iter10A_backlog_memo.md`，包含至少：
- forwarder-pool-direct + cross-host pool generation + free-back ring（押后 idea）
- Phase 3 暴露但超 200 LOC 的 fix 项（如果有）
- 任何 iter-9A 中发现但与本 iter scope 不符的优化机会

### Phase 5 Exit Criteria
- [ ] All living docs 的 git diff 自洽（无章节引用旧 ring 名等等）
- [ ] `iter9A_summary_<date>.md` written
- [ ] `iter10A_backlog_memo.md` written
- [ ] `scaling_ycsb_spec.md §13 gate 6` 标记 HARD

---

## Risks & cautionary precedents

### iter-3A Finding-1 复发风险
N:1:1:N 上次实现失败的根因是 `phys_hosts_pr_=1` 默认值让整个机制 runtime no-op 5 个 iter，激活后 throughput 下降 5-50×。**C4 startup assert 是这次唯一的护栏**——如果忘了或被 bypass，iter-9A 重蹈覆辙的概率非常高。

### Same-host aggregator queue contention
T=64 worker 全部在 DRAM enqueue，是 same-host MPSC pattern。µbench (iter-8A baseline.md) T=64 same-host atomic max 25 ms. 解：**per-thread queue, sender 轮询所有 thread queue**（spec'd 设计，不是 single global queue）。这点必须在 Phase 2.C 落地。

### Variable KV 引入的隐性 bug
- cache_pool 之前存 8B fixed，现在 variable —— hashmap 实现可能假设 8B (e.g., compare with `==` instead of memcmp)
- pool 之前 `pool->write(off, &v, sizeof(v))` 全部 8 —— 改成 `value_len` 后 alignment / overflow 风险
- search 后老 test 假设 `*out` 是 u64，新 API 必须 backward compat

→ Hash-diff (C5) 是 catch-all; 但单独跑一组 unit test 验证 1B / 7B / 64B / 1023B value（边界 size）也很有价值。建议加个 `tests/cxl_kv_ops_A_varlen_test.cc`.

### Staging arena overflow
1 MB staging × write rate. 如果 owner ACK 慢 → forwarder 堆满 → forwarder fail-loud (C1 fallback to slow path?).

→ Phase 2.B 实现时加 staging 满判断 + fallback path（同步 spin? 直接 -EAGAIN?）。建议第一版直接 -EAGAIN 让 worker 重试，避免复杂的内嵌 spin。

### CPU pinning conflict
现在 iter-8A 还有 1 个 ForwardResponder + 1 个 CacheDispatcher 共 2 system thread。iter-9A 改成 6 个，pin 到 cpu 64-69。**确保没有 worker accidentally pin 到 64+ 区**（kernel default scheduler 不会，但用户/test 自己 pin 的不行）。

---

## Open questions for user — RESOLVED 2026-05-04

| QR | 问题 | 决议 |
|---|---|---|
| ~~QR1~~ | iter-9A deadline | **CONFIRMED 2026-05-10 18:00 CDT**. 5+ 天窗口；用户明确 "时间无比充足，完全不用考虑任何实现的时间 constraint" → **不允许**用 deadline 压力作为 in-iter descope 的理由，路径选择以正确性 + spec 对齐为优先 |
| ~~QR2~~ | op 5 ring 名 | **CONFIRMED InvalRing**（用户：typo，原意就是 InvalRing）|
| ~~QR2 (suppl.)~~ | op 4 ReadRing 数据流 | **CONFIRMED**: op 4 走 ReadRing 但 message 只 register directory bitmap, 不携带 value bytes; reader 自己从 CXL `KvBlockpool[owner]` LD-CXL value bytes（per Phase 2.A 描述） |
| ~~QR3~~ | T 网格上限 | **64**（保持 iter-8A 一致；workers cpu 0..(T-1), system threads cpu 64..69 不重叠）|
| ~~QR4~~ | reps in Phase 4 sweep | **1**（per scaling_ycsb_spec §3 standard；anomaly cell 5-rep verify 走 §13 gate 5）|
| ~~QR5~~ | Phase 3 path_decomp 只跑 workload-A 还是也跑其他 workload？| **iter-9A 只 A**（A 是历史最难 cell；其他 workload 通过 Phase 4 sweep 验证）|
| ~~QR6~~ | Phase 3.1 in-iter fix LOC budget | **200 LOC**（conservative；超过就推 iter-10A）|
| ~~QR7~~ | iter-10A backlog 文档现在起草 vs deferred 到 iter-9A 完成后？| **deferred**——iter-9A 跑完结果会大幅影响 iter-10A 优先级 |

**额外 user 指令 2026-05-04**:
- **所有线程都 CPU pinned**（worker + system thread 都要 pin；不只 system thread）—— 已落进 C3 + Phase 2.F
- **Living docs 实时更新**（不是 Phase 5 一次性补）—— 已加入 C7 + 上面 §"Living docs to update" 详细映射表

---

## Phase-by-phase commit prefix convention

| Phase | Commit prefix |
|---|---|
| Phase 0 | `[iter9A-preflight]` |
| Phase 1 | `[iter9A-varlen]` |
| Phase 2 | `[iter9A-3ring]` |
| Phase 3 | `[iter9A-pathdecomp]` |
| Phase 3.1 | `[iter9A-inlinefix]` |
| Phase 4 | `[iter9A-sweep]` |
| Phase 5 | `[iter9A-summary]` |

每个 commit 必须 reference G6/AP16 invariants per `scripts/git-hooks/pre-commit` H4 hook.

---

## Estimated commit log shape

```
[iter9A-plan][G6][AP16] iter-9A constraint draft
[iter9A-preflight][G6] Phase 0 baseline + smoke
[iter9A-varlen][G6][AP16] Phase 1.A API change variable-len value
[iter9A-varlen][G6][AP16] Phase 1.B-D blockpool wire + cache_pool variant + test
[iter9A-3ring][G6][AP16] Phase 2.A-B 3 ring impl + staging arena
[iter9A-3ring][G6][AP16] Phase 2.C aggregator + sender threads
[iter9A-3ring][G6][AP16] Phase 2.D-G receivers + naming + pinning + C4 assert
[iter9A-pathdecomp][G6] Phase 3 workload-A path_decomp
[iter9A-inlinefix][G6][AP16] (optional) Phase 3.1 in-iter scaling fix
[iter9A-sweep][G6] Phase 4 210-cell scaling_ycsb sweep
[iter9A-summary][G6][AP16] iter-9A complete summary + iter-10A backlog
```
