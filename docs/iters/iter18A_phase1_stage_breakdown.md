# iter-18A Phase 1 — xhost_read stage 划分（请用户过目）

**目的**: 在进入 Phase 2 全 sweep 之前，把 Phase 1 划定的 stage / substage 边界 + Phase 1.5 sanity 数据呈给用户验收。
**Status**: Phase 1.1-1.5 已完成，待用户对 stage 划分确认后进 Phase 2。

---

## 1. Worker side（6 stages）— 位于 `CxlKvStoreA::forward_read_direct`

### 路径概览

```
Worker T 上发起 GET(key)，owner = host 1 (peer)
  │
  ├── compute_ring_idx(key)                 ← cheap
  ├── ring = &rr_->rings[me][owner][shard]
  ├── op_id = read_op_counter_.fetch_add()  ← thread-local atomic
  │
  ◇ PROBE XRS1S  ──┐
  │                │  Stage 1: slot_reserve
  ├── ring->tail.fetch_add(1)               ← CXL atomic (cross-host coherence)
  ├── flush_line(&ring->tail)                ← clflushopt
  ├── store_fence()                          ← sfence
  │                │
  ◇ PROBE XRS1E  ──┤
  │                │  Stage 2: slot_wait (spin until slot's req_op_id == 0)
  ├── loop: flush_line(&e->req_op_id) + full_fence + load
  ├── XRS2R 条件 probe (c_iters > 0 = 实际自旋)
  │                │
  ◇ PROBE XRS2E  ──┤
  │                │  Stage 3: req_publish
  ├── (if RCU) rcu_enter()
  ├── my_epoch_at_send = cache_pool_bucket_epoch(key)
  ├── ReadStagingSlot *st = ...
  ├── st->ready_op_id.store(0)                ← clear stale ack
  ├── flush_line(&st->ready_op_id) + sfence
  ├── e->key/resp_op_id/status/.. populate
  ├── e->req_op_id.store(op_id) + flush_line + sfence
  │                │
  ◇ PROBE XRS3E  ──┤
  │                │  Stage 4: ack_wait (DOMINANT)
  ├── loop: flush_line(&st->ready_op_id) + full_fence + load
  ├── XRS4T 条件 probe (timeout 200ms)
  │                │
  ◇ PROBE XRS4E  ──┤
  │                │  Stage 5: cleanup_validate
  ├── e->req_op_id.store(0) + flush + sfence  ← free ring slot
  ├── flush_line(st) + full_fence              ← C13 epoch validate
  ├── if (st->lookup_epoch < my_epoch_at_send) → return -3
  ├── if (st->status != 0) → return st->status
  ├── if (vlen == 0 || vlen > kReadStagingSlotBytes) → early return
  │                │
  ◇ PROBE XRS5E  ──┤
  │                │  Stage 6: value_recv
  ├── STAGING build: for (off ∈ vlen/64) flush_line(st->value_bytes+off)
  ├──                 memcpy(out_buf, st->value_bytes, copy_len)
  │   (or RCU/HAZARD build: pool_->read(blk_off+4, out_buf, copy_len))
  │                │
  ◇ PROBE XRS6E  ──┘
  │
  └── return 0
```

### Substage 边界（Phase 3 候选改动范围预览）

| Stage | Substage 1 | Substage 2 | Substage 3 |
|---|---|---|---|
| **1 slot_reserve** | atomic fetch_add | flush_line(tail) | sfence |
| **2 slot_wait** | flush_line(req_op_id) | full_fence | atomic load (`acquire`) |
| **3 req_publish** | clear staging ack slot | populate entry fields | atomic store + flush + sfence |
| **4 ack_wait** (dom) | flush_line(ready_op_id) | full_fence | atomic load + pause |
| **5 cleanup_validate** | free ring slot | flush_line(st) + fence | epoch/status branch |
| **6 value_recv** | per-64B flush_line loop | memcpy / pool read | — |

Phase 3 候选改动**必须落在某个 substage 内**（per C8）。例：
- "Stage 4 改 pause 节奏" = Stage 4 / Substage 3 内部
- "Stage 3 收 fence-fan-out" = Stage 3 / Substage 3 内部
- "Stage 6 single-flush 整段" = Stage 6 / Substage 1 内部
- 不允许"把 Stage 5 的 cleanup 挪到 Stage 6 之后"——跨 stage 边界

---

## 2. Receiver side（3 stages）— 位于 `CxlKvStoreA::read_receiver_loop` + `read_handler`

### 路径概览

```
Receiver loop on host 1
  while (!stop):
    for ring_idx ∈ owned_shards:
     for src ∈ {0, num_hosts}:        ← cross-host inbound
        ring = &rr_->rings[src][me][shard]
        head = ring->head             ← receiver-local
        │
        ◇ PROBE XRR1S (head)  ──┐
        │                       │  Stage R1: ring_drain
        ├── flush_line(&ring->tail) + full_fence
        ├── tail = ring->tail.load(acquire)
        │ while head < tail:
        │   slot = head % depth
        │   e = &ring->entries[slot]
        │   flush_line(&e->req_op_id) + full_fence
        │   op_id = e->req_op_id.load(acquire)
        │   if op_id == 0:
        │     PROBE XRR1Z (gap detected)
        │     gap-budget spin 4096 iter
        │     if still 0: PROBE XRR1X + break
        │                       │
        │  ◇ PROBE XRR1E (op_id) ┤
        │                       │  Stage R2: handler (bucket+pool+staging)
        │   ┌─ read_handler(e, src, slot):
        │   │    flush_line(bucket) + full_fence
        │   │    bucket scan for key
        │   │    slot_directory_lock(de)
        │   │    de->sharer_bitmap |= (1u << src)
        │   │    encoded = bucket->slots[s].value
        │   │    slot_directory_unlock(de)
        │   │    branch on size class:
        │   │       Inline: memcpy(st->value_bytes, &encoded, 8)
        │   │       Pool:   pool_->read(hdr) + pool_->read(value)
        │   │               for off ∈ vlen/64: flush_line(st->value_bytes+off)
        │   │    st->key/value_size/status/lookup_epoch populate
        │   │    flush_line(st)
        │   │    st->ready_op_id.store(req_op_id) + flush + sfence
        │   └─
        │                       │
        │  ◇ PROBE XRR2E (op_id) ┤
        │                       │  Stage R3: ack_publish
        │   atomic_thread_fence(release)
        │   e->resp_op_id.store(op_id, release)
        │   flush_line(&e->resp_op_id) + sfence
        │                       │
        │  ◇ PROBE XRR3E (op_id) ┘
        │   head++
```

### Substage 边界

| Stage | Substage 1 | Substage 2 | Substage 3 | Substage 4 |
|---|---|---|---|---|
| **R1 ring_drain** | flush(tail) + fence | tail load (acquire) | per-slot flush(req_op_id) + fence + load | gap-tolerance spin (XRR1Z/X) |
| **R2 handler** | bucket flush + scan | dir lock + sharer bitmap | size-class branch / pool read | staging slot publish (key, vlen, status, ready_op_id store + flush) |
| **R3 ack_publish** | release fence | resp_op_id store + flush + sfence | — | — |

**注意**: Stage R2 内部细分（bucket / dir / pool / staging publish）目前**不单独 probe** — 因为 read_handler 总耗时只占 receiver 整体的 ~16-21 %（见下面 sanity 数据）。如果 Phase 2 V-sweep 暴露 R2 是 dominant（kv 大值 / pool read 时间增长），Phase 3 再添 substage probe。

---

## 3. Phase 1.5 Sanity 数据（V=1024, zipf-0.99, N=0, FUSEE_CACHE=0）

### 3.1 Stage breakdown（中位数 ns，TRANS_OPS=200k probe-on）

| Stage | T=1 | T=8 | T=64 | % of StageW @ T=64 |
|---|---:|---:|---:|---:|
| 1. slot_reserve | 524 | 518 | 471 | 0.2% |
| 2. slot_wait | 1108 | 1083 | 1006 | 0.5% |
| 3. req_publish | 104 | 107 | 113 | 0.1% |
| **4. ack_wait** | **4686** | **22298** | **198607** | **96.8%** ★ |
| 5. cleanup_validate | 1675 | 1576 | 1248 | 0.6% |
| 6. value_recv | 1842 | 2839 | 3302 | 1.6% |
| **StageW (worker total)** | **9985** | **28609** | **205076** | 100% |
| R1. ring_drain | 2334 | 3179 | 2974 | (recv side) |
| R2. handler | 1288 | 608 | 580 | (recv side) |
| R3. ack_publish | 15 | 16 | 16 | (recv side) |
| **StageR (recv total)** | **3653** | **3830** | **3582** | — |
| **RTT = StageW − StageR** | **6336** | **24569** | **201476** | 98.2% of StageW @ T=64 |

**Stage 4 (ack_wait) 主导比例（同 iter-16A 写路径 Stage 5 ack_wait 模式）**:
- T=1: 47% — 与单 op CXL RTT 量级匹配
- T=8: 78% — receiver 开始饱和，workers 排队
- T=64: **97%** — workers 几乎全部时间在等 ack（receiver 单线程已锁死）

### 3.2 ∑Stage = StageW sanity（probe 框架完整性）

| T | ∑(Stage1..6) | StageW | 比例 |
|---|---:|---:|---:|
| 1 | 9939 | 9985 | 99.5% |
| 8 | 28421 | 28609 | 99.3% |
| 64 | 204747 | 205076 | 99.8% |

**全 T 通过 ±5% 容差** — probe 边界覆盖完整，没有"漏抓"的代码段。

### 3.3 Probe-on vs probe-off 吞吐影响

| T | probe-off median (Mops/s, 3 rep) | probe-on (1 rep, TRANS=200k) | delta | iter-16A 写路径参考 |
|---|---:|---:|---:|---|
| 1 | 0.288 | 0.213 | **-26.0 %** | +1 % |
| 8 | 0.587 | 0.486 | -17.2 % | -14 % |
| 64 | 0.633 | 0.500 | -21.0 % | -3 % |

读路径 probe 占比比写路径更高（读 op 更短 ~10 µs vs 写 ~16 µs，但 probe 数差不多）。T=1 case -26 % 稍越 -25 % RCA threshold；但 T=1 不是 scaling 主战场，记录但不深挖。

---

## 4. 与原 plan §2.1 估计的对比

| 项 | Plan v3 §2.1 估计 | Phase 1 实际划分 |
|---|---|---|
| Stage 数量 | 8（worker 6 + receiver 2 大块） | **9（worker 6 + receiver 3）** |
| Receiver stages | 粗 2 块 | 细分到 R1/R2/R3 |
| Worker probe 数 | ≥10 | 9 mandatory + 2 conditional (XRS2R, XRS4T) = 11 |
| Receiver probe 数 | (没具体数) | 4 mandatory + 2 conditional (XRR1Z, XRR1X) = 6 |
| 总 probe sites | — | **14 mandatory + 4 conditional** |

调整原因：read_handler 是单函数但内含 4 个 substage（bucket / dir / pool / staging），把它当一个 stage 在 Phase 1.5 数据里看着合理（R2 总耗 580-1288 ns 不算 dominant），不需要细拆；ring_drain 和 ack_publish 拆开是必要的（R1=2974 ns vs R3=16 ns，差两个数量级）。

---

## 5. 等用户确认

请你 review 以下几点：

1. **Stage / substage 划分是否合理？** 特别是 R2 (handler) 当前不拆 substage，等 Phase 2 V-sweep 看是否值得拆。
2. **Phase 1.5 sanity 数据是否接受**？probe 框架完整性 ∑stage = 99.3-99.8% StageW；probe 影响 -26%/-17%/-21%（参 iter-16A 写路径 -14% worst 同量级）。
3. **Phase 4 主导是 ack_wait（与写路径同模式）**：是否影响 Phase 3 候选清单优先序？现在的 plan v3 §3.1 把 R3/R4/RR1/RR2/RR3/RR4 候选都列了，按 Phase 1.5 数据看应该把 **Stage 4 (ack_wait) substage 内的优化先做**（pause 节奏 / acquire 弱化 / flush 频率）— 因为它占 47-97 %，任何 substage 改动 5 % 都给 cluster 1-5 % 提升。

若 1-3 都 OK，我直接进 Phase 2 完整 4 sweep。
