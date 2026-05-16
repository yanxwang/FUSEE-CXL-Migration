# iter-12A constraint document — observe-first bimodal root-cause + R3 verification-vs-sweep bisect + protocol A summary close-out

**Author**: Claude (post iter-11A completion 2026-05-11)
**Date drafted**: 2026-05-16
**Deadline**: TBD by user (default expectation: comfortably day-scale, no descope)
**Branch**: `feat/cxl-migration`
**Predecessor**: `docs/iters/iter11A_summary_20260511.md` (iter-11A 7 phases shipped; gate-12 **❌ FAIL** at 13/210 bimodal; 0/5 workloads at 20 Mops/s; A peak workload-a 13.69 Mops/s)
**Spec refs**: `docs/design_goals.md §I-XIII`, `docs/path_decomp_spec.md`, `docs/scaling_ycsb_spec.md` (§13 gate-12 = bimodal count gate from iter-10A), `docs/protocol_a_architecture_blueprint.md` (iter-11A snapshot), `docs/protocol_a_vs_c_step_explainer.md`
**CLAUDE.md cautionary precedents**: #1 (iter-2A pre-flight descope), #2 (iter-6A scope creep + outlier dismissal), #3 (iter-9A silent minimal-version substitution), **#4 NEW (iter-11A hypothesis-without-observation, see §"new precedent" below)** — `Phase delivery audit gate` MUST appear in iter-12A summary

---

## TL;DR

iter-11A 留下 3 个未解项：

1. **Gate-12 FAIL**: 12/53-verify bimodal cells (> baseline 8 in iter-10A verify) —— iter-11A backlog #2 给的 hypothesis 是 "forwarder-pool-direct epoch retry storm" —— **完全是基于 throughput 数字反推，没有任何代码级观察证据**。**2026-05-16 cross-iter 验证 (§QR3) 进一步发现**：bimodal 在 iter-10A 已有 8 cells = gate-12 阈值，pattern 跨 iter 完全一致 → root cause 是 **protocol A 结构性 issue**，predates iter-11A，所以 iter-11A 任何单 commit revert **都治不了**。
2. **R3 verification-vs-sweep 不一致**: Phase 1 commit `a27dfda` 验证 cell 测到 r_avg = 1.4 µs / workload-c best 18.92 Mops/s，但 Phase 6 全 sweep 同 cell 测到 r_avg = 8.5 µs / 11.19 Mops/s。中间隔了 Phase 2 ship+revert 和 Phase 3/4 doc-only。丢的 6× R3 + 7.7 Mops/s 没定位。这才是 iter-11A 真正的回归（**bimodal 是老的**，R3 才是新的）。
3. **2nd-round path_decomp 没做**: Phase 6 sweep 暴露 5/5 workload 的 best cell 都跟 iter-10A 不同（多数转到 cache=off T=64），但没在新 best cell 上重测 path_decomp，所以"哪个 stage 是新的 bottleneck"是未知的。

iter-12A 用 **5 个 phase** 一次解决三项：

0. **Phase 0** — pre-flight：testbed 健康检查 + iter-11A build 复活 + baseline smoke
1. **Phase 1** — **bimodal observe-first 诊断 + 修复** — 7 sub-phase，先观察后假设 (见下文 methodology)
2. **Phase 2** — R3 regression bisect (a27dfda → 5664945 五个 commit 逐个测)
3. **Phase 3** — 2nd-round path_decomp on iter-11A new-best cells (5 cells × healthy + anomaly)
4. **Phase 4** — full 210-cell sweep + iter-12A summary + iter-13A backlog

**核心 quantitative gate (≥ 3 必须 PASS 才 declare iter complete)**:
- **G-A**: Bimodal cell count ≤ 8 (gate-12 PASS, 修复 iter-11A regression)
- **G-B**: R3 mean ≤ iter-10A baseline 9.5 µs（修复 iter-11A +22% slower 的 regression）
- **G-C**: workload-c best ≥ 17 Mops/s (恢复 Phase 1 验证 cell 测到的 18.92 Mops/s 量级)
- **G-D** (stretch): workload-a best ≥ 20 Mops/s (long-term 20 Mops/s 目标)

iter-12A 是 **stabilization + clean-up iter**，不引入新 architecture。所有新 code 只为 diagnosis 或 narrow-targeted fix。

---

## New CLAUDE.md cautionary precedent #4: "Hypothesis-without-observation"

iter-11A backlog #2 把 13/210 bimodal 归因为 "forwarder-pool-direct epoch retry storm"，但是：

- **没有 bpftrace / ftrace / gdb 抓过 collapsed rep 的实际行为**
- **没有 event-level instrumentation** 看 InvalReceiver 在 collapsed rep 是否在跑
- **没有 ring tail/head snapshot** 看是否 desync
- 完全是 "数字像 X，所以 hypothesis 是 Y" 的反向猜测

风险：基于错的 hypothesis 设计的 fix 会**凑巧**通过 hash-diff (correctness 保持) 但**完全不解决** root cause —— next iter sweep 仍然 bimodal。这跟 iter-2A descope-by-time-budget 一样是**用看似合理的推理代替实证**。

**Rule (从 iter-12A 开始强制)**：

> 调查任何 **performance anomaly / instability**（bimodal, gate-12 FAIL, throughput 反常 collapse, latency 反常 spike），**禁止在没有 code-level observation 的情况下提出 root-cause hypothesis**。Observation phase 必须使用以下至少 2 种 zero-code-change kernel-level tool：
>   - `bpftrace` / eBPF uprobe — function call rate / latency per thread
>   - `ftrace sched_switch` — preemption / scheduler trace per thread
>   - `gdb -batch -ex 'thread apply all bt'` — collapsed-state thread stack snapshot
>   - `perf record -g` — sampling profile of collapsed run
>
> Hypothesis 段只能在 observation 数据**已经入档**之后写。Hypothesis-then-experiment 顺序倒过来不行 (那是 iter-11A backlog #2 的失败模式)。

**Applied to iter-12A**: Phase 1.1-1.3 是强制的 observe-first 步骤，必须先跑完才能写 root-cause RAP (Phase 1.5)。

---

## Scope

### In scope (3 个 modify-code phase + 2 个 investigation phase, 全 mandatory)

- **Bimodal diagnostic** (Phase 1.0-1.7) —— observe-first 流程，目标 gate-12 PASS
- **R3 regression bisect** (Phase 2) —— 找出 Phase 1→Phase 6 中间哪个 commit 引入 7.7 Mops/s 丢失
- **2nd-round path_decomp** (Phase 3) —— iter-11A new-best 5 cells × best+worst on FUSEE_PROBE=1 build
- **Full sweep** (Phase 4) —— 210-cell sweep on iter-12A-final build
- **Living docs (C7)** —— 实时随每 sub-phase 同 commit 落地
- **CLAUDE.md precedent #4** —— observe-first rule 落入 `CLAUDE.md` 永久 section（Phase 1.5 commit）

### Out of scope (defer to iter-13A+)

- **Hot-key replication** (iter-11A backlog #5) —— 只在 Phase 2/3 数据确认 W10 仍 dominant 时才考虑
- **RCU cache_pool** (iter-11A backlog #6) —— 同上 deferred condition
- **Phase 2 parallel-inval redesign** (iter-11A backlog #4) —— 只有 Phase 1 诊断显示 inval path 真的是 bottleneck 才上
- **Probe-overhead attribution** (iter-11A backlog #7) —— 用 FUSEE_PROBE=0 sweep 做了基线就行，详细 attribution 推迟
- **Variable-length keys** —— 不阻塞 20 Mops/s gate
- **BucketLockTable removal** —— pure space, no perf
- **Aggregator P0/P1/P2/P3 batching** —— B0 仍是默认胜利者
- **Multi-host > 2** —— 测试床限制

### Memo for future

- Phase 1.1-1.3 (bpftrace/ftrace/gdb) 大概率会直接定位 root cause（**80% 概率**前 3 步够）。Phase 1.4 (写 event trace) 是 fallback。
- 用户先验 (QR2)：**scheduling preempt 不太可能是 root cause** —— 倾向于 code/state-machine bug。Phase 1.2 ftrace 仍然必跑做证伪 ([V1 verdict]) 但更看重 Phase 1.1 bpftrace function call rate + Phase 1.3 gdb thread snapshot。
- 如果 Phase 1 定位是 **ring head/tail desync** (H1)，fix 是 receiver 加 gap-tolerance；要做 G1 hash-diff 验证 strict-A 不破。
- 如果 Phase 1 root cause 是 **小 KV + Zipf + 多 worker 争同 bucket lock 的死循环 retry pattern** (跟 cross-iter pattern 吻合)，fix 路径走 narrow CAS / yield 优化 (per C17 不允许大改)。
- 如果 Phase 2 bisect 显示是 Phase 2 (parallel-inval) revert **不彻底**，per QR3 走 **fix-in-place**（找到具体 leak path 改回），不再 default 走 revert。
- 如果 Phase 3 path_decomp 显示 R3 在 new-best cell 上**仍然好**（< 9.5 µs），说明 iter-11A Phase 1 forwarder-pool-direct 在 sweep 配置下其实没失效，是 Phase 1 验证 cell 跟 sweep 用了不同 build/env —— 这种情况下 G-C target 应该可以拿到。

---

## Hard constraints (违反 = iter 重做)

继承 iter-11A 的 C1-C15，新增 C16-C18。

| 约束 | 验证手段 |
|---|---|
| **C1-C7** | 同前 iter（继承且必须保持）：变长 KV、no value bytes on message ring、全线程 CPU pin、N:1:1:N runtime active assert、G1 hash-diff at 全 KV×workload、path_decomp 全跑、living docs realtime |
| **C8** TLS cache 跟 shared cache_pool 强一致 (§I9 strict-A 不变) | 同 iter-10A/11A —— 任何修改 cache_pool 或 InvalReceiver 的 phase 后跑 G1 hash-diff 20/20 PASS |
| **C9** 多 build hash-diff 全 KV × workload | Phase 1.6 (fix 后) + Phase 2 (bisect 中) + Phase 3 (new-best path_decomp build) + Phase 4 (final sweep build) 各跑一次 G1 hash-diff battery (20 cells)。任一 build FAIL = phase invalid |
| **C10** 5-workload × 2-cell × 14-stage path_decomp | Phase 3 在 iter-11A Phase 6 sweep 的 new-best+worst cells 上重测；any unjustified `✱ no data` 行 = Phase 3 不 exit |
| **C11** TLS size 验证 | sweet spot 1024 继承，不重 sweep |
| **C12** Bimodal cell count gate (iter-11A NEW, iter-12A 必须修复) | Phase 1.7 5-rep verify on iter-11A 13 bimodal cells + Phase 4 全 sweep 后任何 anomaly cell。**post-fix count ≤ 8** = gate-12 PASS；若 > 8 = Phase 1 fix 不彻底，必须根本性 redo 或 root-cause refine |
| **C13** Forwarder-pool-direct §I9 不变性 | Phase 1 fix 不能改 C13 invariant；G1 hash-diff Phase 1.6 完 20/20 |
| **C14** Parallel inval drain ordering | iter-11A Phase 2 已 revert；iter-12A 不重启 parallel inval，C14 沿用（如果 Phase 1 诊断显示 parallel inval 必须重做，**stop and ask user** 不擅自 reintroduce） |
| **C15** Hot-bucket sharding dynamic + cold-path zero-cost | iter-11A Phase 3 doc-only revert；iter-12A 不引入；C15 sleeping invariant |
| **C16 (NEW)** Observe-first 诊断 discipline | 任何 perf anomaly 调查必须先用 bpftrace + ftrace + gdb 至少 2 种 zero-code tool 采集 raw observation 后才能写 hypothesis。Phase 1.5 RAP 必须 cite 至少 2 种 observation 数据作为 root cause 依据。**任何 hypothesis 段在 Phase 1.1-1.4 完整数据之前写 = phase invalid** |
| **C17 (NEW)** Fix design 必须 narrow-targeted | Phase 1.6 fix code 必须**直接对应** Phase 1.5 RAP 里 cite 的具体 observation（line-of-code level）。不允许 "顺便" 加 unrelated 优化。任何 fix beyond 3 处 (file × function) 必须**stop and ask user**。这是防 iter-9A scope creep precedent #3 的护栏 |
| **C18 (NEW)** Bisect commit-level granularity | Phase 2 R3 regression bisect 必须**逐 commit 测**（a27dfda, 455379e, 5664945, 3469388, e4c682b 五个 commit on workload-c best cell）；不允许"看起来这个 commit 没改 read path 所以跳过"。Bisect 出来的"first bad commit" 必须**复现 ≥ 5 reps** 确认稳定 |

---

## Living docs to update (per C7)

| 文档 | 改的章节 | 触发 phase |
|---|---|---|
| `CLAUDE.md` | 新加 "cautionary precedent #4: hypothesis-without-observation"，行文 mirror precedent #1/2/3 | **Phase 1.5 完时同 commit** (C16 强制 codify) |
| `docs/scaling_ycsb_spec.md` | §13 gate-X (NEW iter-12A) — observe-first 诊断 protocol (引用 CLAUDE.md precedent #4) | Phase 1.5 完 |
| `docs/path_decomp_spec.md` | §11 reference instances — 加 iter-12A new-best 5-cell decomp entries | Phase 3 完 |
| `docs/protocol_a_architecture_blueprint.md` | Part III.3 failure modes —— bimodal 真实 root cause + fix (替换 iter-11A 的"怀疑 forwarder-pool-direct retry storm" 占位文字) | Phase 1.5 完 |
| `docs/protocol_a_architecture_blueprint.md` | Part II.4 / II.5 (invalidate path) —— 如果 fix 涉及 receiver 行为变化 (gap-tolerance, SCHED_FIFO) 同步描述 | Phase 1.6 完 |
| `docs/iters/iter11A_summary_20260511.md` | 加 "iter-12A redress note" 一行附录：bimodal 真实 root cause + 落点 commit | Phase 1.6 完 |
| `docs/fusee_cxl_progress.md` | iter-12A 进度行 | 每 phase 完 |

---

## Open questions — RESOLVED 2026-05-16

| QR | 用户答复 | 落入 plan 的执行规则 |
|---|---|---|
| **QR1** Phase 1.4 触发条件 | **默认** —— 若 Phase 1.1-1.3 给出 strong root-cause evidence (`Phase 1.3.5 verdict` 能 cite 具体 code line + observation row) 即跳过 1.4 | Phase 1.3.5 verdict gate 严格执行；不为"凑齐 tool"跑 1.4 |
| **QR2** SCHED_FIFO/isolcpus level | **L1 only** —— 仅允许 user-space `pthread_setschedparam` + `pthread_setaffinity_np`；L2 (systemd cpuset) / L3 (kernel cmdline isolcpus + reboot) **在本 iter 内禁止**，若 Phase 1 数据指向需要 L2/L3 → **stop and ask user**。用户判断"scheduling stall 极不可能是 root cause" | Phase 1.6 Fix-A 删除 isolcpus 选项；reserve L2/L3 fallback for iter-13A 或 user explicit re-permission |
| **QR3** 若 bisect 指向 Phase 1 commit, revert? | **不 revert, fix-in-place**。**关键新信息**：用户提示 bimodal 在 iter-9A / iter-10A 也存在 → 不是 iter-11A 引入。已 2026-05-16 数据 cross-check 确认（见下 §QR3 验证） | Phase 2 范围**重塑**——bisect 不再服务"找 bimodal 引入 commit"，仅服务 **R3 verification (1.4µs) vs sweep (8.5µs) 6× regression**。bimodal 走 Phase 1 结构性 root cause 路径 |
| **QR4** 时间预算 | 不担心 deadline，按 CLAUDE.md iter discipline 全推进 | 不做 time-driven descope，每 phase 跑完做 audit 再推进 |

### QR3 cross-check (2026-05-16 数据验证)

| Sweep | 5-rep verify cells | BIMODAL count (max≥5×med ∧ med<0.5 ∧ max≥1.0) |
|---|---:|---:|
| iter-10A (2026-05-10) | 37 | **8** (= gate-12 阈值) |
| iter-11A (2026-05-11) | 53 | **12** (>> gate-12) |
| iter-9A_redo (2026-05-10) | — | 未跑 5-rep verify（方法论尚未引入）|

Cross-iter pattern identical:
- Collapsed throughput quantized at 0.001–0.25 Mops/s
- Winning rep concentrated at edge position (rep 1 or 5)
- 集中在 small KV (256/512) + Zipf hot key (workload-a/d/f) + T ≥ 4

→ Bimodal **predates iter-11A**，是 protocol A 结构性 issue。iter-11A 让它**加剧**（8 → 12），但 root cause 在更早的 commit / 更深的设计层。Phase 2 bisect window 重写见下。

---

## Phase 0 — Pre-flight

**目的**: 确认 g3+g4 健康 + iter-11A build 还能跑 + baseline bimodal 复现稳定。

### Phase 0.A —— testbed 健康检查

```bash
ssh g3 'cat /etc/issue; uname -r; daxctl list; ls -la /dev/dax0.0'
ssh g4 'cat /etc/issue; uname -r; daxctl list; ls -la /dev/dax0.0'
```

预期：
- kernel 6.15.0+
- /dev/dax0.0 在 devdax 模式
- 任一 host 不通 → 走 memory `feedback_rekey_slave`: rekey + bootstrap + workloads rsync + daxctl reconfigure

### Phase 0.B —— iter-11A build 复活 + smoke

```bash
ssh g3 'cd /root/FUSEE_CXL/build-cxl && cmake -DFUSEE_PROBE=0 . && make -j16 protocol_a_ycsb' &
ssh g4 'cd /root/FUSEE_CXL/build-cxl && cmake -DFUSEE_PROBE=0 . && make -j16 protocol_a_ycsb' &
wait

# Smoke test (workload-a T=1 cache=on kv=256)
bash scripts/iter11A_sweep.sh /tmp/iter12A_phase0_smoke
# 期望 1 cell ≥ 0.3 Mops/s smoke PASS
```

### Phase 0.C —— bimodal cell baseline 复现 (含 cross-iter cell)

挑 **3 个 cross-iter 验证过最 reproducible 的 cell**（per QR3 验证 §：iter-10A + iter-11A 都 BIMODAL 的稳定 cell 优先）：
- `workloada T=4 cache=off kv=512` ★ **Phase 1 reference cell** (iter-10A 1/5 win, iter-11A 1/5 win — cross-iter 同 pattern)
- `workloada T=64 cache=off kv=256` (iter-11A only, 4/5 collapse, 1/5 win at rep 1，high T 控制变量)
- `workloadd T=4 cache=on kv=256` (iter-11A only, 4/5 collapse, 用来排除 workload-a-only effect)

每 cell 跑 20 reps（不是 5），看 collapse rate 是不是稳定在 ~75-80%。

**Exit condition**: collapse rate 在 60-90% 范围内 → 确认可复现；否则先 root-cause 为什么 baseline 漂了。**优先级**：reference cell `workloada T=4 cache=off kv=512` 若不复现 collapse → 切 iter-11A commit `e4c682b` build 重测；仍不复现 → stop and ask user 调整 reference cell。

---

## Phase 1 — Bimodal observe-first 诊断 + 修复

**核心 methodology**: **Observe → Hypothesize → Validate → Fix**，绝不允许跳过 Observe 直接 Hypothesize（per C16 + CLAUDE.md precedent #4）。

7 sub-phase 顺序 + exit condition：

### Phase 1.0 —— 选 1 个 canonical bimodal cell + 复现 harness

挑 `workloada T=4 cache=off kv=512` 作为**整个 Phase 1 的 reference cell**（**iter-10A + iter-11A 都 bimodal** 的 cross-iter 稳定 cell，证明 root cause 是结构性的）：

| 来源 | reps | min | med | max | 类型 |
|---|---:|---:|---:|---:|---|
| iter-10A verify | 5 | 0.006 | 0.006 | 1.744 | BIMODAL (1/5 win) |
| iter-11A verify | 5 | 0.000 | 0.006 | 1.460 | BIMODAL (1/5 win) |

Cross-iter 同一 cell 同 pattern → root cause 跨 commit/build 稳定，最适合做 reference。

辅助 cell（Phase 1.7 5-rep verify 用，**不**作 reference cell）：
- `workloada T=64 off kv=256` (iter-11A only, 12.469 max, 4/5 collapse)
- `workloadd T=4 on kv=256` (iter-11A only, 1.414 max, 4/5 collapse)
- `workloada T=4 on kv=512` (iter-10A 1.551 max, 4/5 collapse)

原因：
- workload-a T=4 是 cross-iter 稳定复现，root cause 不依赖 sweep 配置变动
- 小 KV (512) + workload-a hot Zipf → 经过两 iter 验证最容易 trigger
- workload-a 是 50/50 RW，cover write + read + inval 全部 path
- 低 T=4 排除 "scheduling stall" 类 hypothesis（per QR2 用户判断 scheduling 不太可能）—— T=4 时 worker 数 < core 数，OS preempt 不该是问题

写 `scripts/iter12A_repro_cell.sh`:
```bash
#!/bin/bash
# 复现单个 bimodal cell N reps，输出 per-rep throughput + r_avg
CELL="workloada T=4 cache=off kv=512"
REPS=10
OUTDIR="$1"
mkdir -p "$OUTDIR"
for rep in $(seq 1 $REPS); do
  cookie=$(date +%s%N)
  # ssh g3/g4 with COMMON_ENV (per iter-11A sweep 模板)
  # capture stdout/stderr per host
done
# parse 出 N 个 trans_agg_thpt + r_avg_ns + w_avg_ns 并 print
```

**Exit**: 跑 10 reps, 至少 6-8 collapse (< 0.1 Mops/s)，至少 1-2 win (> 1.0 Mops/s)，证明 reference cell 可重复触发 bimodal。Collapse rate 应落在 60-90% 范围（与 iter-10A/11A pattern 一致）。

如果 reference cell 在 g3/g4 当前 build 上**完全不 collapse**（10/10 win），意味着环境/build 漂移；切回 iter-11A commit `e4c682b` build 重测。仍不 collapse → ask user 是否选别的 cell 或者宣布 "bimodal 自然消失了"（不太可能但走 due process）。

### Phase 1.1 —— bpftrace 函数级 observation (zero code)

**Goal**: 看 collapsed rep 里哪个函数 latency 变了或停跑了。

3 个并行的 bpftrace 实验：

**Experiment 1.1.A** —— receiver loop 迭代速率
```bash
sudo bpftrace -e '
uprobe:./protocol_a_ycsb:_ZN5fusee*inval_receiver_loop* { @inval_polls[tid] = count(); }
uprobe:./protocol_a_ycsb:_ZN5fusee*write_receiver_loop* { @write_polls[tid] = count(); }
uprobe:./protocol_a_ycsb:_ZN5fusee*read_receiver_loop*  { @read_polls[tid] = count(); }
interval:s:1 {
  printf("%d %d %d %d\n", elapsed_s, sum(@inval_polls), sum(@write_polls), sum(@read_polls));
}'
```
对比 winning rep vs collapsed rep —— 每秒 poll 数差 100×？

**Experiment 1.1.B** —— send_invalidate_direct timeout 频率
```bash
sudo bpftrace -e '
uretprobe:./protocol_a_ycsb:_ZN5fusee*send_invalidate_direct* /retval == -11/ {
  @timeouts = count();
}
interval:s:1 { print(@timeouts); }'
```
collapsed rep 里 timeout/sec 是 0 还是几千？

**Experiment 1.1.C** —— forward_read_direct latency hist
```bash
sudo bpftrace -e '
uprobe:./protocol_a_ycsb:_ZN5fusee*forward_read_direct* { @t[tid] = nsecs; }
uretprobe:./protocol_a_ycsb:_ZN5fusee*forward_read_direct* /@t[tid]/ {
  @lat = hist((nsecs - @t[tid]) / 1000);
  delete(@t[tid]);
}'
```
healthy rep 应该看到 ~10-50 µs；collapsed rep 是不是大量 5000 µs+ 的 timeout bucket？

**Output** in `docs/iter12A_diagnostic/phase_1_1_bpftrace/`：每个实验 collapsed_rep + winning_rep 各 1 份 raw bpftrace output。

**Exit**:
- 至少**找到 1 个 function** 在 collapsed rep 里行为明显异常（poll rate 掉 100×，或 latency 长尾跑出 timeout 范围）
- 数据归档到 disk
- **禁止开始写 hypothesis**；只 record finding

### Phase 1.2 —— ftrace sched_switch (zero code)

**Goal**: 验证 receiver thread 是不是被 OS preempt。

```bash
# 找到 receiver tid
RECV_TID=$(ssh g3 'ps -L -p $(pgrep -f protocol_a_ycsb | head -1) -o tid,comm | grep InvalReceiver | awk "{print \$1}"')

ssh g3 "sudo trace-cmd record -e sched_switch -P $RECV_TID -- sleep 30 &"
# 然后 collapsed cell 跑
bash scripts/iter12A_repro_cell.sh /tmp/p1_2

ssh g3 'sudo trace-cmd report > /tmp/p1_2_sched.txt'
# 分析 inval_recv sched_switch 之间的 gap, 找 > 10ms 的 preempt 事件
```

类似 trace 同时对 WriteReceiver / ReadReceiver。

**Output**: `docs/iter12A_diagnostic/phase_1_2_ftrace/{inval,write,read}_sched_switch.txt`

**Exit**:
- Record receiver thread 的 sched_switch timeline
- 看到 `prev_state == R` 但 next 切走 ≥ 10ms 的事件 → **preempt evidence**
- 或者 receiver thread 一直 running，没有 preempt → preempt hypothesis 否决

### Phase 1.3 —— gdb attach + collapsed-state thread snapshot (zero code)

**Goal**: 抓 collapsed rep 在某个瞬间所有 thread 的 stack + 关键变量值。

```bash
# 启动 collapsed cell，sleep 到 trans phase 跑到 10s
bash scripts/iter12A_repro_cell.sh /tmp/p1_3 &
sleep 12  # 此时 collapsed rep 应该在 trans phase 中段

# Host 0 上 gdb -batch
ssh g3 "sudo gdb -p \$(pgrep -f protocol_a_ycsb | head -1) -batch \
  -ex 'set pagination off' \
  -ex 'info threads' \
  -ex 'thread apply all bt 20' \
  -ex 'thread apply all p \"=== thread ===\"' \
  > /tmp/p1_3_host0_bt.txt 2>&1"

# Host 1 同样
ssh g4 "sudo gdb -p \$(pgrep -f protocol_a_ycsb | head -1) -batch \
  -ex 'thread apply all bt 20' \
  > /tmp/p1_3_host1_bt.txt 2>&1"

# 抓 ring head/tail (用 gdb 直接读 struct fields)
# 需要 attach 时 ring matrix pointer 可访问
```

**Output**: `docs/iter12A_diagnostic/phase_1_3_gdb/{host0,host1}_bt.txt`

**Exit**:
- 64 worker stack + 6 receiver stack 都 capture
- 任何明显 stuck pattern (大量 worker 在 `generic_spin_wait`, receiver 在 `inval_receiver_loop` 但 head < tail 不变, 等) → record
- **禁止**仍然不写 hypothesis；只 record observations

### Phase 1.3.5 —— observation gate (mandatory checkpoint)

完成 Phase 1.1-1.3 后，**stop**。看 3 类 observation 数据：

| Data 类型 | 来源 | 关键问题 |
|---|---|---|
| Per-function latency / call rate | bpftrace 1.1 A/B/C | InvalReceiver / WriteReceiver 是否还在 poll? send_invalidate_direct timeout 频率？|
| Receiver thread scheduling | ftrace 1.2 | Receiver 是否被 OS preempt?  preempt gap 多大? |
| Stuck thread snapshot | gdb 1.3 | 哪些线程 stuck 在哪一行?  ring head/tail 是什么状态? |

**Verdict 4 种可能**：

- **V1: Receiver preempted >10ms** → root cause: scheduling (H5). Fix path: Phase 1.6 SCHED_FIFO **L1 only** 实施（per QR2，禁 isolcpus / systemd cpuset）。**Skip Phase 1.4**. 用户先验：V1 概率低，须 Phase 1.1 + Phase 1.3 同时支持才接受 V1。
- **V2: Receiver running 但 head/tail desync** → root cause: ring state machine bug (H1). Fix path: Phase 1.6 receiver gap-tolerance. **Skip Phase 1.4**.
- **V3: Receiver running, ring 正常, 但 worker spin 在某个意外 path** (cross-iter bimodal pattern 暗示这个可能性最高 —— 小 KV + Zipf 单 hot bucket 上的 spin/retry 死循环) → 新 hypothesis（前面没想到的）。可能进 **Phase 1.4 写 event trace** 获取更多信息。
- **V4: 数据噪声大不足以指向任何一类** → **进 Phase 1.4 写 event trace** 系统化采集。

**Exit**: 写 `docs/iter12A_diagnostic/phase_1_3_5_observation_verdict.md`，cite 具体 observation 数据 row → 给出 V1/V2/V3/V4 verdict。

### Phase 1.4 —— Event trace 实施（**仅在 1.3.5 verdict = V3 或 V4 时执行**）

如果 V1/V2 → 跳到 Phase 1.5。

如果 V3/V4 → 写 `src/cxl_event_trace.h` (~300 LOC):

- Per-thread mmap'd ring buffer (同 cxl_probe.h pattern，独立 trace namespace)
- 12-16 event kinds: `EV_FORWARD_WRITE_ENTER/TAIL_FETCH/PUBLISH/SPIN_TIMEOUT/RETURN`, `EV_WRITE_RECV_POLL/HANDLE/PUBLISH_ACK`, `EV_INVAL_*`, `EV_READ_*`
- 每 event 24 B (tsc + kind + op_id_lo + aux_a + aux_b)
- Post-process tool `scripts/iter12A_event_trace_analyze.py` 把所有 thread 的 event log merge 出时间线

跑 reference cell, dump event trace, 分析。

**Exit**: 找出 collapsed rep 跟 winning rep 在 event 时间线上的 first divergence point。

### Phase 1.5 —— Root cause RAP (Reviewer Attack Process)

**Goal**: 把 Phase 1.1-1.4 数据写成 root-cause RAP per `docs/design_goals.md §XIII`，**所有 hypothesis 必须 cite observation row**。

文件: `docs/iters/iter12A_bimodal_rca.md`

格式（per §XIII）:
- **STATE**: 根因一句话陈述（≤ 30 字）
- **ATTACK VECTORS** (≥ 6 categories): PERFORMANCE / CORRECTNESS / GENERALITY / COMPLEXITY / PRIOR ART / IMPLEMENTATION FEASIBILITY
- **ABLATION CHECK**: fix 移除后该 observation 是否消失？
- **PRIOR ART CHECK**: 这个 fix 在其他系统（Linux kernel、distributed systems）有 precedent 吗？
- **VERDICT**: 这个 root cause 跟 observation 数据是否 100% consistent
- **DECISION**: 进 Phase 1.6 fix design 还是回 Phase 1.4 补 observation

**Critical**: 每个 hypothesis 必须 attached `docs/iter12A_diagnostic/phase_1_X/...` 里的具体 data row。

**Exit**: RAP 写完 + 经过自己的 6+ attack vector review + DECISION = "proceed to Phase 1.6"。

### Phase 1.6 —— Fix design + 实施

**Goal**: 基于 1.5 RAP 实施 narrow-targeted fix（per C17：beyond 3 file × function 必须 stop and ask user）。

可能的 fix 类型（视 verdict）：

**Fix-A (V1 preempt — 用户判断概率低)**:
```c
// src/cxl_kv_ops_A.cc enable_invalidate / enable_read_ring / enable_write_ring
struct sched_param p; p.sched_priority = 80;
pthread_setschedparam(thread, SCHED_FIFO, &p);
```
**per QR2 L1 only**：仅 user-space pthread；**kernel cmdline (isolcpus/nohz_full) + systemd cpuset 在本 iter 禁止**。若 Phase 1.3.5 verdict 确实 = V1 且 L1 fix 不足以闭合，**stop and ask user** 是否允许 escalate 到 L2/L3。**用户 prior**: scheduling 极不可能是 bimodal root cause，所以 V1 verdict 应当被 Phase 1.1-1.3 充分 cross-check 后才接受。

**Fix-B (V2 ring desync)**:
```c
// src/cxl_kv_ops_A.cc inval_receiver_loop / write_receiver_loop / read_receiver_loop
while (head < tail && gap_tolerance < kMaxGap) {
  // ... existing logic ...
  if (op_id == 0) {
    head++;                    // ★ advance head past zero slot ★
    gap_tolerance++;
    continue;
  }
  gap_tolerance = 0;
  // ... process ...
}
```

**Fix-C (V3/V4 其他)**: 视 RAP 具体内容

**Hash-diff battery (C9 强制)**: fix 完后立刻跑 20-cell G1 hash-diff，全 PASS 才进 Phase 1.7。

**Exit**: code committed + hash-diff 20/20 PASS + Phase 1.5 RAP cited 的所有 observation 已被 fix 覆盖。

### Phase 1.7 —— Bimodal cell 5-rep verify (gate-12 check)

```bash
# 跑 iter-11A 13 bimodal cells + reference cell 5 reps each
bash scripts/iter10A_anomaly_5rep_verify.sh /tmp/iter12A_p1_7_verify \
  docs/g34_scaling_ycsb_iter11A_20260511_042814/anomaly_cells.txt

# 分类 (bimodal: max >= 5*median && median < 0.5 && max >= 1.0)
python3 scripts/iter11A_anomaly_scan.py --summary ... --out ...
```

**Exit (G-A gate)**: bimodal count **≤ 8** = PASS。如果仍然 > 8 = root cause refine：
- 选项 1: 回 Phase 1.4 / 1.5 看是否漏了一类 root cause
- 选项 2: **stop and ask user** 是否接受 partial fix + iter-13A 再 refine
- 严禁: 静默 declare "iter-12A complete"（per precedent #3 / #4）

---

## Phase 2 — R3 verification-vs-sweep regression bisect (R3 only, not bimodal)

**重要 scope 限定 (2026-05-16 QR3 确认后)**：

Phase 2 **不**为 bimodal 服务。QR3 数据验证 (iter-10A 已有 8 bimodal cells = gate-12 阈值) 证明 bimodal 是**预 iter-11A** 结构性 issue —— 在 iter-11A 5 个 commit 中 bisect 找不到它的引入点。bimodal 由 Phase 1 observe-first 路径处理。

Phase 2 **专门**处理 **R3 verification-vs-sweep 6× regression**：iter-11A Phase 1 commit `a27dfda` 验证 cell 报告 r_avg = **1.4 µs**，Phase 6 sweep 同 cell 测到 r_avg = **8.5 µs**，read-path 中间至少有一个 commit 让 R3 退化 6×。这是 iter-11A 真正回归（**bimodal 不是新的**，R3 才是）。

候选 commits (iter-11A timeline)：
- `a27dfda` Phase 1 ship (forwarder-pool-direct + ReadStaging) — 验证 cell 测到 R3=1.4µs
- `455379e` Phase 2 ship (parallel inval, hash-diff PASS) — **疑点 #1**：可能引入额外 sync overhead
- `5664945` Phase 2 revert + Phase 3 doc-only + Phase 4 deferred — **疑点 #2**：revert 是否彻底
- `3469388` Phase 5 path_decomp (no code change to protocol，但 build flag 可能不同) — **疑点 #3**：FUSEE_PROBE / inline 边界变化
- `e4c682b` Phase 6 sweep + summary + iter-12A backlog — 应当 doc-only
- + Phase 1.6 commit (this iter's bimodal fix) — 控制变量，需要重测

### Phase 2.A —— Setup test cell + harness

挑 **workload-c T=64 cache=on kv=1024** 作为 R3 regression 复测 cell（iter-11A Phase 1 commit message 报的 18.92 cell，R3 1.4 µs cell）。

写 `scripts/iter12A_p2_bisect_one_commit.sh`:
```bash
# args: <commit_sha> <out_dir>
git checkout $1
make -j16 protocol_a_ycsb
for rep in 1 2 3 4 5; do
  # run workload-c T=64 cache=on kv=1024, capture trans_agg_thpt + r_avg_ns
done
# output 5 rep (thpt, r_avg_us) to $2/$1.csv
```

### Phase 2.B —— Bisect 5 commits (C18 强制)

```bash
for commit in a27dfda 455379e 5664945 3469388 e4c682b; do
  bash scripts/iter12A_p2_bisect_one_commit.sh $commit /tmp/p2_bisect
done
```

**Bisect 信号**：哪一个 commit 上 r_avg 从 ~1.4 µs 跳到 ~8 µs 区间，throughput 从 ~18 跌到 ~11。

### Phase 2.C —— First-bad-commit RAP

如果找到 first-bad commit X：
- 写 `docs/iters/iter12A_r3_regression_rca.md`
- `git diff X^..X` 看具体改了什么
- 跟 Phase 1.5 同样的 RAP 格式
- DECISION (per QR3 = no-revert)：
  - **(a) Fix-in-place** — 默认；找到具体 line 改回 fast-path
  - (b) **Stop and ask user** if Phase 1 (`a27dfda`, forwarder-pool-direct 自身) 是 first-bad —— 用户预期 commit 本身没问题，bisect 此结果须先和用户确认再 fix-in-place 还是 revert (QR3 偏好 fix-in-place 但极端情况留 escape)
  - revert 不再是默认选项（QR3）

### Phase 2.D —— Fix R3 regression

视 2.C 决策实施 fix-in-place。Hash-diff 20/20 必须 PASS。

**Exit (G-B + G-C gate)**: 修复后 workload-c best cell 跑 5 rep, r_avg median ≤ 2 µs 且 throughput median ≥ 17 Mops/s = PASS。

---

## Phase 3 — 2nd-round path_decomp on iter-11A new-best cells

**Goal**: iter-11A Phase 6 sweep 暴露 5/5 workload 的 best cell 都跟 iter-10A 不同。在这些 new-best cell 上跑 path_decomp，看新 dominant stage 是什么。

### Phase 3.A —— Build with FUSEE_PROBE=1

(同 iter-11A Phase 5 procedure)

### Phase 3.B —— 5 cells × {best, worst} = 10 cells path_decomp

iter-11A new-best cells:
- workloada T=64 cache=off kv=1024
- workloadb T=64 cache=off kv=512
- workloadc T=64 cache=on kv=512
- workloadd T=64 cache=off kv=256
- workloadf T=64 cache=off kv=256

每 cell 3 try (1 healthy + 2 anomaly attempt)，跟 iter-11A Phase 5 pipeline 完全一致。

### Phase 3.C —— Delta tables vs iter-10A baseline

用 `scripts/iter11A_delta_tables.py` (已有) 重跑，generate `iter10A_vs_iter12A_delta.md` per cell + consolidated.

### Phase 3.D —— RCA: dominant stage shift?

写 `docs/iters/iter12A_path_decomp_analysis.md`:
- 哪个 stage 现在 dominant?
- 跟 iter-10A 比 R3 / W10 / I6 谁的 mean / p99 变了?
- iter-12A Phase 1/2 修复后, 哪些 stage 改善?

**Exit (C10)**: 10 cells × 14 stage = 140 stage-row, no unjustified `✱ no data`.

---

## Phase 4 — Full sweep + iter-12A summary + iter-13A backlog

### Phase 4.A —— 210-cell sweep on iter-12A final build (FUSEE_PROBE=0)

```bash
bash scripts/iter11A_sweep.sh docs/g34_scaling_ycsb_iter12A_${TS}/
```

### Phase 4.B —— Anomaly scan + 5-rep verify

```bash
python3 scripts/iter11A_anomaly_scan.py --summary ... --out ...
bash scripts/iter10A_anomaly_5rep_verify.sh ...
```

**Gate-12 check (G-A re-confirm)**: bimodal count ≤ 8 = PASS

### Phase 4.C —— iter12A_summary.md

格式 mirror iter-11A summary，包含：

1. TL;DR
2. **Phase delivery audit** (per precedent #3 gate) —— 7 Phase-1 sub-phase + Phase 2 + Phase 3 + Phase 4 完整 audit table
3. **C1-C18 audit** —— 包括新 C16/C17/C18 是否被遵守
4. **Quantitative gate verdict** (G-A/B/C/D)
5. **Bimodal root cause + fix**（cite Phase 1.5 RAP）
6. **R3 regression bisect + fix**（cite Phase 2.C RAP）
7. **Cross-iter performance evolution table** (iter-10A / iter-11A / iter-12A 三列对比)
8. **Process retrospective** —— observe-first methodology 是否有效？是否需要进一步 codify?
9. **Commits this iter** (列每 commit + 简要说明)

### Phase 4.D —— iter13A_backlog_memo.md

继承 iter-11A 未做项 + iter-12A 新发现项:
- iter-11A backlog #4 (parallel inval redesign) —— 仅在 Phase 1 RCA 显示 inval path 仍是 bottleneck 时才进
- iter-11A backlog #5 (hot-key replication) —— 仅在 W10 仍 dominant 时才进
- iter-11A backlog #6 (RCU re-eval) —— 同上
- iter-11A backlog #7 (probe overhead attribution)
- 长期项: 变长 key / BucketLockTable removal / 蓝图重写 / aggregator batching

### Phase 4.E —— Living docs final sync

确保 C7 涉及的所有 doc 都 commit 落地。

---

## Phase delivery audit gate

iter-12A end of iter 必须在 `iter12A_summary.md` 里写**这张表**（per precedent #3）:

| Sub-phase / Constraint | Plan | Delivered | Status |
|---|---|---|---|
| Phase 0.A testbed health | — | — | — |
| Phase 0.B build smoke | — | — | — |
| Phase 0.C baseline repro | — | — | — |
| Phase 1.0 reference cell harness | — | — | — |
| Phase 1.1 bpftrace ABC | — | — | — |
| Phase 1.2 ftrace sched | — | — | — |
| Phase 1.3 gdb snapshot | — | — | — |
| Phase 1.3.5 observation verdict | — | — | — |
| Phase 1.4 event trace (conditional) | — | — | — |
| Phase 1.5 RAP | — | — | — |
| Phase 1.6 fix + hash-diff | — | — | — |
| Phase 1.7 bimodal 5-rep verify | — | — | — |
| Phase 2.A-D R3 bisect | — | — | — |
| Phase 3.A-D path_decomp 5 new-best cells | — | — | — |
| Phase 4.A 210-cell sweep | — | — | — |
| Phase 4.B anomaly scan + verify | — | — | — |
| Phase 4.C summary | — | — | — |
| Phase 4.D iter-13A backlog | — | — | — |
| C1 var KV | — | — | — |
| C2 no value bytes on ring | — | — | — |
| ... | ... | ... | ... |
| C16 observe-first | — | — | — |
| C17 narrow-targeted fix | — | — | — |
| C18 bisect commit-level | — | — | — |

任何 ⚠ PARTIAL 或 ❌ NOT DONE 行没有 prior user-approved descope = iter not complete.

---

## Quantitative success gate (≥ 3 of 4 必须 PASS)

| Gate | Metric | Target | Where verified |
|---|---|---|---|
| **G-A** | Bimodal cell count (gate-12) | ≤ 8 / 210 | Phase 4.B |
| **G-B** | R3 mean (any cell) | ≤ 9.5 µs (iter-10A baseline) | Phase 3.B/C |
| **G-C** | workload-c best (any KV) | ≥ 17 Mops/s | Phase 4.A |
| **G-D** | workload-a best (any KV) | ≥ 20 Mops/s (stretch) | Phase 4.A |

iter-12A complete = at least 3 of {G-A, G-B, G-C, G-D} PASS.

G-A 是**必须 PASS** —— 这是 iter-12A 存在的根本理由。如果 G-A 都 FAIL 而 G-B/C/D PASS，iter-12A 仍然 NOT COMPLETE（per C12）。

---

## Risk register

| Risk | Trigger | Mitigation |
|---|---|---|
| Phase 1.1-1.3 数据**仍然指向不明确 root cause** | observation 数据噪声大或多原因 cascade | 进 Phase 1.4 写 event trace，最差 +5h 实施成本 |
| Fix-A (SCHED_FIFO) 需要 kernel cmdline 改动 | g3/g4 是 PXE-managed | QR2 已问 user，否则只在 user-space 层 setschedparam（可能不够） |
| Phase 1.6 fix 不彻底, gate-12 仍 FAIL | RCA 漏了一类 root cause | C12 强制 stop+escalate，不允许 silent declare complete |
| Phase 2 R3 bisect 定位是 Phase 1 commit 本身 | Phase 1 forwarder-pool-direct 有 latent bug | QR3 已问 user，决定 revert 或 fix |
| Time overrun | Phase 1 比预期慢 | precedent #1/#2/#3 全适用 —— 不允许时间判断驱动 descope |
| Phase 1 fix 引入 correctness regression | hash-diff 应该 catch | C9 强制每 fix phase 跑 G1 hash-diff 20/20 |
| g3/g4 PXE reboot 中断 iter | 历史频发 | memory `feedback_rekey_slave` 标准恢复流程 |

---

## Workflow summary (linear order, no skipping)

```
Phase 0.A → 0.B → 0.C
   ↓
Phase 1.0 → 1.1 → 1.2 → 1.3 → 1.3.5 (verdict gate)
                                    ↓
                              ┌──── V1/V2 ────┐
                              ↓               ↓
                          (skip 1.4)      1.4 event trace
                              ↓               ↓
                              └──→ 1.5 RAP ←──┘
                                    ↓
                                  1.6 fix
                                    ↓
                                  1.7 verify (gate-12)
                                    ↓
Phase 2.A → 2.B → 2.C → 2.D (R3 regression)
   ↓
Phase 3.A → 3.B → 3.C → 3.D (path_decomp 5 new-best)
   ↓
Phase 4.A → 4.B → 4.C → 4.D → 4.E (sweep + summary)
   ↓
iter-12A COMPLETE (if G-A PASS + ≥ 2 of G-B/C/D PASS)
```

---

## What "iter-12A complete" means

不是"7 sub-phase 都 ship 了"，是：

1. **All Phase delivery audit rows ✅ FULL** (no ⚠ or ❌ without prior user OK)
2. **G-A PASS** (bimodal ≤ 8) —— 必须
3. **At least 2 of G-B/G-C/G-D PASS**
4. **C1-C18 全 PASS** (尤其 C16/C17/C18)
5. **Living docs (C7) 全 sync'd**, 包括 CLAUDE.md precedent #4
6. **iter13A_backlog_memo.md committed** —— genuine deferral only

Anything less = NOT COMPLETE, redo until.

---

## Anti-patterns explicitly forbidden in iter-12A

- ❌ 写 Phase 1.5 RAP 时引用任何**不在** docs/iter12A_diagnostic/ 里的 observation —— hypothesis 必须 cite raw 数据
- ❌ Phase 2 bisect 跳 commit —— 必须逐 commit (per C18)
- ❌ Phase 4.A sweep 用 FUSEE_PROBE=1 build —— probe overhead 会污染 sweep（per iter-10A 教训）
- ❌ Phase 1.6 fix 包含跟 Phase 1.5 RCA **无关** 的"顺便优化" (per C17 narrow-targeted)
- ❌ Time-budget descope —— precedent #1/2/3 全适用
- ❌ Silent declare "iter-12A complete" 当 gate-12 仍 FAIL —— precedent #3 + #4 都覆盖
- ❌ Phase 1.4 event trace **超过 3 file × function** without stop+ask —— C17 适用

---

## Files I will create / modify in iter-12A

### New scripts (Phase 1)
- `scripts/iter12A_repro_cell.sh` —— Phase 1.0 复现 harness
- `scripts/iter12A_p1_1_bpftrace.sh` —— Phase 1.1 三个 bpftrace 实验封装
- `scripts/iter12A_p1_2_ftrace.sh` —— Phase 1.2 sched_switch trace
- `scripts/iter12A_p1_3_gdb_snapshot.sh` —— Phase 1.3 gdb -batch dump

### New scripts (Phase 2/3/4)
- `scripts/iter12A_p2_bisect_one_commit.sh` —— Phase 2.B per-commit perf
- `scripts/iter12A_p3_path_decomp.sh` —— Phase 3.B 5-cell path_decomp driver (mirrors iter11A_5wl_pathdecomp.sh)
- `scripts/iter12A_sweep.sh` —— Phase 4.A 210-cell sweep (mirrors iter11A_sweep.sh)

### Conditional code (Phase 1.4, if needed)
- `src/cxl_event_trace.h` —— per-thread event ring buffer (~200 LOC)
- `src/cxl_event_trace.cc` —— ~100 LOC
- `scripts/iter12A_event_trace_analyze.py` —— post-process

### Diagnostic data
- `docs/iter12A_diagnostic/phase_1_1_bpftrace/{collapsed,winning}_{A,B,C}.out`
- `docs/iter12A_diagnostic/phase_1_2_ftrace/{inval,write,read}_sched_switch.txt`
- `docs/iter12A_diagnostic/phase_1_3_gdb/{host0,host1}_bt.txt`
- `docs/iter12A_diagnostic/phase_1_3_5_observation_verdict.md`
- `docs/iter12A_diagnostic/phase_1_4_event_trace/` (conditional)

### RCA docs
- `docs/iters/iter12A_bimodal_rca.md` —— Phase 1.5
- `docs/iters/iter12A_r3_regression_rca.md` —— Phase 2.C
- `docs/iters/iter12A_path_decomp_analysis.md` —— Phase 3.D
- `docs/iters/iter12A_summary_<ts>.md` —— Phase 4.C
- `docs/iters/iter13A_backlog_memo.md` —— Phase 4.D

### Living docs updates
- `CLAUDE.md` —— precedent #4 (Phase 1.5 commit)
- `docs/scaling_ycsb_spec.md` —— observe-first protocol (Phase 1.5 commit)
- `docs/path_decomp_spec.md` —— iter-12A entries (Phase 3 commit)
- `docs/protocol_a_architecture_blueprint.md` —— bimodal failure mode + fix (Phase 1.6 commit)
- `docs/iters/iter11A_summary_20260511.md` —— iter-12A redress note (Phase 1.6 commit)
- `docs/fusee_cxl_progress.md` —— every phase

---

## Final reminder to self

iter-12A 的**核心规则**就一条：

> **Observe before hypothesize. Hypothesize from observed data only. Fix from cited observations only.**

这是 iter-11A backlog #2 失败模式的**直接反义** —— iter-11A 用"数字像 X，所以 root cause 是 Y"反推 hypothesis 而**从未实证**。iter-12A 通过 C16/C17/C18 + CLAUDE.md precedent #4 把这条规则**永久 codify** 进项目 process。

如果 iter-12A 完成后 bimodal 还在，那说明 C16/C17/C18 这套 process 不够 —— 但**不能**因为时间 / 直觉跳过 process 直接动手 fix。
