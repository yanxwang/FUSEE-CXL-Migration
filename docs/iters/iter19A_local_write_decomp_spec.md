# iter-19A — local_write stage decomposition spec (LWS)

**Path covered**: `CxlKvStoreA::execute_write_local()`
([src/cxl_kv_ops_A.cc:529-744](../../src/cxl_kv_ops_A.cc#L529))
— owner-self write, no cross-host forward. Reached when
`owner_host(key) == host_id_` via insert/update/remove.

**Probe macro**: `PROBE_LW_OP(tag, op_id)` defined in
[src/cxl_probe.h](../../src/cxl_probe.h). Gated by build flag
`FUSEE_LOCAL_WRITE_PROBE=1` (and master `FUSEE_PROBE=1`).
Independent of FUSEE_PROBE_PATH (legacy W*/R*), FUSEE_READ_PROBE
(XRS/XRR), FUSEE_LOCAL_READ_PROBE (LRS).

**Naming convention** (matches iter-16A XWS, iter-18A XRS, iter-19A LRS):
`LWS<n><suffix>` where n=1..6 is stage index, suffix:
- `S` = stage start
- `E` = stage end
- `B` = branch taken (path marker, optional)
- `A` = intra-stage anchor (between S and E, when sub-stage timing matters)
- `W` = intra-stage anchor for CXL write completion (analogous to A)

---

## Stage table

| Stage | Tags | What it measures | Always emitted? | Source line |
|---|---|---|---|---|
| LWS1 entry+slotscan | `LWS1S` / `LWS1E` | function entry → target_slot chosen (hashtable scan of 8 slots) | yes | 542, 568 |
| LWS2 slot_dir_lock | `LWS2S` / `LWS2E` | acquire slot_directory_lock (LFM bucket lock) | yes | 571, 573 |
| LWS3 sharer_inval | `LWS3S` / `LWS3E` | read sharer bitmap → broadcast OP_INVALIDATE to non-self sharers via InvalRing → return (no wait for ACK here; ACKs handled async) | yes; LWS3B optional | 593, 613 |
|  | `LWS3B` | at least one invalidate was sent (broadcast taken, not fast-skip) | optional path tag | 606 |
| LWS4 cow_publish | `LWS4S` / `LWS4E` | block alloc + CXL write of {len,value} + publish_slot_cow OR retire_slot (DELETE) OR inline-u64 (pool==nullptr) | yes | 622, 690 |
|  | `LWS4A` | post `pool_->alloc()` (blockpool path only) | optional sub-stage | 636 |
|  | `LWS4W` | post `pool_->write()` to CXL bytes (blockpool path only) | optional sub-stage | 664 |
| LWS5 dir_state | `LWS5S` / `LWS5E` | `de->version++` + state + sharer_bitmap = self → `slot_directory_unlock` | yes | 693, 705 |
| LWS6 own_cache | `LWS6S` / `LWS6E` | cache_pool_insert / TLS insert (or _evict on DELETE) | yes | 708, 738 |

**Total probe insertions**: 15 (8 S+E pairs + 1 path tag B + 2 sub-stage tags A/W). Macro is no-op when build flag is off, zero runtime cost.

---

## Derived stage latencies

For a single op with `op_id = key`:

| Quantity | Formula |
|---|---|
| Stage 1 latency (slot scan) | `LWS1E - LWS1S` |
| Stage 2 latency (lock acquire) | `LWS2E - LWS2S` |
| Stage 3 latency (inval) | `LWS3E - LWS3S` |
| Stage 4 latency (CoW publish total) | `LWS4E - LWS4S` |
| Stage 4a (block alloc) | `LWS4A - LWS4S` (blockpool only) |
| Stage 4b (CXL write) | `LWS4W - LWS4A` (blockpool only) |
| Stage 4c (slot publish) | `LWS4E - LWS4W` (blockpool only) |
| Stage 5 latency (dir update + unlock) | `LWS5E - LWS5S` |
| Stage 6 latency (own cache) | `LWS6E - LWS6S` |
| Total local_write latency | `LWS6E - LWS1S` |

**Path counters** (per-op category):
- LWS3B emitted ↔ at least one peer host was a sharer → broadcast took the
  loop. Without LWS3B → fast-skip (INSERT, single host, or empty bitmap).
- LWS4A+LWS4W both present ↔ blockpool path. Otherwise inline-u64 or DELETE.

---

## Error-path semantics

`execute_write_local` returns early on:

| Return code | Site | Stage at exit | Probes emitted |
|---:|---|---|---|
| -1 (empty key / bad args) | line 532, 534 | pre-LWS1S | none |
| -5 (value too big) | line 535 | pre-LWS1S | none |
| -2 (INSERT match-conflict) | line 557 | between LWS1S and LWS1E | LWS1S only (orphan) |
| -3 (INSERT no empty slot) | line 558 | between LWS1S and LWS1E | LWS1S only (orphan) |
| -1 (UPDATE/DELETE not found) | line 562, 565 | between LWS1S and LWS1E | LWS1S only (orphan) |
| -4 (blockpool exhausted) | line 654 | between LWS4A and LWS4E | LWS1S..LWS4A emitted; LWS4E missing |
| -5 (block too small for value) | line 664 | between LWS4A and LWS4E | LWS1S..LWS4A emitted; LWS4E missing |
| 0 (success) | line 740 | LWS6E emitted | complete chain |

For the canonical workload (workload-a update, valid key, sized correctly,
pool has space) 100% of ops emit complete chain. Probe analyzer should
group ops by `(highest stage with E) → (next stage with S only)` to
identify error-path operations vs successful operations.

---

## Comparison with prior decomp specs

| Path | Prefix | Stages | Probe macro | Build flag |
|---|---|---:|---|---|
| xhost_write sender | XWS | 5 | PROBE_OP | FUSEE_PROBE |
| xhost_write receiver | XWR | 8 | PROBE_OP | FUSEE_PROBE |
| xhost_read sender | XRS | 6 | PROBE_READ_OP | FUSEE_READ_PROBE |
| xhost_read receiver | XRR | 3 | PROBE_READ_OP | FUSEE_READ_PROBE |
| local_read | LRS | 4 | PROBE_LR_OP | FUSEE_LOCAL_READ_PROBE |
| **local_write** | **LWS** | **6** | **PROBE_LW_OP** | **FUSEE_LOCAL_WRITE_PROBE** |

local_write has 6 stages vs local_read's 4 because:
- adds slot-directory lock acquire (stage 2)
- adds sharer-invalidate broadcast (stage 3)
- adds own-cache-insert from write path (stage 6)
- replaces local_read's "cxl_miss bucket scan + pool->read" with
  "CoW publish via pool->write + slot publish" (stage 4)

---

## Build instructions

To enable LWS probing (g1/g2 build dir):

```bash
cd ~/FUSEE_CXL/build-cxl-w1-v1024-lrprobe   # or any existing variant
cmake -DFUSEE_PROBE=ON -DFUSEE_LOCAL_WRITE_PROBE=ON .
make -j16 protocol_a_ycsb_sweep
```

Or create a dedicated build dir for local_write decomp:

```bash
mkdir -p ~/FUSEE_CXL/build-cxl-w1-v1024-lwprobe
cd ~/FUSEE_CXL/build-cxl-w1-v1024-lwprobe
cmake -DCONSENSUS_OPT=FUSEE_OPT_A \
      -DFUSEE_PROBE=ON -DFUSEE_LOCAL_WRITE_PROBE=ON \
      -DKV_VALUE_LEN=1024 ../..
make -j16 protocol_a_ycsb_sweep
```

Runtime: dump dir env `FUSEE_PROBE_DUMP=/tmp/probe_lw_<ts>` →
analyzer reads `probe.<pid>.<tid>` files (probe.h header lines 1-30).

---

## Use cases for LWS decomp

1. **Identify Anomaly B candidate stages on local_write**. iter-19A Phase 2 isolated B-H3 on **local_read** (owner-self read clflushopt storm). The analog stage on local_write (LWS4 CoW publish — does it carry a similar avoidable flush?) is unmeasured. LWS4S→LWS4E timing under zipf-1.5 vs zipf-0.99 directly answers it.

2. **Stage 3 broadcast cost decomposition**. iter-9A introduced N:1:1:N InvalRing and the `send_invalidate` call. iter-14A added FUSEE_XHOST_WRITE_SELF_INVAL skip. Neither has direct per-op stage-3 timing.  LWS3 measures the cost of the inval-broadcast loop in production (vs the static analysis estimate).

3. **Stage 5 directory write fence cost**. The `de->version++ → state → bitmap → unlock` sequence touches multiple CXL cachelines. If `flush_line(de)` is removed (Task 3 hypothesis), LWS5 timing should change.

4. **Stage 6 cache_pool_insert thundering-herd metric**. cache_pool_insert from write path competes with concurrent cache_pool_insert from read-MISS path. LWS6 timing reveals this contention under high T.

---

## Open follow-ups

- LWS probe analyzer: extend the existing iter-19A LRS analyzer in
  `scripts/analyze_lr_probes.py` (TODO file path verify) to handle the
  6-stage LWS schema. Stage 4 sub-stages (A/W) only present on blockpool
  path — analyzer must conditionalize.
- Build variant `build-cxl-w1-v1024-lwprobe` not yet created on g1/g2.
  Defer until user kicks off Task 3.
- Sister path `execute_write_local_with_blk` (forwarded write, line 440)
  uses similar structure but without `pool_->alloc/write` (block is
  forwarded). Add `LWSF*` prefix probes if iter-20A needs xhost-forwarded
  write decomp.
