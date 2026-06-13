# Protocol A — 4-path stage decomposition consolidated reference

**Purpose**: single-page reference for the stage decomposition of all
four hot paths in Protocol A. Captures every probe tag, what it
measures, and the source-code anchor for each stage.

**Paths covered**:

| Path | Probe prefix | Macro | Build flag | Stages | Established in |
|---|---|---|---|---:|---|
| 1. local_read | LRS | `PROBE_LR_OP` | `FUSEE_LOCAL_READ_PROBE=1` | 4 | iter-19A Phase 2.4 |
| 2. local_write | LWS | `PROBE_LW_OP` | `FUSEE_LOCAL_WRITE_PROBE=1` | 6 | iter-19A Phase 3 |
| 3. xhost_read sender | XRS | `PROBE_READ_OP` | `FUSEE_READ_PROBE=1` | 6 | iter-18A |
| 3. xhost_read receiver | XRR | `PROBE_READ_OP` | `FUSEE_READ_PROBE=1` | 3 | iter-18A |
| 4. xhost_write sender | XWS | `PROBE_OP` | `FUSEE_PROBE=1` | 5 | iter-16A |
| 4. xhost_write receiver | XWR | `PROBE_OP` | `FUSEE_PROBE=1` | 3 | iter-16A |

All probe macros emit a 24 B frame (tag[8] + rdtscp-cycles[8] + op_id[8])
into a per-thread mmap ring (`$FUSEE_PROBE_DUMP/probe.{pid}.{tid}`,
128 MB / thread). See [src/cxl_probe.h](../../src/cxl_probe.h).

---

## Path 1 — local_read (LRS)

**Function**: `CxlKvStoreA::search()` — owner-self HIT or MISS branch.
Cross-host MISS (forward_read) goes through XRS instead.
[src/cxl_kv_ops_A.cc:2865-3078](../../src/cxl_kv_ops_A.cc#L2865)

### Stage table

| # | Stage | Tags | Description | Source range |
|---|---|---|---|---|
| 1 | entry | LRS1S, LRS1E | function entry + hash + bucket idx + parameter validation | 2879-2882 |
| 2 | cache_pool_lookup | LRS2S, LRS2H, LRS2M | shared cache_pool seqlock CAS reader. H = HIT (returns to caller); M = MISS (fall through to LRS3). Path counter `lrs2r` counts seqlock retry iterations | 2919-2978 |
| 3 | cxl_miss | LRS3S, LRS3E | owner-self bucket scan + pool->read (16 cachelines for V=1024) | 3018-3061 |
| 4 | populate | LRS4S, LRS4E | cache_pool_insert + TLS L1 insert. Path counter `lrs4r` counts seqlock thundering-herd retries | 3065-3072 |

### Probe trace per op (success path)

- HIT op: `LRS1S → LRS1E → LRS2S → LRS2H` (4 emits, returns)
- MISS op: `LRS1S → LRS1E → LRS2S → LRS2M → LRS3S → LRS3E → LRS4S → LRS4E` (8 emits)

### Derived per-op latencies

| Name | Formula | Notes |
|---|---|---|
| stage1 | LRS1E − LRS1S | constant, ~50 ns |
| stage2 | LRS2H or LRS2M − LRS2S | seqlock read; HIT cost includes 1KB DRAM memcpy |
| stage3 | LRS3E − LRS3S | bucket scan + CXL pool->read (1KB) |
| stage4 | LRS4E − LRS4S | cache_pool seqlock CAS + 1KB DRAM memcpy |
| total_HIT | LRS2H − LRS1S | HIT-path total |
| total_MISS | LRS4E − LRS1S | MISS-path total |

### Key findings to date (iter-19A)

- HIT path is ~95% of ops under typical cache_pct=100 + zipf-0.99.
- Stage 2 (HIT) at high T dominated by LRU touch ping-pong (B-H2) +
  hashtable hot bucket — Anomaly B mechanism (5.9× collapse at zipf-1.5).
- Stage 3 (MISS) under FUSEE_LR_DEL_OWNER_FLUSH default skips bucket
  pre-scan flush+mfence (B-H3); 16-cacheline pool->read flush is still
  active (iter-19A Phase 3 isolation candidate G2).

---

## Path 2 — local_write (LWS)

**Function**: `CxlKvStoreA::execute_write_local()` — owner-self write.
INSERT / UPDATE / DELETE all flow through this function.
Forwarded write (peer→owner) uses `execute_write_local_with_blk` — same
structure minus block alloc + write.
[src/cxl_kv_ops_A.cc:529-740](../../src/cxl_kv_ops_A.cc#L529)

### Stage table

| # | Stage | Tags | Description | Source range |
|---|---|---|---|---|
| 1 | entry+slotscan | LWS1S, LWS1E | parameter validation + hash + bucket scan to find match / empty slot | 542-568 |
| 2 | slot_dir_lock | LWS2S, LWS2E | `slot_directory_lock` (pthread_spin on DRAM directory) | 571-573 |
| 3 | sharer_inval | LWS3S, LWS3E, LWS3B | read sharer_bitmap + broadcast OP_INVALIDATE to peer-host sharers via InvalRing aggregator. LWS3B emitted on broadcast-taken (at least 1 sharer to invalidate) | 593-613 |
| 4 | cow_publish | LWS4S, LWS4E, LWS4A, LWS4W | block alloc + CXL write + publish_slot_cow. Sub-anchors A=post-alloc, W=post-CXL-write (blockpool path only) | 622-690 |
| 5 | dir_state | LWS5S, LWS5E | de->version++ + de->state + de->sharer_bitmap + spin_unlock (DRAM) | 693-705 |
| 6 | own_cache | LWS6S, LWS6E | cache_pool_insert + TLS L1 insert (or _evict on DELETE) | 708-738 |

### Probe trace per op (success path)

- INSERT (no sharers): `LWS1S → LWS1E → LWS2S → LWS2E → LWS3S → LWS3E → LWS4S → LWS4A → LWS4W → LWS4E → LWS5S → LWS5E → LWS6S → LWS6E` (14 emits)
- UPDATE w/ peer sharers: same + LWS3B before LWS3E (15 emits)
- DELETE: same minus LWS4A/LWS4W (only retire_slot called in stage 4)

### Derived per-op latencies

| Name | Formula | Notes |
|---|---|---|
| stage1 | LWS1E − LWS1S | constant, ~50 ns |
| stage2 | LWS2E − LWS2S | pthread_spin acquire; ~10ns no-contention |
| stage3 | LWS3E − LWS3S | sharer broadcast via aggregator; grows with peer count + sender backlog |
| stage4 | LWS4E − LWS4S | DELETE: tiny; inline: ~50ns; blockpool: ~200ns @ V=1024 |
| stage4_alloc | LWS4A − LWS4S | DRAM bump pointer, ~10ns |
| stage4_cxl_write | LWS4W − LWS4A | 16-cacheline CXL flush+sfence; ~100-200ns @ V=1024 |
| stage5 | LWS5E − LWS5S | DRAM directory + unlock; ~10ns |
| stage6 | LWS6E − LWS6S | cache_pool_insert seqlock + 1KB DRAM memcpy + TLS insert; ~200-500ns |
| total_W | LWS6E − LWS1S | full local_write latency |

### Key findings to date

- Probes just inserted in iter-19A Phase 3 (this iter). Initial
  measurement pending Task 3c sweep.
- Anomaly A is hypothesized to surface mainly in stage 6
  (cache_pool_insert under 64-way concurrent DRAM write).
- Anomaly B mechanism (B-H3 owner-self flush storm) is on the local_read
  path; the equivalent question on local_write is whether the publish
  flushes (LWS4 cow_publish) materially cost throughput — Task 3c G3/G4/G5/G6.

---

## Path 3 — xhost_read (XRS sender + XRR receiver)

**Sender function**: `CxlKvStoreA::forward_read_direct()` — invoked by
worker when read MISS + key owned by peer host.
[src/cxl_kv_ops_A.cc:2261](../../src/cxl_kv_ops_A.cc#L2261)
**Receiver function**: `read_receiver_loop()` running on owner host.
[src/cxl_kv_ops_A.cc:2773](../../src/cxl_kv_ops_A.cc#L2773)

### Sender stage table (XRS)

| # | Stage | Tags | Description | What's measured |
|---|---|---|---|---|
| 1 | slot_reserve | XRS1S, XRS1E | atomic fetch_add on ring tail + flush + sfence | ring tail acquire cost |
| 2 | slot_wait | XRS2E, XRS2R | spin on prior owner releasing the slot (`req_op_id == 0`). XRS2R counts retry iters (path counter) | C-spin contention |
| 3 | req_publish | XRS3E | clear staging area + fill ReadEntry + atomic store req_op_id + flush + sfence | CXL cacheline write |
| 4 | **ack_wait** | XRS4E, XRS4T | spin on `ready_op_id == op_id`. XRS4T marks 200ms timeout. **Dominant stage** — includes CXL RTT + receiver work + worker pause | RTT + receiver saturation |
| 5 | cleanup_validate | XRS5E | release ring slot + flush(staging) + epoch+status validity check | post-RTT bookkeeping |
| 6 | value_recv | XRS6E | 64B-loop flush of staging value + memcpy to caller buf (or pool->read if not-staged) | value xfer cost |

### Receiver stage table (XRR)

| # | Stage | Tags | Description |
|---|---|---|---|
| R1 | ring_drain | XRR1S, XRR1E, XRR1Z, XRR1X | tail flush+load, per-slot req_op_id flush+load. Z = gap detected (req_op_id==0 on first load), X = gap budget exhausted |
| R2 | handler | XRR2E | bucket flush+scan + dir lock + size-class branch + staging publish |
| R3 | ack_publish | XRR3E | resp_op_id atomic store + flush + sfence |

### Probe trace per op

- Sender (no spin, no timeout): `XRS1S → XRS1E → XRS2E → XRS3E → XRS4E → XRS5E → XRS6E` (7 emits)
- Receiver (per drained op): `XRR1S → XRR1E → XRR2E → XRR3E` (4 emits)

### Key findings (iter-18A)

- iter-17A 217× thpt collapse on YCSB-c T=16 N=4 root cause: ReadStagingMatrix
  was 2D `[req][owner][slot]` while ReadRingMatrix is 3D
  `[req][owner][shard][slot]`. With N>0 (multi-ring shard), all shards
  responses mapped to SAME staging slot → race → corruption → workers
  spin XRS4T timeout. Fix: make staging 3D, add ring_idx param to all
  `read_staging_slot()` callsites.
- Stage 4 (ack_wait) is dominant 72-99% of XRS latency at all T.

---

## Path 4 — xhost_write (XWS sender + XWR receiver)

**Sender function**: `CxlKvStoreA::forward_write_direct()` — invoked
when worker writes a key owned by peer host.
[src/cxl_kv_ops_A.cc:1453](../../src/cxl_kv_ops_A.cc#L1453)
**Receiver function**: `write_receiver_loop()` running on owner host.
[src/cxl_kv_ops_A.cc:2415](../../src/cxl_kv_ops_A.cc#L2415)

### Sender stage table (XWS)

| # | Stage | Tags | Description |
|---|---|---|---|
| 1 | slot_reserve | XWS1S, XWS1E | `ring->tail.fetch_add` + flush + sfence (reserve ring slot on owner host) |
| 2 | slot_wait | XWS2E, XWS2R | spin on `e->req_op_id == 0` (prior owner releases). XWS2R = retry iters |
| 3 | value_xfer | XWS3E | worker writes 1024 B value to owner's CXL pool. **CPU time only — CXL propagation hidden in stage 5** |
| 4 | ctrl_publish | XWS4E | worker fills entry cacheline 1 + atomic store req_op_id + flush + sfence |
| 5 | **ack_wait** | XWS5E, XWS5T | spin on `resp_op_id == op_id`. **Dominant** — includes CXL RTT + receiver work + spin overhead. XWS5T = timeout |

### Receiver stage table (XWR)

| # | Stage | Tags | Description |
|---|---|---|---|
| 6 | rcv_poll | XWR6S, XWR6E, XWR6Z, XWR6H, XWR6X | per-op poll: flush+load ring->tail + req_op_id. Z = gap detected, H = gap healed, X = gap budget exhausted |
| 7 | rcv_work | XWR7E | bucket+dir+CoW publish in `execute_write_local_with_blk` |
| 8 | ack_publish | XWR8E | resp_op_id store + flush + sfence |

### Key findings (iter-16A)

- Stage 5 (ack_wait) dominates 72-99% at all T; at T=64 = 139 µs vs
  receiver-side StageR = 1.8-3.7 µs — receiver is NOT saturated;
  workers queue waiting for the single receiver thread.
- thpt invariant across V (V=64 vs V=1024 same thpt) → NOT BW-bound.
- thpt invariant across distribution (uniform vs zipf-1.5 same) →
  current single-receiver bottleneck masks hot-bucket effects.

---

## Compact comparison

### Path scale

| Path | Sender stages | Receiver stages | Where dominant time goes |
|---|---:|---:|---|
| local_read (LRS) | 4 (HIT: 2, MISS: 4) | — | stage 2 HIT (DRAM memcpy) under high T |
| local_write (LWS) | 6 | — | stage 6 (cache_pool_insert) hypothesis |
| xhost_read (XRS+XRR) | 6 | 3 | stage 4 ack_wait (RTT + recv saturation) |
| xhost_write (XWS+XWR) | 5 | 3 | stage 5 ack_wait (RTT + single recv) |

### Shared idioms

- **S/E start-end pair per stage** — all 4 paths.
- **branch tags** — single uppercase letter:
  - H = HIT, M = MISS (LRS2)
  - R = Retry counter (XRS2, XWS2)
  - T = Timeout (XRS4, XWS5)
  - Z = Zero / gap detected (XRR1, XWR6)
  - X = eXit / budget exhausted (XRR1, XWR6)
  - H = Healed (XWR6)
  - B = Broadcast taken (LWS3)
- **Sub-stage anchors** — capital letter mid-stage:
  - A = post-Alloc (LWS4A)
  - W = post-Write (LWS4W)

### Probe ring sizing

128 MB per-thread → 5.3M frames per thread. A 200k-op trans phase with
6 stages = 1.2M frames per worker → fits comfortably. The
`OVRFLOW` sentinel is emitted if exceeded.

### Build flags overview

| Flag | Default | Enables |
|---|---:|---|
| `FUSEE_PROBE` | 0 | Master gate for all probes |
| `FUSEE_PROBE_PATH` | 0 | Legacy W*/R*/I* path coverage probes |
| `FUSEE_READ_PROBE` | 0 | XRS/XRR (xhost_read) |
| `FUSEE_LOCAL_READ_PROBE` | 0 | LRS (local_read) |
| `FUSEE_LOCAL_WRITE_PROBE` | 0 | LWS (local_write, new this iter) |

A single build can enable any combination by passing
`-DFUSEE_PROBE=1 -DFUSEE_LOCAL_WRITE_PROBE=1 -DFUSEE_LOCAL_READ_PROBE=1`
at cmake configure.

### Analyzer scripts

| Path | Analyzer | Location |
|---|---|---|
| LRS | iter-19A LR decomp analyzer | `scripts/iter19A_local_read_decomp_analyze.py` |
| LWS | iter-19A LW decomp analyzer | TBD (Task 3c follow-up — extend LR analyzer to handle 6-stage schema) |
| XRS/XRR | iter-18A XR decomp analyzer | `scripts/iter18A_xhost_read_decomp_analyze.py` |
| XWS/XWR | iter-16A XW decomp analyzer | `scripts/iter16A_decomp_analyze.py` |

All analyzers read per-thread mmap dumps + compute per-stage p50/p99
latency histograms grouped by HIT/MISS/branch tag.
