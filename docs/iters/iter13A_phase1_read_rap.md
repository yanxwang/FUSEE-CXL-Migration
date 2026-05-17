# iter-13A Phase 1 — Read-path RAP (RCU vs Hazard pointers)

**Date**: 2026-05-17
**Branch**: `feat/cxl-migration`
**Parent**: `docs/iters/task_plan_iter13A.md §Phase 1`
**Goal**: 完全消除 cross-host 读路径上 owner-side `pool→staging→reader` 的额外数据拷贝；让 reader 直接 `pool_->read(blk_off, …)` 把数据从 CXL 拉到自己 DRAM。

---

## STATE (≤ 30 chars)

> Direct pool-pointer read with RCU or Hazard protection.

## 当前读路径 vs 目标读路径

### 当前 (STAGING)
```
Reader (host A) forward_read_direct(key):
  1. Reserve ReadRing[A][B] slot, send (key, req_op_id)
  2. Spin staging.ready_op_id == req_op_id
  3. memcpy(out_buf, st->value_bytes, vlen)  ← value 来自 staging CXL 区

Owner (host B) read_handler(req):
  a. Look up bucket → find slot → encoded_value
  b. blk_off = cxl_slot_blk_off(encoded_value)
  c. pool_->read(blk_off, st->value_bytes, vlen)  ← CXL→DRAM→CXL extra copy
  d. publish_staging.ready_op_id = req_op_id
```
**冗余**：owner 把 pool 数据复制到 staging slot 仅是为了给 reader 一个稳定 buffer。

### 目标 (RCU 或 HAZARD)
```
Reader (host A) forward_read_direct(key):
  1. Reserve ReadRing slot
  2. {GUARD}_enter()                          ← 标记 "I'm reading"
  3. Send (key, req_op_id)
  4. Spin staging.ready_op_id == req_op_id
  5. blk_off = st->blk_off  (staging 只载 control: blk_off + vlen + lookup_epoch + status)
  6. {GUARD}_protect(blk_off)                 ← (Hazard 才需要；RCU 在 enter 已 cover)
  7. pool_->read(blk_off, out_buf, vlen)      ← DIRECT CXL→DRAM read, no extra copy
  8. {GUARD}_exit()

Owner (host B) read_handler(req):
  a. Look up bucket → find slot → encoded_value
  b. blk_off = cxl_slot_blk_off(encoded_value)
  c. st->blk_off = blk_off; st->vlen = vlen; st->lookup_epoch = ...  ← 控制信息 only
  d. publish_staging.ready_op_id = req_op_id
```

**Saved**: 1 × CXL→DRAM→CXL copy of vlen bytes per cross-host read. iter-12A baseline 在 cache=off workloadc 单 read 走过约 12 KB CXL bandwidth, 优化后降到 ~6 KB（节省一倍）。

---

## 当前 pool 语义的关键观察 (会影响 RCU/Hazard 设计)

`src/cxl_kv_blockpool.cc` 当前实现：
- `alloc()`: pure bump per-host segment (`cursors_[host_id_].bump.fetch_add(1)`)
- `free_lazy(off)`: **stub** — 空函数，从不真正回收
- 所以 **block 一旦写出去就永久存在**，直到进程退出

**含义**：iter-13A 200k-ops scaling test 期间 reclamation 不会触发；reader-via-direct-pointer **本质上已经是 safe 的**（block 一直存在，永远读得到提交时的内容）。

那为什么仍然需要 RCU/Hazard？两个原因：
1. **未来 GC**：iter-14A+ 可能加真正的 block reclamation；现在打好基础避免到时候改协议。
2. **测量正确路径的 overhead**：iter-13A 的对比目的之一就是 "在生产语义下，RCU vs Hazard 哪个 overhead 更低"——即便本 iter 不触发回收，publish/scan 的 hot-path 开销 IS the cost we are measuring。

---

## 候选 A: RCU + epoch

### Data structures

新文件 `src/cxl_rcu.h` + `src/cxl_rcu.cc`:

```cpp
// All in CXL so cross-host visible.
// Single global epoch (not per-host) — simpler reasoning, scan is small.
struct CxlRcuDomain {
  std::atomic<uint64_t> publish_epoch;   // bumped on every retire
  struct ThreadSlot {                    // padded to 64B
    std::atomic<uint64_t> reader_epoch;  // 0 = idle, else = epoch_at_enter
    uint64_t _pad[7];
  } thread_slots[kNumHosts][kMaxThreadsPerHost];
};
```

Size: 1 publish_epoch + `2 hosts × 86 threads × 64B = 11 KB` — trivial.

### API

```cpp
// Reader enters critical section before issuing forward_read.
inline uint64_t rcu_enter(CxlRcuDomain *d, int host, int tid) {
  flush_line(&d->publish_epoch);
  full_fence();
  uint64_t e = d->publish_epoch.load(std::memory_order_acquire);
  d->thread_slots[host][tid].reader_epoch.store(e, std::memory_order_release);
  flush_line(&d->thread_slots[host][tid].reader_epoch);
  store_fence();
  return e;
}

inline void rcu_exit(CxlRcuDomain *d, int host, int tid) {
  d->thread_slots[host][tid].reader_epoch.store(0, std::memory_order_release);
  flush_line(&d->thread_slots[host][tid].reader_epoch);
  store_fence();
}

// Owner: called before reclaiming. Returns the epoch needed for grace.
inline uint64_t rcu_advance_epoch(CxlRcuDomain *d) {
  uint64_t prev = d->publish_epoch.fetch_add(1, std::memory_order_acq_rel);
  flush_line(&d->publish_epoch);
  store_fence();
  return prev + 1;
}

// Owner: wait until all readers in epoch < target have exited.
// Cost: O(N_threads) CXL loads each iteration of polling.
inline void rcu_synchronize(CxlRcuDomain *d, uint64_t target_epoch) {
  for (;;) {
    bool clear = true;
    for (int h = 0; h < kNumHosts; ++h) {
      for (int t = 0; t < kMaxThreadsPerHost; ++t) {
        flush_line(&d->thread_slots[h][t].reader_epoch);
        full_fence();
        uint64_t e = d->thread_slots[h][t].reader_epoch.load(std::memory_order_acquire);
        if (e != 0 && e < target_epoch) { clear = false; break; }
      }
      if (!clear) break;
    }
    if (clear) return;
    __builtin_ia32_pause();
  }
}
```

### Per-read overhead (RCU)
- `rcu_enter`: 1 CXL load (publish_epoch) + 1 CXL store (thread_slot) ≈ **1.2 µs**
- `rcu_exit`: 1 CXL store ≈ **0.6 µs**
- **Total: ~1.8 µs per cross-host read**

### Optimization: cached publish_epoch
- Reader keeps DRAM-cached `last_seen_publish_epoch`
- On `rcu_enter`: only CXL-load publish_epoch every Nth call; otherwise reuse cached. Always CXL-store thread_slot.
- Reduces overhead to ~0.6 µs steady-state (1 CXL store only)

**Decision**: implement optimization in v1 (cheap to add, big win).

---

## 候选 C: Hazard pointers

### Data structures

新文件 `src/cxl_hazard.h` + `src/cxl_hazard.cc`:

```cpp
// All in CXL so cross-host visible.
// Per-thread: 1 hazard slot (FUSEE read fast-path is single-outstanding per thread → 1 enough).
struct CxlHazardDomain {
  struct ThreadSlot {                    // padded to 64B
    std::atomic<uint64_t> hazard_blk_off; // 0 = idle, else = currently-reading blk_off
    uint64_t _pad[7];
  } thread_slots[kNumHosts][kMaxThreadsPerHost];
};
```

### API

```cpp
// Reader: after receiving blk_off from owner, before pool_->read.
// Returns true if blk_off still valid (re-check pattern).
inline void hazard_protect(CxlHazardDomain *d, int host, int tid, uint64_t blk_off) {
  d->thread_slots[host][tid].hazard_blk_off.store(blk_off, std::memory_order_release);
  flush_line(&d->thread_slots[host][tid].hazard_blk_off);
  store_fence();
}

inline void hazard_release(CxlHazardDomain *d, int host, int tid) {
  d->thread_slots[host][tid].hazard_blk_off.store(0, std::memory_order_release);
  flush_line(&d->thread_slots[host][tid].hazard_blk_off);
  store_fence();
}

// Owner: check if blk_off is protected by any reader. Returns true if free to reclaim.
inline bool hazard_safe_to_free(CxlHazardDomain *d, uint64_t blk_off) {
  for (int h = 0; h < kNumHosts; ++h) {
    for (int t = 0; t < kMaxThreadsPerHost; ++t) {
      flush_line(&d->thread_slots[h][t].hazard_blk_off);
      full_fence();
      uint64_t p = d->thread_slots[h][t].hazard_blk_off.load(std::memory_order_acquire);
      if (p == blk_off) return false;
    }
  }
  return true;
}

// Owner: deferred-free wrapper.
inline void hazard_retire(CxlHazardDomain *d, uint64_t blk_off, std::function<void()> free_fn) {
  retire_list.push_back({blk_off, free_fn});
  // Periodically (e.g., every K retires) scan and free.
}
```

### Per-read overhead (Hazard)
- `hazard_protect`: 1 CXL store ≈ **0.6 µs**
- `hazard_release`: 1 CXL store ≈ **0.6 µs**
- **Total: ~1.2 µs per cross-host read**

### Re-validation requirement (Hazard standard pattern)

Hazard 有 small race window: reader 拿到 blk_off → 写 hazard slot → 但 owner 在两步之间 free 了 blk_off → reader 写的 hazard 已无效。

标准解决：reader 写完 hazard slot 后**重新读** bucket，确认 blk_off 还在 → 没变才能 read。

但 FUSEE 当前 read 路径里 reader 是从 owner 的 staging 拿到 blk_off (cross-host)，**不**直接读 bucket。所以这个 race 在 cross-host 读上无法用 "re-read bucket" 解决。

替代：让 owner 在 publish staging 之前先确保 reader hazard 已生效。Owner read_handler 流程：
1. lookup bucket → blk_off
2. **写 staging.blk_off, lookup_epoch, vlen**
3. publish staging.ready_op_id (release)
4. Reader 看到 ready_op_id → 写 hazard_blk_off = blk_off → reread staging.blk_off 确认（如果 owner 在第 2 步后 free 了 block 又 alloc 给 new write 同 blk_off ABA → 那 staging.blk_off 不变也会出 bug，但 ABA 在 bump-only allocator 下不会发生，未来 freelist allocator 才需要 generation tag）

iter-13A 200k-ops bump-only 下没有 ABA 问题，hazard 的 re-validation 暂不实现，记入 iter-14A backlog。

---

## RAP §XIII (≥ 6 categories per CLAUDE.md)

### V_PERFORMANCE

**Attack 1**: RCU 的 1 CXL load on enter — 比 Hazard 的 0 load 多 0.6 µs，被 hazard 完胜？
- **Defense**: 加 cached publish_epoch 后，RCU enter 只在 publish_epoch 变化时 CXL-load (rare events)。**稳态 RCU = 1 CXL store, Hazard = 1 CXL store**。等价。
- **Verdict**: 稳态 tie. iter-13A 实测见分晓。

**Attack 2**: Hazard 的 re-validation 复杂度 + ABA 风险在未来 GC 下变成阻塞 hazard？
- **Defense**: 现在 bump-only 没有 ABA。iter-14A 引入 freelist GC 时加 generation tag (16-bit per blk_off 高位)，cost = 0 (复用 encoded high bits)。
- **Verdict**: future-proof OK.

**Attack 3**: Owner sync cost (RCU rcu_synchronize) vs Hazard scan — 哪个 free 路径更便宜？
- **Defense**: 
  - RCU sync: 必须 wait 所有 reader 离开 target_epoch ≤ — 可能 spin 几十 µs
  - Hazard scan: O(N_threads) CXL loads per free, but no wait
- **Verdict**: 200k-ops 测试期间 free 路径 NEVER triggers (bump-only)，所以这个差异**对 throughput 无影响**。Free-path 成本不算入 winner 选择。

### V_CORRECTNESS (§I9)

**Attack 4**: Direct pool read 后 owner 可能 update slot 指向新 blk_off，reader 是不是读到 stale value 破坏线性化？
- **Defense**: 当前 pool 不 free → old block 内容不变 → reader 看到 committed-at-time-of-capture 的 value，逻辑时间点 = 它捕获 blk_off 那一刻。这本身就是 linearizable point of operation。  
- C13 epoch validation 不变：staging.lookup_epoch ≥ my_epoch_at_send 必须 pass，否则 stale → retry。所以 reader 不会观察到 "比自己 send epoch 还旧" 的 value。
- **Verdict**: §I9 preserved.

**Attack 5**: Hazard slot 写晚于 owner 收到 request 的 race？
- **Defense**: Owner 收到 request 时只查 bucket + 写 staging control fields, 不 free（free 是另一个独立 trigger）。reader 在收到 staging.ready_op_id 后才 hazard_protect，但此时如果 owner 已经在另一个并发流上准备 free 这个 blk_off？bump-only 下不会发生 free，所以无问题。Future GC 下需要 reader hazard_protect → owner re-check; 见上述 Attack 2 Defense.
- **Verdict**: pass under current pool semantics.

### V_GENERALITY

**Attack 6**: 哪个方案对未来 freelist GC 适应性更强？
- **RCU**: 对 GC 自然 — owner free 时 advance epoch + sync wait。Block 可以立即 free 在 sync return 后。
- **Hazard**: GC 时需要 scan 所有 hazard slot 找 "没人持有的 blk_off"。要么 retire_list 累积一批一起 scan (amortize), 要么每次 free 都 O(N) scan。
- **Verdict**: RCU 略胜 for future-proof GC，但都 viable。

### V_COMPLEXITY

**Attack 7**: 哪个实现行数少？
- RCU: ~150 LOC for cxl_rcu.{h,cc} (epoch counter + thread slot scan + cached publish_epoch optimization)
- Hazard: ~120 LOC for cxl_hazard.{h,cc} (slot store + retire list + scan)
- 都在 user-allowed 范围内（C5 已豁免 C17 limit）

### V_PRIOR_ART

- **RCU**: Linux kernel (1992+), 是 read-mostly 场景的标杆。最近 user-space RCU (URCU) 库被 DPDK / RAMCloud / TiKV 广泛使用。
- **Hazard pointers**: Maged Michael 2004，标准并发数据结构 (e.g., Folly's HazPtr, lock-free queues)。
- 两者都有大量工业案例。

### V_IMPLEMENTATION_FEASIBILITY

**Attack 8**: 编译开关 `-DFUSEE_READ_GUARD={STAGING,RCU,HAZARD}` 的实现方式？
- 三个 build dir: `build-cxl` (STAGING baseline), `build-cxl-rcu`, `build-cxl-hazard`
- Each `cmake -DCMAKE_CXX_FLAGS="-DFUSEE_READ_GUARD=RCU" ...` 等
- `cxl_kv_ops_A.cc::forward_read_direct` + `read_handler` 用 `#if FUSEE_READ_GUARD == RCU` 分支
- **Verdict**: feasible, 标准编译时分支。

### V_DIAGNOSTIC_PROVENANCE

**Attack 9**: 怎么验证 winner pick 不重蹈 iter-12A V1 错误 (推断而非观测)？
- **Defense**: 双轨 5 代表性 cell × 5 reps → 取 median throughput + r_avg + r_p99，**数字 vs 数字**。比较表写进 `iter13A_phase1_compare/VERDICT.md`。
- 任何超出 5% throughput 差异的 winner 都必须有 r_avg / r_p99 数据支持，否则 redo。
- **Verdict**: evidence-based pick guaranteed.

---

## ABLATION

Phase 1 winner 选定后，重跑 baseline `STAGING` build 在同样 5 cell × 5 reps，确认数据没漂移。如果 STAGING 数字与 Phase 0 baseline 不一致 → 测试环境不稳，winner pick 暂停 reopen。

---

## DECISION

**实现两个候选 (RCU + Hazard)，跑代表性 cell 对比，按测量数字 pick winner**。

实现顺序:
1. 先 Hazard (简单 + 无 epoch cache 优化)
2. 后 RCU (含 cached publish_epoch 优化)
3. STAGING baseline 保持 default

5 代表性 cell:
- workloada T=4 cache=off kv=512
- workloada T=32 cache=off kv=1024
- workloadb T=32 cache=off kv=256
- workloadc T=64 cache=off kv=1024 ← read-only, 最受 read 优化影响
- workloadf T=32 cache=off kv=512

输出: `docs/iter13A_phase1_compare/SUMMARY.tsv` + `VERDICT.md`

Winner pick 规则:
1. **Primary**: median throughput on workloadc T=64 (最 read-dominant)
2. **Tie-break**: median r_p99 across all 5 cells
3. **Veto**: winner 的 r_avg 在任何 cell 不能 regress > 30% from STAGING baseline
