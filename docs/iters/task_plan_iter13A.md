# iter-13A Task Plan — Cross-host 数据拷贝消除 + 双轨对比挑选

**Date drafted**: 2026-05-17
**Branch**: `feat/cxl-migration`
**Predecessor**: `docs/iters/iter12A_phase5_rca.md` (Phase 5 stale-head fix, 60/60 + 40/40 WIN)
**North-star (依然)**: YCSB-A AND YCSB-C aggregate ≥ 20 Mops/s on g3+g4 testbed (`docs/design_goals.md`).

---

## TL;DR

iter-12A 把 bimodal collapse 修掉之后，cross-host 关键路径上还有两个**冗余的 data copy**：

- **读路径**：owner 把 value 从自己的 blockpool **再拷一份**到 read-staging（CXL→CXL，经 owner DRAM），reader 再从 staging 拷到 DRAM。Owner-side staging copy 是冗余的（reader 完全可以直接从 pool 拉）。
- **写路径**：worker 把 value 写到 forward-staging（DRAM→CXL），receiver 把 staging 读回自己 DRAM，再写到 owner pool（DRAM→CXL）。中间这次 staging→DRAM→pool 是冗余的（worker 完全可以一步到位写到 owner pool）。

iter-13A 分两个 Phase，每个 Phase **双轨实现两个候选方案**，在代表性 cell 上对比 throughput + latency，挑出 winner，然后跑完整 scaling_ycsb sweep 验证。

| Phase | 路径 | 候选 A | 候选 B | 挑选标准 |
|---|---|---|---|---|
| **Phase 1** | 读 | **RCU + epoch** 保护 reader 直读 pool block | **Hazard pointers** 保护 reader 直读 pool block | 代表性 cell 的 r_avg / r_p99 / 聚合 Mops/s |
| **Phase 2** | 写 | **W1 per-host reserved segments** 让 worker 直接写到 owner pool | **W3 batched pre-allocation** worker 一次性预约 N blocks，本地维护 reserved queue | 代表性 cell 的 w_avg / w_p99 / 聚合 Mops/s |

每个 Phase 结束都跑一次完整 spec-default scaling_ycsb sweep（5 workloads × 7T × 2 cache × 3 KV × 1 rep = 210 runs）做最终 verify。

---

## CLAUDE.md cautionary precedents 强化

iter-13A 必须主动避免前 3 个 precedent + iter-12A Phase 5 新增的 precedent #4：

1. **不允许 silent descope**：双轨实现就是双轨，不能只交 1 个 + 编个理由说"另一个 mock 一下就行"（iter-9A 教训）。
2. **不允许 hypothesis-without-observation**：双轨对比的"winner"必须有量化 throughput/latency 测量，不允许"理论上 A 更好"做结论（iter-12A Phase 5 教训）。
3. **不允许跳过 anomaly scan**：每个 sweep 完成后必须跑 §13 gate 5 anomaly scan，任何 outlier 必须 5-rep verify 或 cited explanation（iter-6A/7A 教训）。
4. **Phase delivery audit gate** 在 iter-13A summary 必填，每个 sub-phase 明确 ✅/⚠/❌。

---

## Scope（不可在 iter 内 silently 改）

| 维度 | 值 | 备注 |
|---|---|---|
| 协议 | Protocol A only | 不动 B / C |
| 工作负载 | a, b, c, d, f | per spec |
| Thread grid | 1, 2, 4, 8, 16, 32, 64 | per spec; T_max=64 (留 22 cores 给 system threads) |
| Cache mode | on, off | per spec |
| KV size | 256, 512, 1024 | per spec |
| Reps (sweep) | 1 | spec default; 单 rep 已能看出 bimodal 是否回归 |
| MAX_OPS | 200000 | spec default |
| TIMEOUT_S | 600 | spec default |

代表性 cell（双轨对比时跑这些）：
- `workloada T=4 cache=off kv=512` — read-heavy, 中等并行度
- `workloada T=32 cache=off kv=1024` — read-heavy, 高并行度
- `workloadb T=32 cache=off kv=256` — 中度写, 小 value
- `workloadc T=64 cache=off kv=1024` — 100% read, 应该最受读路径优化影响
- `workloadf T=32 cache=off kv=512` — RMW 混合，写路径优化敏感

每个代表性 cell 跑 5 reps，取 median + max for 对比。

---

## Hard constraints（违反 = iter 重做）

| ID | 约束 | 强制方式 |
|---|---|---|
| **C1** | §I9 strict-A linearizability **不变** | 每个 sub-phase commit 前 H2 hash-diff 20-cell battery PASS |
| **C2** | message ring entries 不再含 value bytes（已生效自 iter-9A C2） | RAP 中说明新增字段不破坏此 invariant；ring entry 仍只承载 control + pointer/blk_off |
| **C3** | 不允许 cross-host atomic on CXL 做 hot-path 协调 | RCU epoch / hazard slot 用 per-host DRAM atomic + flush_line publish，不允许跨 host fetch_add |
| **C4** | iter-12A Phase 5 修复必须保留 | 双轨实现的 attach path 不许 revert "总是 memset+flush" 行为 |
| **C5** | **C17 不适用于 iter-13A 的架构性新增**。CLAUDE.md C17 是 bug-fix 护栏 (防 sprawl)；iter-13A 是新模块引入，由 §XIII RAP 把关。新文件 (cxl_rcu.{h,cc} / cxl_hazard.{h,cc} / cxl_reserved_alloc.{h,cc} 等) 函数数 + 调用 site 数**不设上限**——重点是**完全消除 data copy** 且**严格保证读/写路径正确性 (§I9)**。C17 在 iter-13A 内只约束: 若发现旧代码 bug 需 fix，fix 动作 ≤ 3 file × function。 |
| **C6** | 双轨实现必须**同时可编译并存**（编译时 flag 切换）；不允许"track A 实现后 track B 把 track A 删了"  | 编译开关：`-DFUSEE_READ_GUARD={STAGING,RCU,HAZARD}`、`-DFUSEE_WRITE_ALLOC={STAGING,RESERVED,BATCHED}`。Default = `STAGING`（旧路径），新双轨用 build-cxl 不同 build dir。 |
| **C7** | sweep 输出严格符合 `scaling_ycsb_spec.md §6` 目录布局 | sweep 完即跑 `scripts/plot_iter11A.py <sweep_dir>` + `scripts/plot_iter12A_best_per_workload.py <sweep_dir>` 生成全部 spec §6 PNG 集合。**每次完整 ycsb scaling sweep 后必须画图**（Phase 0.A, Phase 1.5, Phase 2.5 三次都要），iter summary 必须引用关键 PNG 作为可视证据。 |
| **C8** | 性能数据不允许"single-best-rep" 报告 | 代表性 cell 对比用 5-rep median + p99；sweep 用 single-rep 但走 spec gate 5 anomaly scan |
| **C9** | 不允许 silent fallback 退化 | reserved segment 用满 / RCU grace timeout 等 fallback 路径触发时必须 fprintf 警告 + 计数；iter summary 必须 report 触发次数 |

---

## Open questions — RESOLVED 2026-05-17

> **QR1 — RESOLVED (a)**: RCU epoch storage = 纯 DRAM (per-host)。Reader 写自己 DRAM epoch counter (cheap)；owner free 时跨 host 拉取所有 reader 的 epoch (用控制 ring / CXL mirror，低频)。

> **QR2 — RESOLVED (a)**: Hazard pointer = 每 reader thread 1 slot (FUSEE read fast-path 单 outstanding per thread)。

> **QR3 — RESOLVED (b)**: W1 reserved segment 默认 30% 给 peer，用满 fallback 到 staging copy 路径 + fprintf 警告 + counter 计入 iter summary。

> **QR4 — RESOLVED (sweep)**: W3 batch size K 不预设固定值——用 workloada 在 T={4, 16, 32, 64} × K={16, 32, 64, 128, 256, 512, 1024, 2048, 4096} 跑 3 reps 取 median。挑选标准：highest median throughput AND median w_p99 not regressed > 20 % from K=16。结果写 Phase 2.2.E。

> **QR5 — RESOLVED (b)**: Phase 1 只比 read (写仍用 staging copy)；Phase 2 只比 write (读用 Phase 1 winner)。**不**跑全 4 笛卡尔组合。

> **QR6 — RESOLVED (a + G1)**: W1 block lifecycle = **G1 (RCU-defer + retire list)**——
> - Bucket pointer update 时 push old_blk_off 到 owner 的 per-host retire queue (1 atomic push, ~10 ns)
> - retire queue 满 OR bump pointer 到段末时: `rcu_synchronize()` + free retired blocks + 重置 bump pointer
> - 复用 Phase 1 RCU 基础设施
> - 稳态 throughput = G3 (never-free); worst case 不会 stall (G3 会)
> - 由 Phase 1 RCU 实现完成后才能进 Phase 2.1 (W1 依赖 RCU)
>
> **Push-back resolutions**:
> - **C5**: C17 不约束 iter-13A 新模块；只约束旧代码 bug fix (已写入 hard constraint table)
> - **编译开关**: `-DFUSEE_READ_GUARD={STAGING,RCU,HAZARD}` + `-DFUSEE_WRITE_ALLOC={STAGING,RESERVED,BATCHED}` 同源码不同 build dir
> - **代表性 cell list**: Claude 选: workloada T=4 off kv=512, workloada T=32 off kv=1024, workloadb T=32 off kv=256, workloadc T=64 off kv=1024, workloadf T=32 off kv=512

---

## Phase 0 — Pre-flight + iter-12A baseline 数据归档

### 0.A iter-12A post-fix sweep (DONE 2026-05-17)
- 命令: `bash scripts/iter12A_post_fix_sweep.sh docs/g34_scaling_ycsb_iter12A_postfix_<ts>/`
- 输入: 当前 `feat/cxl-migration` commit `2e61487` 的 build-cxl binary
- 输出: `docs/g34_scaling_ycsb_iter12A_postfix_20260517_022409/SUMMARY.log` + 86 PNGs + 42 extras
- 画图命令 (已执行):
  ```
  python3 scripts/plot_iter11A.py docs/g34_scaling_ycsb_iter12A_postfix_<ts>/
  python3 scripts/plot_iter12A_best_per_workload.py docs/g34_scaling_ycsb_iter12A_postfix_<ts>/
  ```
- **iter-13A baseline = 这次 sweep 数据**。所有后续 throughput/latency 改善对此比较。
- 结果: 209/210 OK, 1 FAIL (transient timeout, re-verify 5/5 OK), **0 collapse**, peak workloada 11.59 / b 19.00 / c 18.93 / d 17.91 / f 17.01 Mops/s. iter-12A Phase 5 stale-head fix 全 scale verified.

### 0.B Anomaly scan + gate 5 verify
- `gap_to_target.md` 必含 anomaly section
- 任何 outlier 必须 5-rep verify 或 cited 解释
- iter-12A Phase 5 stale-head fix 应该把 bimodal rate 降到 0；如果还有任何 cell 显示 collapse → 立即 STOP 报警，开新 RCA

### 0.C 写 iter-13A baseline summary
- `docs/iters/iter13A_baseline_summary.md`
- 内容：5 workload × 7 T × 2 cache × 3 KV peak Mops/s 表，对比 design_goals.md 20 Mops/s 缺口

---

## Phase 1 — 读路径双轨实现 + 对比 + 全 sweep verify

### 1.0 RAP for both 候选 (`docs/iters/iter13A_phase1_read_rap.md`)
每个候选独立 §XIII RAP（STATE / 6 ATTACK VECTORS / ABLATION / PRIOR ART / VERDICT / DECISION）。已在前面对话中讨论过 outline，正式写文档时填实。

### 1.1 候选 A 实现：RCU + epoch

#### 1.1.A 新文件 `src/cxl_rcu.h` + `src/cxl_rcu.cc`
- `struct CxlRcuDomain { atomic<uint64> publish_epoch; per_thread_epoch_slot[MAX_THREADS]; }` — per-host DRAM
- API:
  - `rcu_enter() -> uint64 my_epoch` — reader 进入 critical region；本 thread 的 epoch slot 写 publish_epoch.load()
  - `rcu_exit()` — 本 thread 的 epoch slot 写 0（idle 标记）
  - `rcu_synchronize()` — owner free 前调用，wait 直到所有 thread 的 epoch slot 要么 == 0 要么 >= caller's epoch_at_call
  - `rcu_advance_epoch()` — owner free 前 publish_epoch.fetch_add(1)
- Cross-host visibility: per-thread epoch slot 在 DRAM；owner 用 RPC（轻量 ring message）询问对端"你的 active reader 谁还在 epoch ≤ X"

#### 1.1.B 修改 `cxl_kv_ops_A.cc::read_handler`
- 替换 `pool_->read(blk_off+4, st->value_bytes, vlen)` 为 publish `(blk_off, vlen)` 到 ReadStaging 控制字段（无 value bytes）
- Reader 端 (`forward_read_direct`) 拿到 `(blk_off, vlen)` 后 `rcu_enter()` → `pool_->read` 直接读 CXL → `rcu_exit()`
- Free 路径（execute_write_local 替换旧 block 时）：rcu_advance_epoch → rcu_synchronize → free

#### 1.1.C 编译开关
- `-DFUSEE_READ_GUARD=RCU` 启用本路径
- Default `STAGING` 走旧路径

### 1.2 候选 C 实现：Hazard pointers

#### 1.2.A 新文件 `src/cxl_hazard.h` + `src/cxl_hazard.cc`
- `struct CxlHazardDomain { per_thread_slot[MAX_THREADS]; }` — per-host DRAM，slot 内容 = active blk_off 或 0
- API:
  - `hazard_protect(blk_off)` — 写本 thread slot = blk_off + sfence
  - `hazard_release()` — 写本 thread slot = 0
  - `hazard_scan(blk_off) -> bool` — 扫描所有 slot，若有 == blk_off 返回 true（"还有人持有"）
  - `hazard_retire(blk_off, free_fn)` — 加入 owner-local retire list；retire list 满时 scan 决定哪些可以真 free

#### 1.2.B 修改 `read_handler` + `forward_read_direct`
- 类似 1.1.B，把 `rcu_enter/exit` 换成 `hazard_protect/release`

#### 1.2.C 编译开关
- `-DFUSEE_READ_GUARD=HAZARD`

### 1.3 双轨对比（5 代表性 cell × 5 reps）
- Build 三个 binary: `build-cxl` (STAGING baseline), `build-cxl-rcu` (RCU), `build-cxl-hazard` (HAZARD)
- 跑代表性 cell on 各 build
- 输出 `docs/iter13A_phase1_compare/SUMMARY.tsv`：每 cell × build 的 (r_avg, r_p99, agg_thpt) median
- 写 `docs/iter13A_phase1_compare/VERDICT.md` 引用数字，pick winner

### 1.4 G1 hash-diff battery (winner only)
- Winner build × hash-diff 20 cells must all PASS

### 1.5 Full scaling sweep (winner only)
- `bash scripts/iter12A_post_fix_sweep.sh docs/g34_scaling_ycsb_iter13A_phase1_<ts>/` 但用 winner build
- Anomaly scan + gate 5 PASS
- 与 Phase 0.A baseline 对比，写 `docs/iter13A_phase1_sweep.md`

---

## Phase 2 — 写路径双轨实现 + 对比 + 全 sweep verify

### 2.0 RAP for both 候选 (`docs/iters/iter13A_phase2_write_rap.md`)

### 2.1 候选 W1 实现：Per-host reserved segments

#### 2.1.A 修改 `src/cxl_kv_blockpool.{h,cc}`
- Pool layout 变成 N+1 段：1 private + N peer-reserved（N = num_hosts - 1）
- 新增 API:
  - `pool_alloc_local(size) -> blk_off` — 在 private 段 bump（host 自己 owner key）
  - `pool_alloc_peer(peer_host, size) -> blk_off` — 在 peer 给我的 reserved 段 bump
  - `pool_free(blk_off)` — 根据 blk_off 落在哪段 → 在对应 bump pointer 上做 free（按 QR6 协议）
- Bump pointer 都在 alloc-host 自己 DRAM，跨 host 不共享

#### 2.1.B 修改 `cxl_kv_ops_A.cc::forward_write_direct`
- 替换 `memcpy(staging, value, vlen) + flush_line` 为：
  - `blk_off = pool_alloc_peer(owner, vlen)` — 本地 bump
  - `pool_->write(blk_off, value, vlen)` — 直接 DRAM→CXL 写到 owner pool
- WriteRing entry payload 改成 `(key, blk_off, vlen, op_kind)` — 不带 value bytes

#### 2.1.C 修改 `cxl_kv_ops_A.cc::write_handler`
- 直接读 e->blk_off + vlen，更新 `bucket->slots[].value = encode(blk_off, vlen, size_class)`
- 不再 staging→DRAM→pool 的 copy

#### 2.1.D 编译开关
- `-DFUSEE_WRITE_ALLOC=RESERVED`

### 2.2 候选 W3 实现：Batched pre-allocation

#### 2.2.A 新文件 `src/cxl_reserved_queue.{h,cc}`
- 每 worker thread 维护 per-thread reserved queue: `{blk_off[Q], next_use_idx}`
- API:
  - `queue_refill(owner_host)` — 发一个控制 message 到 owner，请求 K blocks；owner 回复 K blk_off
  - `queue_pop(owner_host) -> blk_off` — 取一个；若空触发 refill

#### 2.2.B 修改 owner 端
- 新增 `ReservationRing[A][B]` 控制 channel；owner 收到 `reserve(K)` 请求后 alloc K → 回包 K blk_off

#### 2.2.C 修改 `forward_write_direct`
- 类似 W1，但 `blk_off = queue_pop(owner)`（worker 维护本地 queue）

#### 2.2.D 编译开关
- `-DFUSEE_WRITE_ALLOC=BATCHED`

#### 2.2.E W3 batch size K sweep (per QR4)
- Workload: `workloada` (50/50 R/U Zipf, 对 allocation 频率最敏感)
- Cells: T = {4, 16, 32, 64}; cache = off (要让 cross-host 写走真路径); kv = 1024
- K values: {16, 32, 64, 128, 256, 512, 1024, 2048, 4096} — 9 个
- Reps: 3 per (T, K), 取 median
- 共 4 × 9 × 3 = 108 runs
- 输出: `docs/iter13A_phase2_w3_ksweep/SUMMARY.tsv`
- 挑选 K_winner: highest median throughput AND median w_p99 not regressed > 20 % from K=16
- 后续 Phase 2.3 W3 用此 K_winner 跑代表性 cell 对比

### 2.3 双轨对比
- 同 Phase 1.3 pattern，Phase 1 winner 已固定 → 只切 WRITE_ALLOC

### 2.4 G1 hash-diff battery (winner only)

### 2.5 Full scaling sweep (winner only)
- Anomaly scan + gate 5 PASS
- 与 Phase 1 sweep 对比，写 `docs/iter13A_phase2_sweep.md`

---

## Phase 3 — iter-13A summary + 完成 gate

### 3.A 写 `docs/iters/iter13A_summary_<ts>.md`
- Per spec §13 iter-completion gate
- Delivery audit table（每 sub-phase ✅/⚠/❌）
- 双 sweep（Phase 1 + Phase 2）的 peak/median 对比 iter-12A baseline
- gap_to_target 表 vs 20 Mops/s

### 3.B Backlog memo `docs/iters/iter14A_backlog_memo.md`
- 落选的双轨候选（lost track）如果有未来潜力（如不同 workload pattern 上可能 winner 反转）→ 记下
- iter-12A 遗留 Bug B（ack-visibility 2/102 timeout）→ 看 Phase 1/2 是否顺带解决了，否则 carry
- 20 Mops/s 还差多少 → 下一 iter 攻击方向

### 3.C iter-13A completion gate
- ✅ Phase 1 sweep + winner verdict committed
- ✅ Phase 2 sweep + winner verdict committed
- ✅ G1 hash-diff PASS for both winners
- ✅ §13 gate 5 anomaly scan PASS (zero unexplained outlier) for both sweeps
- ✅ Delivery audit table 全 ✅（任何 ⚠/❌ 必须 cite 前置 user 授权）

---

## 不在 iter-13A scope 内的事情

- 不动 Protocol B / C
- 不做 K-channel sender / receiver 重构
- 不动 LFM 锁实现
- 不引入新的 cache eviction 策略
- 不改 invalidate ring 协议（只改 read+write data path）
- 不做 hot-key replication（iter-14A backlog 已记）

如果 sweep 数据显示某 workload **现在**离 20 Mops/s 已经接近（比如 workloadc 已经 16-17 Mops/s），iter-13A 后续可以 surgical 加 cache prefetch 等小动作，但 **不** 当 mandatory scope。

---

## Living docs to update (per CLAUDE.md C7)

- `docs/refs/ABC_throughput_improvement_plan.md` — iter-13A section
- `docs/fusee_cxl_progress.md` — progress entry
- `CLAUDE.md` 不动（CLAUDE.md 是 project-level constraints，iter 内不轻易改）

---

## User 确认 2026-05-17

> "对，iter13A 中的是新功能 module 实现，不用考虑 c17 约束，≤ 3 site per candidate 也不一定要遵守。重要的是将 data copy 完全去除且能完全保证读路径的正确性。"
> "G1 做出来保证可用性，以后的 iter 再考虑优化"
> "不允许私自 descale，不用考虑 deadline，在 iter 完成前我将不再交互"

→ Iter-13A 进入 autonomous execution mode。Sub-phase 完成即 commit；只有遇到**非常规架构选择**（例如发现 RCU 在 g3+g4 实际不可行）才暂停 reopen task plan。
→ Hash-diff 范围: 全量 20-cell battery for both winners (Phase 1 winner + Phase 2 winner)。
→ `FUSEE_READ_GUARD` / `FUSEE_WRITE_ALLOC` 编译开关同源码不同 build dir 实现。
