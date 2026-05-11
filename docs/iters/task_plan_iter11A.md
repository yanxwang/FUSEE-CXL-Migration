# iter-11A constraint document — bimodal root-cause + forwarder-pool-direct + parallel inval drain + hot-bucket sharding + RCU re-evaluation

**Author**: Claude (post iter-10A completion 2026-05-10)
**Date drafted**: 2026-05-10
**Deadline**: TBD by user (default expectation: comfortably day-scale, no descope)
**Branch**: `feat/cxl-migration`
**Predecessor**: `docs/iters/iter10A_summary_20260510.md` (iter-10A COMPLETE 11/11 hard constraints with 2 carve-outs, 5 commits)
**Spec refs**: `docs/design_goals.md §I-XIII` (esp. §I3 cache_pool, §I9 strict-A linearizability, §I11 N:1:1:N), `docs/path_decomp_spec.md`, `docs/scaling_ycsb_spec.md` (incl. §13 gate-7 bimodal flag added by iter-10A), `docs/protocol_a_architecture_blueprint.md`
**CLAUDE.md cautionary precedents**: #1 (iter-2A pre-flight descope), #2 (iter-6A scope creep + outlier dismissal), #3 (iter-9A silent minimal-version substitution) — `Phase delivery audit gate` MUST appear in iter-11A summary

---

## TL;DR

iter-10A 把 5-workload best 推到 11-15 Mops/s（57-74% 目标），剩下 5-9 Mops/s 的 gap 由 path_decomp 量化为 4 个独立 µs 级 stage:

- **R3 = 9.5 µs** (forward_read RDMA-equivalent) — universal 7/7 cells top-3
- **W10 = 6.4 µs mean / 25 µs p99** (cache_pool insert CAS retry storm) — universal 7/7
- **R1 = 5.5 µs mean / 17.8 µs p99** (cache_pool memcpy MESI ping-pong) — 5/7 write-heavy
- **I6 = 591 µs mean / 1369 µs p99** (invalidate broadcast wait) — workload-a/f 失控 tail

外加 1 个 iter-10A 新发现的 **failure mode** 必须先排掉，否则后续所有 sweep 数据被污染:

- **Bimodal-cell instability**: 8/210 cell 单 rep 双峰（max 5-13 Mops/s, median <0.05），中间无值

iter-11A 用 **6 个 phase** 一次性解决：

0. **Phase 0 — Bimodal-cell root-cause investigation** (no code change, instrumentation + experiments)
   - 不先排掉这个，后续 sweep 数据全部不可信
   - Gate-7 (iter-10A 新增 spec) 第一次正式应用
1. **Phase 1 — Forwarder-pool-direct + cross-host pool generation** (iter-10A backlog #1)
   - 解 R3 universal bottleneck
   - 5-workload 普惠（每个 workload 都受益）
   - iter-9A redo 已有完整设计 → 实施 risk 最低
2. **Phase 2 — Parallel inval-broadcast receiver** (iter-10A backlog #8, NEW finding)
   - 解 I6 失控 tail（591 µs → 50 µs 预测）
   - workload-a/f 直接 +20% 起步
3. **Phase 3 — Hot-bucket sharding within owner** (iter-10A backlog #6)
   - 解 R1 MESI ping-pong + workload-A Zipf 头部串行化
   - **workload-A 突破 20 Mops/s 的关键 phase**
   - 复杂度最高（dynamic split/merge concurrent lookup safety）
4. **Phase 4 — RCU cache_pool re-evaluation** (iter-10A backlog #5)
   - **首先 measure** Phase 1+2+3 后的 W10 p99
   - 如果 ≤ 12 µs（前 3 phase 间接修了 contention pattern）→ skip 实施，移到 iter-12A
   - 否则实施 RCU + epoch reclamation
   - **决策点 driven by 测量**，不是先实施再回头看
5. **Phase 5 — Pre-sweep path_decomp on iter-10A's same 10 cells** (validate stage delta, cleanest iter-10A vs iter-11A comparison)
6. **Phase 6 — Full 210-cell sweep + iter-11A summary + iter-12A backlog**

**核心目标（quantitative, 必须达成 ≥ 3/5 才 declare iter complete）**:
- workload-a best ≥ **20 Mops/s** (vs iter-10A 14.81, +35%) — **breakthrough target**
- workload-f best ≥ **20 Mops/s** (vs iter-10A 13.65, +47%)
- workload-c best ≥ **17 Mops/s** (vs iter-10A 11.72, +45%)
- workload-d best ≥ **15 Mops/s** (vs iter-10A 11.35, +32%)
- workload-b best ≥ **15 Mops/s** (vs iter-10A 11.67, +29%)

**核心约束**:
- 所有 phase 完整执行，**不允许 descope**（CLAUDE.md precedent #1/2/3 全适用）
- forwarder-pool-direct 必须保持 §I9 strict-A linearizability，不能 LRC-化
- parallel inval drain 必须保持 invalidate ordering（per-bucket FIFO）
- hot-bucket sharding 必须 dynamic — cold key 的 lookup path 不能变慢
- RCU 实施前必须先 measure，不是先做再说
- bimodal cell count after each phase 必须 ≤ iter-10A baseline 8/210（gate-12 NEW）

---

## Scope

### In scope (5 个 modify-code phase + 1 个 investigation phase 全 mandatory)

- Bimodal-cell root-cause investigation + mitigation OR explicit carve-out with iter-12A backlog
- Forwarder thread architecture rewrite — direct-deposit to requester staging area instead of req-ack roundtrip
- InvalReceiver split: 1 dispatcher + N=8 worker threads, per-bucket FIFO ordering preserved
- Hot-bucket detection + dynamic N=8 sharding within owner host
- RCU + epoch reclamation cache_pool — **conditional on Phase 4.A measurement**
- Full 210-cell sweep on the post-Phase-3 (or post-Phase-4) architecture
- 5-workload × 2-cell path_decomp on the new architecture (re-do iter-10A's Phase 4 with new measurements)
- Living docs (C7) 实时更新

### Out of scope (defer to iter-12A+)

- Variable-length keys (iter-9A redo backlog #7) — 不阻塞 20 Mops/s gate
- BucketLockTable removal (-2.5 GB CXL, iter-7A backlog) — pure space optimization, no perf impact
- Living-docs full §II.3-II.6 narrative rewrite — separate doc-only iter
- Multi-host > 2 (currently g3 + g4 only) — out of testbed scope
- TLS cache size sweep re-run (iter-10A determined sweet spot = 1024) — no need to re-validate
- Aggregator path P0/P1/P2/P3 with true fetch_add(N) batching (iter-10A backlog #4) — only relevant if B0 path saturates; currently no evidence it does

### Memo for future

- After Phase 1 (forwarder-pool-direct), R3 should drop ~40%. If R1 / W10 stay similar, that means **read path dominated by R3** as iter-9A redo predicted, and the read-path -37% regression is recovered.
- After Phase 3 (hot-bucket sharding), workload-A R1 should drop dramatically. If W10 also drops (因为 hot bucket 不再 contention 中心), Phase 4 RCU 可能不需要做。
- iter-12A first task likely: (a) cross-host hot-bucket coordination if Phase 3 reveals **inter-host** hot key 协调 issue, OR (b) NUMA-aware shard placement if workload-A T=64 hot keys 跨 socket。

---

## Hard constraints (违反 = iter 重做)

继承 iter-10A 的 C1-C11，新增 C12-C15。

| 约束 | 验证手段 |
|---|---|
| **C1-C7** | 同 iter-9A redo（全部继承且必须保持）：变长 KV、no value bytes on message ring、全线程 CPU pin、N:1:1:N runtime active assert、G1 hash-diff at 全 KV×workload、path_decomp 全跑、living docs realtime |
| **C8** TLS cache 跟 shared cache_pool 强一致 (§I9 strict-A 不变) | 同 iter-10A — TLS 存 epoch, lookup 命中后 reload bucket epoch 比较；G1 hash-diff battery Phase 1 / 2 / 3 后各跑一次 |
| **C9** 多 build hash-diff 全 KV × workload | Phase 1 / 2 / 3 / 4 后**各**跑一次 G1 hash-diff battery (20 cells)；Phase 5 final build 一次。任一 build FAIL = phase invalid |
| **C10** 5-workload × 2-cell × 14-stage path_decomp | Phase 5：5 workload × 2 cell (best + worst from Phase 5 sweep) × 14 stage = 140 stage-row 全覆盖；any unjustified `✱ no data` 行 = Phase 5 不 exit |
| **C11** TLS size 验证 | iter-10A sweet spot=1024 沿用；不重新 sweep。但 sanity check：post-Phase-1 跑一次 workload-A T=64 cache=on KV=1024 with TLS=1024，confirm thpt 不变差 |
| **C12 (NEW)** Bimodal cell count gate | Phase 0 完后 + 每个 modify-code phase (1/2/3/4) 完后：跑一次 5-rep verify on 已知 8 个 bimodal cell + Phase 5 sweep 后扫出的新 bimodal cell。**count ≤ 8/210 (iter-10A baseline)** = phase pass；任一 phase 后 count > 8 = 该 phase 引入 instability，必须 root cause OR revert |
| **C13 (NEW)** Forwarder-pool-direct §I9 不变性 | (a) Forwarder pre-fetch 时必须验证 epoch 一致，stale read = retry from latest snapshot；(b) Direct-deposit to requester staging 必须经过 receiver-side validation （checksum or epoch tag in staging slot）；(c) G1 hash-diff battery 20/20 PASS at Phase 1 完。任一不 PASS = Phase 1 重做 |
| **C14 (NEW)** Parallel inval drain ordering | (a) Per-bucket FIFO must hold — 同一 bucket 的两个 invalidate 不能被两个 worker thread 乱序处理；(b) Sharding key = `bucket_id` (不是 `key_hash`) — 保证同一 bucket 总是同一 worker；(c) cache_pool_set_stale 仍然 bump epoch (同 iter-10A C8) — TLS readers 行为不变；(d) G1 hash-diff Phase 2 完 20/20 |
| **C15 (NEW)** Hot-bucket sharding dynamic + cold-path zero-cost | (a) Cold bucket 的 lookup path 必须 unchanged — 不能加额外 indirection 让冷数据变慢；(b) Hot detection threshold = adaptive (lookup count per epoch ≥ 10× mean per-bucket count) — 不能 hardcode；(c) Split/merge 必须 lock-free or epoch-protected — 不能让 split 过程中 lookup 看到 partial state；(d) G1 hash-diff Phase 3 完 20/20 |

---

## Living docs to update (per C7, 实时随每 sub-phase 同 commit 落地)

| 文档 | 改的章节 | 触发 phase |
|---|---|---|
| `docs/protocol_a_architecture_blueprint.md` | Part II §II.2 (read path R1-R6) — R3 改 forwarder-pool-direct，移除 ack-wait 描述 | Phase 1.B 完 |
| `docs/protocol_a_architecture_blueprint.md` | Part I.5 (forwarder thread) — 加 pre-fetch + direct-deposit 描述 | Phase 1.A 完 |
| `docs/protocol_a_architecture_blueprint.md` | Part II §II.4 (invalidate path I1-I8) — I6 改 dispatcher + worker pool 描述 | Phase 2.B 完 |
| `docs/protocol_a_architecture_blueprint.md` | Part I.3 (cache_pool) — 加 hot-bucket sharding 描述 + adaptive split criteria | Phase 3.A 完 |
| `docs/protocol_a_architecture_blueprint.md` | Part II.3-II.6 narrative rewrite (long-deferred) — 趁 II.2/II.4 改了一并补 | Phase 5 |
| `docs/design_goals.md` | §I3 (cache_pool) — 加 sharding invariant: cold path zero-cost, split lock-free | Phase 3.A 完 |
| `docs/design_goals.md` | §I9 (strict-A) — 加 forwarder-direct 路径下 §I9 怎么保 | Phase 1.A 完 |
| `docs/design_goals.md` | §X H6 — invalidate ordering invariant: per-bucket FIFO under parallel drain | Phase 2.B 完 |
| `docs/scaling_ycsb_spec.md` | §13 gate-12 (NEW) — bimodal cell count regression gate | Phase 0 完 |
| `docs/path_decomp_spec.md` | §11 reference instances — 加 iter-11A 5-workload decomp entries | Phase 5 完 |
| `CLAUDE.md` | 加新 cautionary precedent (if iter-11A 暴露 anything novel) | Phase 5 |
| `docs/fusee_cxl_progress.md` | iter-11A 进度行 | 每 phase 完 |

强制规则：每个 commit staged file 列表必须包含上面表格中**触发 phase 匹配**的 doc 改动 (per iter-10A C7)。Phase 5 不补遗 — 只做最终一致性 review。

---

## Phase 0 — Bimodal-cell root-cause investigation

**目的**: iter-10A 发现 8/210 cell 双峰（max 5-13 Mops/s, median <0.05, 中间无值）。**不先排掉这个，iter-11A 所有 sweep 数据可信度都打折**。这是 gate-12 (NEW) 第一次正式应用。

### 0.A — Per-rep first-op latency probe

加 1 个 µs 级 probe：从 `attach_complete` 到 `first_op_complete`。导出到 stderr per-rep。

**Output**: `docs/iter11A_bimodal_p0_<ts>/probe_per_rep.tsv` (cell, rep, attach_to_first_op_ns, total_wall_ns, thpt)

### 0.B — Run 5 known-bimodal cells × 20 reps each

5 cells = `workloada_T4_on_kv512`, `workloada_T64_on_kv1024`, `workloadb_T64_on_kv512`, `workloadd_T4_off_kv512`, `workloadf_T8_on_kv512`

每 cell 20 reps，记录每 rep 的 first-op latency + total thpt。Cluster 分析: 失败 rep 的 first-op latency 跟健康 rep 是否系统性不同？

### 0.C — perf-record 对照实验

挑一个最稳定的 bimodal cell（i.e., 健康 rep 跟崩溃 rep 在 20 reps 里各占 ~50%），分别 perf-record 一次健康 + 一次崩溃，对比：
- CPU usage per thread
- ssh kernel events (sched_switch / page fault / TLB miss)
- 应用层：哪个 stage 卡住 (probe based timing)

### 0.D — Cookie collision + state-pollution check

- Log all `FUSEE_RUN_COOKIE` values used in a sweep, count duplicates
- 在每个 cell 之间加 explicit `daxctl reset` + `dmesg | grep -i cxl` capture，确认 device 无残留状态

### 0.E — Mitigation OR carve-out

基于 0.B-D 的发现，三选一:

(a) **Found root cause + 200-LOC fix**: implement, re-run 20×5 reps verify, bimodal count → 0
(b) **Found root cause + structural fix needed > 200 LOC**: implement workaround (e.g., per-cell sleep + state-reset), document root cause + iter-12A backlog #N for full fix
(c) **No clear root cause**: explicit carve-out per gate-7 — document all hypotheses tested + ruled out, file iter-12A backlog #N with experiment plan

### Phase 0 Exit Criteria

- [ ] Per-rep first-op latency probe landed
- [ ] 5 known-bimodal cells × 20 reps verification data
- [ ] perf-record health/collapse comparison on 1 cell
- [ ] Cookie collision + dmesg state pollution checked
- [ ] Mitigation/carve-out decided + committed
- [ ] **Bimodal count after Phase 0**: target ≤ 5/210 (down from iter-10A 8/210); if no fix possible, gate-12 baseline = 8 (unchanged)
- [ ] **C7 living doc**: scaling_ycsb_spec §13 gate-12 (NEW) added: bimodal cell count regression gate

**预期 LOC**: ~150 (instrumentation + cell-isolation script). Code change for fix: 0-300 depending on root cause.

---

## Phase 1 — Forwarder-pool-direct + cross-host pool generation

**目的**: 解 R3 universal bottleneck (9.3-10.5 µs across all 5 workloads). iter-9A redo 预测的 -37% read-path 回归在这一 phase 修复。**5-workload 普惠** — 每个 workload 都受益。

### 1.A — Architecture: forwarder pre-fetch + direct-deposit

**Current** (iter-10A):
```
reader → fwd_read req on ReadRing → owner host receiver → cache_pool_lookup
       ← fwd_read ack on ReadRing ← owner host sender   ← memcpy value to ack slot
```
2 × CXL fetch_add (req + ack) + 2 × memcpy (write to ack slot, read from ack slot) = ~9 µs

**iter-11A target**:
```
reader → fwd_read req on ReadRing → owner host forwarder thread:
                                      cache_pool_lookup (epoch tagged)
                                      direct memcpy(reader_staging[req.slot_idx], value)
                                      sfence + bump reader_staging[req.slot_idx].ready_epoch
       ← reader spins on staging.ready_epoch (DRAM-local poll, not CXL)
```
1 × CXL fetch_add (req only) + 1 × memcpy (forwarder writes directly to reader's CXL-mapped staging) + DRAM poll on ready_epoch.

**省 1 次 CXL roundtrip + memcpy 跟 fetch_add 并行**.

### 1.B — Implement ForwardStaging[H] read-side arena

iter-9A redo 已有 `ForwardStaging[H]` arena for write path. iter-11A 加 read variant:

```cpp
struct ReadStagingSlot {
  alignas(64) std::atomic<uint64_t> ready_epoch;  // 0 = not ready, N = ready (matches req epoch)
  uint64_t key;
  uint32_t value_size;
  char value_bytes[1024];
};
ReadStagingSlot read_staging[NUM_HOSTS][READ_RING_DEPTH];  // CXL-mapped, per-(req_host, slot_idx)
```

Forwarder thread on owner host writes to `read_staging[req_host][slot_idx]` directly (CXL store, not DRAM); requester reads its own slot from local CXL view.

### 1.C — Forwarder thread main loop rewrite

```cpp
while (running) {
  fwd_req = read_ring_pop_req();              // 1× CXL fetch_add on ReadRing tail
  if (!fwd_req) continue;
  
  cur_epoch = bucket_epoch(fwd_req.key);
  value = cache_pool_lookup(fwd_req.key);     // DRAM-local
  if (!value) {
    // miss path: fall through to legacy fwd_read_ack with sentinel
    fwd_read_ack(fwd_req, MISS, 0);
    continue;
  }
  
  // Direct deposit
  slot = &read_staging[fwd_req.req_host][fwd_req.slot_idx];
  memcpy(slot->value_bytes, value, fwd_req.kv_size);
  slot->value_size = fwd_req.kv_size;
  slot->key = fwd_req.key;
  flush_line(slot, sizeof(ReadStagingSlot));
  sfence();
  slot->ready_epoch.store(cur_epoch, std::memory_order_release);
  flush_line(&slot->ready_epoch, 8);
  sfence();
}
```

**Critical (C13)**: epoch tag in staging slot — requester checks `ready_epoch >= req_epoch_at_send`, ensures the read sees a value at-least-as-recent as when req was sent. Stale value = retry.

### 1.D — Requester read-side path rewrite

```cpp
read_value(key, out) {
  // L1: TLS cache (unchanged from iter-10A Phase 1)
  if (tls_lookup(key, out, &observed_epoch) && bucket_epoch(key) == observed_epoch) return HIT;
  
  // L2: shared cache_pool (unchanged)
  if (cache_pool_lookup(key, out)) { tls_insert(key, out, observed_epoch); return HIT; }
  
  // L3: forward_read with direct-deposit
  slot_idx = read_ring_push_req(key, my_epoch_at_send);
  spin_until(read_staging[my_host][slot_idx].ready_epoch >= my_epoch_at_send, timeout=200ms);
  if (timed_out) return TIMEOUT;
  
  // Validate epoch + copy
  copy_from_staging(slot_idx, out);
  if (bucket_epoch(key) != read_staging[my_host][slot_idx].ready_epoch) goto retry;  // someone wrote between fwd_read return and our copy
  
  tls_insert(key, out, ready_epoch);
  return HIT;
}
```

### 1.E — G1 hash-diff Phase 1 (20 cells)

`scripts/iter11A_phase1_hashdiff.sh` runs 5 workload × 4 KV × 1 rep, compares cross-host bucket bytes. Per C13: **20/20 PASS = phase exit; any FAIL = phase 重做**.

### 1.F — Spot-check Phase 1 effect

Run workload-c (100% read) T=64 cache=on KV=1024 × 5 reps. Compare median vs iter-10A 11.72 Mops/s.

**Target**: ≥ 14 Mops/s (= 70% of 20 target). Per phase exit gate.

### Phase 1 Exit Criteria

- [ ] ReadStagingSlot arena landed + forwarder thread rewritten
- [ ] Reader-side path uses staging direct-deposit
- [ ] G1 hash-diff 20/20 PASS
- [ ] workload-c best ≥ 14 Mops/s (on Phase 1 build, sanity)
- [ ] R3 mean (path_decomp on workload-c best) ≤ 6 µs
- [ ] **C7 living doc**: blueprint Part I.5 + II.2; design_goals §I9 forward-path

**预期 LOC**: ~400 (forwarder rewrite + reader rewrite + ReadStaging arena + hashdiff)

---

## Phase 2 — Parallel inval-broadcast receiver

**目的**: 解 I6 失控 tail (591-738 µs/op mean on workload-a/f). 1 InvalReceiver thread per ring 是 bottleneck — 写多了 invalidate 排队 ack。

### 2.A — Architecture: dispatcher + worker pool

**Current** (iter-10A):
```
InvalRing → InvalReceiver (1 thread) → cache_pool_set_stale (per-bucket spinlock)
                                       → bump bucket epoch
```

**iter-11A target**:
```
InvalRing → InvalDispatcher (1 thread) → enqueue to worker_shard[bucket_id & 7]
                                         → 8 InvalWorker threads in parallel:
                                            cache_pool_set_stale + bump epoch
```

### 2.B — Implementation

```cpp
struct InvalShard {
  alignas(64) MPSC<InvalEntry> queue;      // dispatcher → worker
  std::atomic<bool> stop;
  pthread_t worker_tid;
};
InvalShard inval_shards[NUM_INVAL_SHARDS];  // NUM_INVAL_SHARDS = 8

// Dispatcher (1 thread per ring):
while (running) {
  inval = inval_ring_pop();
  shard_idx = inval.bucket_id & (NUM_INVAL_SHARDS - 1);  // C14: bucket-id sharding
  inval_shards[shard_idx].queue.push(inval);
}

// Worker (8 threads):
while (!shard.stop) {
  inval = shard.queue.pop_blocking();
  cache_pool_set_stale(inval.bucket_id, inval.key);
  bucket_epoch(inval.bucket_id).fetch_add(1, std::memory_order_release);
  // Bumped epoch invalidates TLS readers next access (per iter-10A C8)
}
```

**Critical (C14)**:
- Sharding key = `bucket_id` (not `key_hash`) → same bucket always handled by same worker → per-bucket FIFO ordering preserved.
- 8 workers can run in parallel because **different buckets have no ordering relationship** under §I9.

### 2.C — Thread inventory + CPU pinning (C3 maintained)

iter-10A 6 named threads (Write/Read/InvalSender/Receiver) → iter-11A 13 named threads:
- WriteSender (1) + WriteReceiver (1)
- ReadSender (1) + ReadReceiver-now-Forwarder (1) [from Phase 1]
- InvalSender (1) + InvalDispatcher (1) [renamed from InvalReceiver] + InvalWorker × 8 [NEW]

CPU pin map updated: `taskset CPU 64-76` for system threads (vs iter-10A 64-69).

### 2.D — G1 hash-diff Phase 2 (20 cells)

Per C9 + C14. 20/20 PASS = phase exit.

### 2.E — Spot-check Phase 2 effect

Run workload-a T=64 cache=on KV=1024 × 5 reps + path_decomp on it. 

**Target**:
- I6 mean ≤ 100 µs (vs iter-10A 591)
- workload-a best ≥ 17 Mops/s (vs iter-10A Phase-3 5-rep avg 10.4)

### Phase 2 Exit Criteria

- [ ] InvalDispatcher + 8 InvalWorker threads spawned + CPU-pinned
- [ ] Per-bucket FIFO ordering verified (test: write same key 1000× from H0, H1 reader ack ordering matches)
- [ ] G1 hash-diff 20/20 PASS
- [ ] I6 mean ≤ 100 µs (path_decomp on workload-a worst)
- [ ] workload-a/f best ≥ +20% over iter-10A
- [ ] **C7 living doc**: blueprint Part II.4; design_goals §X H6

**预期 LOC**: ~250 (dispatcher rewrite + worker thread pool + per-shard MPSC)

---

## Phase 3 — Hot-bucket sharding within owner

**目的**: 解 R1 MESI ping-pong (5.5 µs across write-heavy) + workload-A Zipf 头部 key 串行化. **workload-A 突破 20 Mops/s 的关键 phase**. 复杂度最高 — dynamic split/merge concurrent lookup safety.

### 3.A — Architecture: adaptive hot-bucket detection + dynamic split

**Current** (iter-10A):
```
key X → hash(X) % NUM_BUCKETS → bucket B → cache_pool_entry[B]
                                            ↑ 64 worker compete on this cacheline
```

**iter-11A target**:
```
key X → hash(X) % NUM_BUCKETS → bucket B
        → if (B is HOT): hash(X) >> N_low_bits → sub_bucket s → cache_pool_entry[B][s]
        → else (cold): cache_pool_entry[B]   (unchanged, zero-cost cold path per C15)
```

### 3.B — Hot detection (adaptive, per C15)

```cpp
struct BucketStats {
  alignas(64) std::atomic<uint64_t> lookup_count_this_epoch;
  std::atomic<uint64_t> last_epoch_total;     // for moving average
  std::atomic<uint8_t> shard_count;           // 1 = cold, 8 = hot
};

// Background thread (or piggyback on cache_pool_set_stale):
every 1ms:
  total = sum(bucket.lookup_count_this_epoch for all buckets) / NUM_BUCKETS;  // mean
  for each bucket:
    if (bucket.lookup_count_this_epoch >= 10 * total && bucket.shard_count == 1):
      try_split_to_8(bucket);
    if (bucket.lookup_count_this_epoch < total * 2 && bucket.shard_count == 8):
      try_merge_to_1(bucket);
    bucket.last_epoch_total = bucket.lookup_count_this_epoch.exchange(0);
```

### 3.C — Lock-free split (C15 critical)

Splitting a cold bucket → 8 sub-buckets must not corrupt concurrent lookups:

```cpp
try_split_to_8(bucket B):
  // 1. Allocate 8 sub-buckets, copy current entries to correct sub by hash(key)>>N
  new_subs = alloc_8_sub_buckets();
  for each entry e in B:
    sub_idx = hash(e.key) >> N_low_bits;
    new_subs[sub_idx].insert(e);
  
  // 2. Atomic publish: bump shard_count + swap pointer (RCU-style)
  old_ptr = B.entries;
  B.subs = new_subs;
  std::atomic_thread_fence(std::memory_order_release);
  B.shard_count.store(8, std::memory_order_release);
  
  // 3. Schedule old_ptr reclaim after grace period (epoch-based)
  schedule_reclaim(old_ptr, current_epoch + 2);
```

Lookup path:
```cpp
cache_pool_lookup(key):
  bucket = &cache_pool[hash(key) % NUM_BUCKETS];
  shard_count = bucket->shard_count.load(std::memory_order_acquire);
  if (shard_count == 1):
    return bucket->entries.lookup(key);    // cold path — UNCHANGED, zero-cost (C15)
  else:
    sub_idx = hash(key) >> N_low_bits;
    return bucket->subs[sub_idx].lookup(key);
```

### 3.D — Cold path zero-cost verification (C15)

Build a micro-bench: 1000 keys uniformly distributed across 65536 buckets (cold). Compare lookup latency before/after Phase 3.

**Target**: cold lookup latency delta ≤ 5% (per C15).

### 3.E — G1 hash-diff Phase 3 (20 cells)

Per C9 + C15. 20/20 PASS = phase exit.

### 3.F — Spot-check Phase 3 effect

Run workload-A T=64 cache=on KV=1024 × 5 reps + path_decomp.

**Target**:
- workload-A best ≥ **18 Mops/s** (vs iter-10A 14.81; +22%)
- R1 mean ≤ 3 µs (vs iter-10A 5.5)

### Phase 3 Exit Criteria

- [ ] Adaptive hot detection landed (NOT hardcoded threshold)
- [ ] Lock-free split + epoch-based reclaim
- [ ] Cold lookup latency delta ≤ 5% (C15 micro-bench)
- [ ] G1 hash-diff 20/20 PASS
- [ ] workload-A best ≥ 18 Mops/s
- [ ] **C7 living doc**: blueprint Part I.3; design_goals §I3

**预期 LOC**: ~300 (sharding logic + adaptive detection + RCU-style publish + micro-bench)

---

## Phase 4 — RCU cache_pool re-evaluation (decision-driven)

**目的**: iter-10A backlog #5 = RCU + epoch reclamation cache_pool to fix W10 p99 retry storm. **但 Phase 1+2+3 都改变了 W10 的 contention pattern** — measure first, decide second.

### 4.A — Measure post-Phase-3 W10 stats

Run path_decomp on:
- workload-a worst (T=64 cache=on kv=1024)
- workload-c best (T=64 cache=on kv=1024)
- workload-f best (T=64 cache=on kv=512)

Extract W10 p50 / p99 / mean.

**Decision tree**:
- W10 p99 ≤ 12 µs (≤ iter-10A 5× spinlock baseline) → **SKIP Phase 4 implementation**, defer to iter-12A. Document measurement evidence.
- W10 p99 > 12 µs → **implement Phase 4** as described below.

This is **driven by data, not a-priori commitment**. Per CLAUDE.md anti-confirmation-bias.

### 4.B — Implementation (only if 4.A says yes)

```cpp
struct CachePoolEntryV {
  uint64_t key;
  uint32_t value_size;
  char value_bytes[1024];
};

struct CachePoolBucket {
  std::atomic<CachePoolEntryV*> current;     // RCU-protected pointer
  std::atomic<uint64_t> epoch;                // unchanged (TLS invariant per C8)
  // ... no more spinlock or seq
};

// Writer:
cache_pool_insert(key, value):
  new_v = alloc_entry();
  new_v->key = key; memcpy(new_v->value_bytes, value, size);
  old_v = bucket->current.exchange(new_v, std::memory_order_acq_rel);
  bucket->epoch.fetch_add(1, std::memory_order_release);
  schedule_reclaim(old_v, current_epoch + 2);   // grace period

// Reader:
cache_pool_lookup(key, out):
  epoch_pin();
  v = bucket->current.load(std::memory_order_acquire);
  if (v && v->key == key):
    memcpy(out, v->value_bytes, v->value_size);
    epoch_unpin();
    return HIT;
  epoch_unpin();
  return MISS;
```

### 4.C — Epoch-based reclamation (RCU lite)

```cpp
class EpochReclaimer {
  std::atomic<uint64_t> global_epoch;
  std::atomic<uint64_t> per_thread_epoch[MAX_WORKERS];
  std::vector<std::pair<uint64_t, void*>> retire_list;  // sharded per-thread
  
  void epoch_pin() {
    per_thread_epoch[my_id].store(global_epoch.load());
  }
  void schedule_reclaim(void* p, uint64_t reclaim_epoch) {
    retire_list[my_id].emplace_back(reclaim_epoch, p);
  }
  // Background reaper thread or piggyback on epoch_pin():
  void try_reclaim() {
    min_e = min(per_thread_epoch[i] for i in active workers);
    while (retire_list[my_id].front().first < min_e):
      free(retire_list[my_id].pop_front().second);
  }
};
```

### 4.D — G1 hash-diff Phase 4 (20 cells)

Per C9. 20/20 PASS.

### 4.E — Spot-check Phase 4 effect

Re-measure W10 p50/p99/mean on same 3 cells from 4.A.

**Target**: W10 p99 ≤ 10 µs (vs iter-10A 25 µs).

### Phase 4 Exit Criteria

- [ ] 4.A measurement table committed (decision evidence)
- [ ] If implementing: RCU + epoch reclamation landed
- [ ] G1 hash-diff 20/20 PASS
- [ ] W10 p99 ≤ 10 µs (post-impl)
- [ ] **C7 living doc**: design_goals §VI-A.bis (RCU pattern); blueprint Part I.3 update

**预期 LOC**: ~500 if implementing; ~50 if skipping (just measurement doc)

---

## Phase 5 — Pre-sweep path_decomp on iter-10A's same 10 cells (clean delta)

**目的**: 用 iter-10A Phase 4 选过的同样 10 个 cell（best + worst per workload）跑 path_decomp on the post-Phase-4 build. 这样 stage 数据可以**直接对比 iter-10A vs iter-11A 的 µs delta**, 验证每个修复是否真有效（forwarder-direct 是否真的把 R3 砍下来，hot-bucket sharding 是否真的把 R1 砍下来，等等）.

不依赖 Phase 6 sweep 结果选 cell — 这一步先做，独立验证 phase 1-4 的修复。

### 5.A — Re-run iter-10A's 10 cells with iter-11A build

Cells (from `docs/g34_scaling_ycsb_iter10A_20260510_182258/phase4_cells.txt`):
- workloada_best, workloada_worst
- workloadb_best, workloadb_worst
- workloadc_best, workloadc_worst
- workloadd_best, workloadd_worst
- workloadf_best, workloadf_worst

Re-use `scripts/iter10A_5wl_pathdecomp.sh` infrastructure → `scripts/iter11A_5wl_pathdecomp.sh` (same logic, just iter-11A build envs).

### 5.B — Per-cell delta table (iter-10A vs iter-11A)

For each cell, generate `iter10A_vs_iter11A_delta.md`:

| Stage | iter-10A mean µs | iter-11A mean µs | delta % | iter-11A p99 µs | iter-10A p99 µs | p99 delta % |
|---|---:|---:|---:|---:|---:|---:|
| R1 | 5.5 | ? | ? | 17.8 | ? | ? |
| R3 | 9.5 | ? | ? | 17.3 | ? | ? |
| W10 | 6.4 | ? | ? | 25.1 | ? | ? |
| I6 | 591 | ? | ? | 1369 | ? | ? |
| ... | ... | ... | ... | ... | ... | ... |

### 5.C — Cross-cell summary: which fix worked, which didn't

`consolidated_iter10A_to_iter11A_delta.md`:
- For each phase (1/2/3/4), show the targeted stage's delta across all 10 cells
- If a fix didn't deliver predicted gain → flag for iter-12A investigation
- If a NEW stage emerged as top-3 (e.g., a stage that was top-5 in iter-10A is now top-1 because the others got faster) → flag

### Phase 5 Exit Criteria

- [ ] All 10 cells re-captured with iter-11A build
- [ ] Per-cell `iter10A_vs_iter11A_delta.md` exists
- [ ] `consolidated_iter10A_to_iter11A_delta.md` cross-cell summary exists
- [ ] Each phase's predicted-vs-actual gain documented (R3, I6, R1, W10)
- [ ] Any "predicted gain not realized" flagged with hypothesis + iter-12A backlog entry
- [ ] **C7 living doc**: path_decomp_spec §11 + iter-11A reference instance

**预期 LOC**: ~80 (mostly script tweaks + delta table generator)

---

## Phase 6 — Full sweep + iter-11A summary + iter-12A backlog

### 6.A — Full 210-cell scaling_ycsb sweep on post-Phase-4 architecture

```bash
scripts/iter11A_sweep.sh docs/g34_scaling_ycsb_iter11A_<ts>/
```

5 workload × 7 T × 2 cache × 3 KV = 210 cells × 1 rep (per scaling_ycsb_spec §3 standing default).

Build envs: `FUSEE_TLS_SIZE=1024 FUSEE_USE_AGGREGATOR=0` (B0 winner from iter-10A, unchanged in iter-11A).

### 6.B — Anomaly scan + bimodal gate (per CLAUDE.md 2026-05-03 + iter-10A gate-7)

`scripts/probe_anomaly_scan.py` on `gap_to_target.md`.

Anomaly cells must be either (a) 5-rep verified (recovered or steady-state) OR (b) flagged as bimodal per gate-7 with iter-12A backlog entry.

**Gate-12 (NEW)**: bimodal cell count after iter-11A ≤ 8/210 (iter-10A baseline). If > 8 → some Phase introduced instability → **must root cause OR revert that phase**.

### 6.C — Optional: 2nd-round path_decomp on Phase-6 sweep best/worst

If Phase-6 sweep reveals **new** best/worst cells (different from iter-10A's), run a 2nd path_decomp round on those to capture the true post-iter-11A landscape. If sweep best/worst match iter-10A cells (Phase 5 已经覆盖), skip.

### 6.D — iter11A_summary doc

Per `iter10A_summary_20260510.md` template:
- TL;DR
- **Phase delivery audit table** (per CLAUDE.md precedent #3 gate, MANDATORY)
- **Hard constraint compliance audit (C1-C15)**
- Headline measurements (per-workload best, **distance to 20 Mops/s — primary success metric**)
- Cross-workload bottleneck table (vs iter-10A delta)
- Bimodal cell count regression table (Phase 0 → 1 → 2 → 3 → 4)
- Forwarder-pool-direct R3 measurement
- Parallel inval drain I6 measurement
- Hot-bucket sharding R1 + workload-A measurement
- RCU decision evidence (Phase 4.A measurement table)
- iter-12A backlog (genuine deferrals only — no relabeled in-scope work per precedent #3)
- Process retrospective

### 6.E — scaling_ycsb_spec.md gate updates

If new bottleneck pattern emerged + worth codifying → add new gate (e.g., "forwarder thread CPU saturation" if observed).

### 6.F — iter12A_backlog_memo.md

Genuine deferrals:
- Variable-length keys (still open, iter-9A redo backlog #7)
- BucketLockTable removal (iter-7A backlog)
- Living-docs full §II.3-II.6 narrative rewrite (if not done in 5.D)
- Anything new from Phase 5/6 cross-workload analysis
- iter-10A backlog #4 true fetch_add(N) batching — re-evaluate priority based on whether B0 saturates after iter-11A wins

### Phase 6 Exit Criteria

- [ ] 210-cell sweep done, 0 unexplained FAILs
- [ ] Anomaly scan zero unexplained outliers (carve-out OK with iter-12A entry)
- [ ] Bimodal cell count ≤ 8/210 (gate-12)
- [ ] Doubling-ratio gate PASS for all 5 workloads
- [ ] **≥ 3 of 5 workloads at quantitative target**:
  - workload-a ≥ 20 Mops/s
  - workload-f ≥ 20 Mops/s
  - workload-c ≥ 17 Mops/s
  - workload-d ≥ 15 Mops/s
  - workload-b ≥ 15 Mops/s
- [ ] iter11A_summary.md committed
- [ ] iter12A_backlog_memo.md committed
- [ ] **C7 living doc**: scaling_ycsb_spec gate updates if needed; CLAUDE.md new precedent if any

**预期 LOC**: ~150 (sweep script tweaks + parser updates for iter-10A → iter-11A delta tables)

---

## Risks & cautionary precedents

### Risk 1: Phase 1 (forwarder-pool-direct) breaks §I9 strict-A

**Risk**: Direct-deposit to staging means **value is visible to reader before bucket epoch is checked on owner side**. Could LRC-ify the read path (read may return value from a snapshot older than the writer that was committed at req-send time).

**Mitigation (C13)**:
- Forwarder MUST tag staging slot with `cur_epoch` AT THE TIME of cache_pool_lookup
- Reader MUST validate `staging.ready_epoch >= my_epoch_at_send` AND `bucket_epoch(key) <= staging.ready_epoch + window` after copy
- Stale read = retry (loop back to L3 fwd_read)
- Hash-diff battery 20/20 is the regression gate

**Precedent**: iter-2A Phase 1 wire silently degraded strict-A → LRC by publishing ack_seq before clients consumed inval. Same failure mode could repeat here. Be paranoid in C13 verification.

### Risk 2: Phase 3 (hot-bucket sharding) split race

**Risk**: Concurrent lookup during split sees `shard_count=1` but reads `bucket->subs[...]` (uninitialized) — segfault or wrong-key match.

**Mitigation (C15)**:
- shard_count is the gating atomic; load with `acquire`
- subs pointer must be fully populated + fenced BEFORE shard_count.store(8)
- Old `entries` pointer reclaimed only after grace period (epoch + 2)
- Unit test: 16 reader threads + 1 splitter thread × 1M ops, validate no torn reads

### Risk 3: Phase 4 RCU global epoch contention

**Risk**: All workers `epoch_pin()` on every lookup → `global_epoch.load()` on every op → cacheline bouncing.

**Mitigation**:
- Per-thread epoch slot (cacheline padded, `per_thread_epoch[my_id]`)
- Global epoch updated lazily by reaper thread, not by writers
- Worst case: skip Phase 4 entirely (Phase 4.A measurement gates this)

### Risk 4: Bimodal cell investigation finds nothing

**Risk**: Phase 0 fails to identify root cause → bimodal count stays 8/210 → gate-12 baseline = 8 is unmovable → can't tell if Phase 1/2/3 introduces NEW bimodal cells.

**Mitigation**:
- Phase 0 gate-12 baseline = 8/210 (current). Gate-12 pass condition: count ≤ 8/210 after each phase.
- If a phase introduces NEW bimodal cells, gate-12 fails → phase rework required

### Risk 5: Phase scope drift (iter-9A precedent #3)

**Risk**: implementation hits unforeseen blocker (e.g., forwarder thread CPU saturation after Phase 1 due to direct-deposit cost). Tempting to "minimal-version" the next phase.

**Mitigation**: 
- ALL 5 modify-code phases (1-4 + summary 5) MUST ship — no minimal substitution
- If genuinely impossible: STOP AND ASK USER (per CLAUDE.md cautionary precedent #3)
- Phase delivery audit table mandatory in iter11A_summary.md

---

## Open questions for user (to resolve before kickoff)

- [ ] **QR1**: Phase 0 mitigation policy — if root cause is found but fix > 200 LOC, do we (a) implement workaround + iter-12A full-fix, (b) full-implement now even if 500 LOC, or (c) skip and gate-12 baseline = 8 as carve-out? **Recommendation: (a)** — workaround keeps iter-11A focused, full-fix is its own effort.
- [ ] **QR2**: Phase 3 split granularity — N_subshard = 4 or 8 or 16? **Recommendation: 8** — matches iter-9A redo's NUM_INVAL_SHARDS and balances cache-line spread vs metadata overhead.
- [ ] **QR3**: Phase 4 measurement-gated skip — if W10 p99 ≤ 12 µs after Phase 3, do we (a) skip Phase 4 entirely, (b) implement anyway "for completeness", or (c) implement only if W10 still in top-3 of any cell? **Recommendation: (a)** — measurement-driven, save LOC for actual bottleneck.
- [ ] **QR4**: Iter completion gate — must ALL 5 workloads hit their target (a/f ≥ 20, c ≥ 17, d/b ≥ 15) for iter-11A to declare complete, or ≥ 3 of 5? **Recommendation: ≥ 3 of 5** — leaves room for one workload to be uncooperative without forcing iter-redo.
- [ ] **QR5**: Deadline — same as iter-10A ("时间充裕，不允许 descope")? **Default: yes**.

---

## Phase-by-phase commit prefix convention

Per iter-10A precedent + AP16 reference:

| Phase | Prefix |
|---|---|
| Phase 0 (bimodal investigation) | `[iter11A-P0][G12]` |
| Phase 1 (forwarder-pool-direct) | `[iter11A-P1][G6][I9][AP16]` |
| Phase 2 (parallel inval drain) | `[iter11A-P2][G6][I9][C14][AP16]` |
| Phase 3 (hot-bucket sharding) | `[iter11A-P3][G6][I3][C15][AP16]` |
| Phase 4 (RCU re-eval) | `[iter11A-P4][G6][I3][AP16]` |
| Phase 5 (path_decomp delta) | `[iter11A-P5][G6][I9][AP16]` |
| Phase 6 (sweep + summary) | `[iter11A-P6][G6][I9][AP16]` |

Each commit msg references which spec items (I/AP/C) the change relates to (per project H4 hook enforcement).

---

## Estimated commit log shape

Based on iter-10A's 6-commit shape, iter-11A target ~8 commits:

```
1. [iter11A-P0][G12] Bimodal cell investigation: per-rep first-op probe + 5-cell × 20-rep verify + root cause OR carve-out
2. [iter11A-P1][G6][I9][C13][AP16] Phase 1.A-C: forwarder pre-fetch + ReadStaging arena + reader rewrite
3. [iter11A-P1][G6] Phase 1.D-F: hash-diff 20/20 PASS + workload-c spot-check
4. [iter11A-P2][G6][I9][C14][AP16] Phase 2: InvalDispatcher + 8 InvalWorker pool + per-bucket FIFO + hash-diff 20/20
5. [iter11A-P3][G6][I3][C15][AP16] Phase 3: hot-bucket adaptive sharding + lock-free split + cold-path 0-cost + hash-diff 20/20
6. [iter11A-P4][G6][I3] Phase 4.A: W10 p99 measurement table — DECISION (skip OR implement)
7. [iter11A-P4][G6][I3][AP16] Phase 4.B-E: RCU + epoch reclamation (only if 4.A says yes)
8. [iter11A-P5][G6][I9][AP16] Phase 5: 210-cell sweep + 5-workload path_decomp + summary + iter-12A backlog
```

Each commit must include:
- Code changes (if any)
- Test/verification results in commit msg (hash-diff PASS count, target-metric value)
- Living doc updates per the Living Docs table
- iter11A_summary.md or iter12A_backlog_memo.md final update only in commit #8
