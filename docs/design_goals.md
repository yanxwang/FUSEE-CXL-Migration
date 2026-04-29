# FUSEE-CXL migration — design goals and validation discipline

## Hardware baseline (g3 / g4 — authoritative ceiling stack)

All "distance to ceiling" arguments **must** reference the numbers
in this section. Do not use vendor spec sheets, do not estimate.
g4 is mirror-configured to g3, so g3's numbers apply to both hosts.

The relevant ceilings form a **3-layer stack**, each layer is a
strict upper bound on the layer below. Whether a given workload is
"bandwidth-bound" depends on which layer is the binding constraint
for that workload's access pattern.

### Layer 1 — Hardware link ceiling (mlc, raw NT-stream)

Measured with **Intel Memory Latency Checker v3.12** with
`/dev/dax0.0` reconfigured to `system-ram` mode (so CXL appears as
NUMA node 1). User-supplied 2026-04-23 numbers cross-checked by
re-running 2026-04-25 (`g3:/tmp/mlc/Linux/mlc`):

|                                    |  DRAM (node 0) |  CXL (node 1) | DRAM / CXL |
|------------------------------------|---------------:|--------------:|-----------:|
| Idle latency (`--latency_matrix`)  |  **134.3 ns**¹ |   **604.0 ns**|  4.50 ×    |
| Peak read BW (`--bandwidth_matrix`)| **391.78 GB/s**|  **51.57 GB/s**| 7.60 ×    |
| Peak NT-write BW (`-W2`)           | 330.21 GB/s    |   54.22 GB/s  |  6.09 ×    |

Status: **measured** (not derived). Re-runnable: ssh g3, switch
dax to system-ram, run mlc, switch back to devdax (~10 min total;
caveat: kernel block-offline can hang if memory was used).

¹ DRAM latency: user's 2026-04-23 baseline was **174.8 ns**;
2026-04-25 re-test got 134.3 ns. mlc warns "Birch Stream platform,
latency optimized mode may need to be turned on" — the gap is
BIOS prefetcher state, not measurement error. The CXL latency
604.0 ns reproduced to within 0.6 % of the 607.8 ns baseline, and
both BW numbers reproduced to within 0.5 %. Use **174.8 ns** as
the conservative DRAM latency reference, **604 ns** as the
authoritative CXL latency.

Earlier doc described 393.22 / 51.78 GB/s as "write bandwidth";
that labeling was inadvertent — those numbers are mlc's default
**read-only** matrix. The CXL link is approximately symmetric
(read 51.57 vs NT-write 54.22), so the conclusion does not move,
but use 51.78 GB/s as **"peak read BW"** and 54 GB/s as
**"peak NT-write BW"** in subsequent reasoning.

### Layer 2 — User-space achievable (M1 dual-host AVX-512 NT-stream)

Measured with `tests/cxl_dualhost_bw_bench.cc` (commit `5c83965`):
both hosts spawn N pthreads each issuing
`_mm512_stream_si512()` to disjoint halves of an 8 GiB CXL devdax
region for 5 s, sfence every 4 KiB. Independent of N (saturates
at N=1):

| metric                                   | value         | vs Layer 1   |
|------------------------------------------|--------------:|-------------:|
| Per-host sustained NT-write              |  **12.5 GB/s**| 0.23 × of 54 |
| Dual-host aggregate                      |  **25.0 GB/s**| (= 2 × per-host — expander supports parallel hosts) |

Status: **measured**. This is the actual user-space
upper bound under devdax + AVX-512 NT-stream + per-4 KiB sfence.
The 4–5× gap to Layer 1 comes from devdax-mode losing kernel
page-level channel interleaving (mlc system-ram mode gets it
free) and per-thread access-pattern differences vs mlc's tuned
kernel. See "Why Layer 2 ≠ Layer 1" note below.

### Layer 3 — FUSEE protocol-C write path (derived from sweep data)

The FUSEE write path adds further overhead on top of Layer 2:
`memcpy + clflushopt-per-cacheline + sfence + bucket-cacheline
flush + bump_epoch (CXL atomic) + LFM lock`. Derived from
iter-5 9-sweep data:

| workload | per-op writer bytes | Layer-2 ceiling at this size | iter-5 best peak | Layer-3 utilisation |
|----------|--------------------:|-----------------------------:|-----------------:|--------------------:|
| B kv=256 (best=N=1)   | 320 B  | 78 Mops/s  | 27.50 Mops/s | **0.35** |
| B kv=512 (best=N=4)   | 576 B  | 43 Mops/s  | 25.53 Mops/s | **0.59** |
| B kv=1024 (best=N=2)  | 1088 B | 23 Mops/s  | 16.73 Mops/s | **0.73** |

Status: **derived** (Layer 2 measured / iter-5 sweep measured /
arithmetic). The Layer-3 utilisation answers "how close are we to
the link ceiling?". 0.73 at kv=1024 means kv=1024 is **truly
BW-bound** (close to Layer 2 ceiling); 0.35 at kv=256 means
kv=256 is **latency / lock-bound** (lots of headroom for protocol
optimisation before BW becomes the binding constraint).

### Why Layer 2 ≠ Layer 1 (the 51 GB/s vs 12.5 GB/s puzzle)

Three contributors to the 4× gap, in order of believed magnitude:

1. **Devdax loses kernel-managed channel interleaving.** mlc
   system-ram measurements get the device's internal memory
   channels striped at page granularity by the Linux node-1
   memory allocator. devdax exposes the device as a single
   contiguous virtual region with no interleaving — a single
   contiguous write stream effectively targets one channel at a
   time and serialises on that channel's queue. **Estimated 2–3×
   loss.**
2. **mlc uses tuned kernels.** mlc's NT-store loop is
   hand-optimised to maximise concurrent issued writes per core
   (LFB / WCB occupancy), with prefetch hints and stride
   patterns chosen to keep all WCBs in-flight. M1's
   `_mm512_stream_si512` in a flat per-thread loop is naive by
   comparison and likely under-uses the CPU's WCB issue width.
   **Estimated 1.3–1.6× loss.**
3. **sfence-per-4 KiB pacing.** Each sfence drains the WC buffer
   and waits for outstanding stores to retire to CXL.
   At ~12.5 GB/s = 326 ns per 4 KiB chunk, an sfence costs
   ~30–50 ns each = ~10 % overhead. **Estimated 1.1× loss.**

These multiply: ~2.5× × 1.5× × 1.1× ≈ 4× — matches the observed
51.6 / 12.5 = 4.1× gap. Status: **explanatory hypothesis,
not measured.** Confirming each contributor independently would
require a microbench for each (e.g. a system-ram backed M1
re-run, an sfence-frequency sweep). For ceiling-of-FUSEE
purposes, **Layer 2's 12.5 GB/s/host is the operative ceiling**
because FUSEE uses devdax + AVX-NT + sfence — the same regime
M1 measures.

### What this means for tuning decisions

- **Use Layer 2 (12.5 GB/s/host, 25 GB/s aggregate) as the
  ceiling for FUSEE write throughput**, not Layer 1.
- **Layer 1 vs Layer 2 gap is recoverable** if we want — switching
  to system-ram mode on the FUSEE region would in principle expose
  ~2–3× more BW headroom. Cost: lose devdax's mmap predictability,
  lose explicit cacheline placement control, and any kernel
  allocation on the CXL pages risks the offline-hang we saw on
  2026-04-21. **Out of scope for current iters; revisit if Layer 2
  becomes the binding constraint AND no protocol-side optimisation
  remains.**
- **Layer 3 utilisation chart above is the iter-6 priority signal**:
  workloads with utilisation < 0.5 (B kv≤512, A any kv, F any kv,
  C any kv) have material upside from protocol optimisations
  (multi-flusher V2 already proven insufficient — the next lever is
  hot-bucket sharding). Workloads at > 0.7 (B kv≥1024) only move
  by reducing per-op byte cost (value-cache, increment-update) or
  by switching to system-ram mode.

### Quick derived budgets (use Layer 2 numbers)

- **Per-op write-bytes budget at 20 Mops/s, single-host**:
  12.5 GB/s ÷ 20 M ops/s = **625 B/op**. bucket+kv512 (576 B) just
  fits; kv1024 (1088 B) exceeds by 1.7×. **kv1024 cannot reach
  20 Mops/s without 2× host-aggregation OR without leaving devdax
  mode.**
- **Per-op write-bytes budget at 20 Mops/s, dual-host aggregate**:
  25 GB/s ÷ 20 M ops/s = **1.25 KB/op**. kv1024 fits with 16 % slack.
  This is why iter-5 kv=1024 best = 17 Mops/s sits just below 20.
- **Latency floor per cross-host op**: ≥ 604 ns × 2 round-trips =
  ~1.2 µs. Any p50 < 1.2 µs on a cross-host path is an artefact
  (cache hit, batched op, bench noise) — not real cross-host
  latency.
- **Per-op CPU budget at 20 Mops/s = 50 ns**. CXL access at 604 ns
  is ~12× the per-op budget — we can afford **at most 1 CXL access
  per op on the synchronous fast path**, and that access must be
  amortised across many concurrent clients.

## North-star throughput target

The CXL migration is considered **successful only if both read and
write workloads sustain ≥ 20 Mops/s** on the 2-host (g3 + g4) testbed:

- **YCSB-C** (100 % read, Zipfian) ≥ **20 Mops/s** aggregate
- **YCSB-A** (50 % read + 50 % update, Zipfian) ≥ **20 Mops/s** aggregate

Until both bars are met, the migration is **incomplete**. A 3–5×
improvement over the prior baseline is not a stopping condition — it
is a checkpoint; measure the remaining gap to 20 Mops/s explicitly
before moving on.

## Why 20 Mops/s is the right target (and how to overturn it)

20 Mops/s is derived from the **measured hardware envelope** (see
"Hardware baseline" section above for sources):

- CXL random-access latency: **607.8 ns** (mlc, g3)
- CXL write bandwidth: **51.78 GB/s** per host (mlc, g3)
- Local DRAM latency / write BW: **174.8 ns / 393.22 GB/s** (g3)
- LFM fast-path (uncontended): ~3 µs per critical section (currently)
- RACE bucket layout: 2 cachelines, 7 slots, hash-addressed

For a write to cost < 50 ns of amortized CPU time (the 20 Mops/s
budget at T = 64–86 clients/host) we have a plausible slack in the
CXL latency (one round-trip ≈ 300 ns, amortized across clients on
independent buckets). The target is not soft: it presupposes we can
drive writes mostly through **local DRAM cache + 1 CXL cacheline
publish** per op, with bucket-granularity coordination not
serializing on hot keys.

**Rule for overturning this target**: any claim that 20 Mops/s is
not reachable must come with (a) latency decomposition showing the
irreducible component and (b) an argument that no simpler ordering
or coarser granularity recovers it. Fuzzy arguments ("it's the
protocol overhead") are not sufficient.

## Analysis discipline (the rule I kept breaking on 2026-04-22)

Every time a benchmark completes, BEFORE claiming a phase is "done",
compute the delta to 20 Mops/s and the latency budget consumed:

1. For every (protocol, workload, T) cell compare to 20 Mops/s.
2. If below: compute `budget_remaining_ns = 1e9 / 20e6 - observed_p50_ns`.
   That number is negative if the op is slower than the target per-op
   budget. Pick the worst cells and add to the open investigation list.
3. For each open investigation cell, do a latency decomposition: wrap
   the hot path with rdtscp-style timers (or clock_gettime if
   simpler) around the 4–5 sub-stages:
   `lock_acquire | pre_scan | publish_slot | dispatch | bump_epoch + unlock`.
   Report the median and p99 of each stage.
4. Identify the dominant stage. Decide whether it's (a) CXL-bound
   (hardware), (b) fence/serialization overhead (tunable), (c) lock
   contention (granularity or algorithm). Only (a) is acceptable as
   "irreducible"; (b) and (c) are open work items.
5. Do NOT stop at "we got a 3x improvement". Stop when either the
   cell hits 20 Mops/s or step 4 concludes "(a) irreducible".

This discipline applies every time a result is reported — even on
non-scaling-sweep micro-benchmarks.

## Current distance to target (2026-04-22 v4 sweep)

| workload | opt | best thpt (Mops/s) | at T | gap to 20 Mops/s |
|---|---|---|---|---|
| workloada | A | 0.60 | 4 | 33× |
| workloada | B | 0.92 | 4 | 21× |
| workloada | C | 1.08 | 4 | 19× |
| workloada | (all) @ T=32 | 0.16–0.56 | 32 | 36–125× |
| workloadc | C | 51.2 | 86 | **met** |
| workloadd | C | 45.8 | 86 | **met** |

**YCSB-C is met. YCSB-A is ~20× away** on every protocol. This is
the open work.

## Updated distance to target (2026-04-24 iter-3 phase-3 sweep, protocol C only)

After four iterations of C write-path work (per-slot LFM, atomic
epoch-outside-crit-section, read-singleshot + flush-collapse +
route-seq, and finally per-host DRAM ring UPDATE micro-batching):

| workload | opt | best thpt (Mops/s) | at T | gap to 20 Mops/s |
|---|---|---|---|---|
| workloada | C | 17.05 | 64 | 1.17 × (85 % of bar)|
| workloadb | C | **33.37** | 64 | **met** ✓ |
| workloadc | C | 48.73 | 86 | **met** (regressed 12 % vs phase-2 55.12) |
| workloadd | C | 33.92 | 64 | **met** (regressed 14 % vs phase-2 39.50) |
| workloadf | C | **20.48** | 64 | **met** ✓ |

See `docs/g34_scaling_ycsb_C_only_20260424_052400/iteration_note.md`
for the micro-batching design and the remaining-gap analysis on
workload A (single-flusher saturation at T > 64 — flusher sharding is
the documented next step).

## Protocol-C under micro-batching — relaxed LRC bound

With `FUSEE_BATCH_K > 0` (runtime env) protocol C batches UPDATE
writes into a per-host DRAM ring and amortises one cross-host
`bump_epoch` over K batched UPDATEs. Peer-host readers only observe
the materialised slot state + `write_epoch` — ring entries are
invisible to the peer. Formal staleness bound for UPDATEs under
batching:

  `peer_visibility_lag ≤ T_flush_us + cxl_epoch_latency (~3 µs)`

where `T_flush_us` is the configured flusher interval. INSERT and
DELETE remain on the iter-2 per-op path and keep the original
"readers see writes immediately after the completing UPDATE returns"
guarantee. When `FUSEE_BATCH_K = 0` (default), the relaxed bound does
not apply and all of INSERT/UPDATE/DELETE use the stricter per-op
path.

`FUSEE_BATCH_MERGE_SAME_KEY=ON` (default) further strengthens the
peer bound to "the peer may miss any intermediate UPDATE on a key
within a single batch; only the last write in each batch is
guaranteed to become peer-visible."


---

## Protocol A v2 — directory-based cache coherence (iter-4A 及之后)

> **本节是硬约束**。任何 iter-4A 及之后的实现、优化、benchmark
> 必须遵守。如果某个改动跟本节冲突，stop 并 escalate 给 user
> revise 本节，**不要先改代码再回头修文档**。
>
> 本节的存在是为了避免 iter-3A 之前的 silent design drift（典型例：
> Finding-1 的 routing field 默认值，把 hash table 搬到 CXL
> 单副本，etc.）。每条不变式都标注了对应的失败模式来源。

### Background — why Protocol A had to be redesigned at iter-4A

iter-3A 末尾发现两个结构性问题：

1. **iter-2A-revised 起 N:1:1:N 写入端 enqueue loop 是 runtime no-op**
   （`phys_hosts_pr_=1` 默认值），所有 sweep 数字反映的是
   "no cross-host coordination" 的 lower bound, 不是 strict-A 真实
   性能。激活后 throughput 下降 5-50× (T=64 时 56× 退化)。
2. **Hash table + KV 整体从 multi-replica DRAM 搬到 CXL 单副本**,
   违反 `docs/fusee_cxl_progress.md:410` 立下的 "preserve original
   FUSEE design as much as possible; only replace RDMA transport
   with CXL" 约束。

iter-4A 重新基于以下原则设计 Protocol A：CXL 既是 authoritative 主存
也是通讯层；DRAM 是 cache（不是 multi-replica 的 authoritative
copy）；用 directory-based MESI-style cache coherence 而非 epoch
seqlock；通过 sharding 把跨 host writer-writer 互斥转化成 op
forwarding 避免 cross-host LFM contention。

### I — Highest-level invariants（违反任何一条都是设计偏差）

| # | 不变式 | 失败模式 |
|---|---|---|
| I1 | **CXL is the unique authoritative copy** of (index + KV). DRAM in every host is pure cache. No host has an "authoritative DRAM replica"; "owner host" is a write-routing concept, not a storage-tier concept. | 偏差例: 把 hash bucket array 视作 host-local primary 副本 → 退化回 RDMA-FUSEE 的 multi-replica 模式但失去 CXL 作为唯一 source of truth 的语义。|
| I2 | **Sharding rule: key K's writer always lives on `owner_host(K)`**. Cross-host write requests are **forwarded** through N:1:1:N to the owner host before execution. Cross-host LFM lock between writers is forbidden. | 偏差例: 任何 host 直接 acquire CXL bucket lock 写 K → cacheline pingpong on b[]/ready[]/done[] in high T, throughput collapse. |
| I3 | **Same-host workers share a single cache pool via MAP_SHARED.** Directory.sharers maps to host-level (H bits, not N=N_workers bits). Same-host invalidation is a single physical store on the shared cache, not per-worker IPC. | 偏差例: 每个 worker process 各自维护 private cache → directory.sharers 维度爆炸 + invalidate 流量 ×N_workers + same-host worker-worker 协调成本. |
| I4 | **Lazy stale flag, NOT physical delete on invalidate.** Cache entry stays in DRAM with `stale=1` flag set; fast-path reader does `if entry && !entry.stale` (1 byte same-cacheline check, ≈ 0 ns overhead). Eviction is a separate background concern, decoupled from invalidation. | 偏差例: invalidation receiver 物理 free entry 内存 → invalidation 关键路径 latency ↑, batched invalidation 难做. |
| I5 | **Single-level directory: per-bucket only**. KV value blocks are **immutable** (CoW: every UPDATE allocates a new block, slot.pointer atomic CAS is the commit point). Reader follows pointer indirection naturally gets atomic snapshot of value bytes. KV-level directory tracking is therefore unnecessary. | 偏差例: 把 KV blocks 设计成 mutable + in-place update → 立即引发 reviewer attack (crash consistency, concurrent reader semantics, two code paths). 见本节末尾 "I5/I6 修订理由" 子节. |
| I6 | **All UPDATE paths use Copy-on-Write (allocate-new-block)**, NOT in-place. Same-size and size-changing UPDATE go through identical path: alloc new KV block from blockpool → write value bytes to new block → `flush + sfence` → atomic CAS `slot.pointer` → old block enters lazy GC. This matches FUSEE original design and standard practice for persistent KV stores. | 偏差例: in-place update on shared CXL → > 8B value 缺乏原子性 → partial-write race; reviewer attack #1. 见 "I5/I6 修订理由". |
| I7 | **Directory home = owner host DRAM only.** No CXL replica, no cross-host directory query path. Reader does not query directory; reader self-tracks "I'm in sharers" via local flag set during `cache_register` ACK. | 偏差例: directory 放 CXL → 每次 directory update 付 1-2 µs CXL store, hot directory entry 在 hot bucket 上的 cacheline pingpong. |
| I8 | **Directory updates use host-local atomics or spinlock on shared mem (PROCESS_SHARED), NOT LFM.** LFM is reserved for genuinely cross-host CXL state. | 偏差例: 拿 LFM 保护本机 DRAM directory → 把 cross-host coordination 工具用在不需要的地方, 引入跨主机 cacheline 流量给本来不跨主机的字段. |
| I9 | **Read fast path = local cache lookup + stale check (no `acquire-load` on shared atomic, no CXL load).** Read slow path on cache miss = register with directory (mandatory N:1:1:N message + ACK) **before** filling cache. | 偏差例: reader 先 fill cache 后 register (race window: writer 已经 invalidate 完所有当前 sharers 但 reader 还没在 sharers 里 → reader 永远不被通知 stale). |
| I10 | **Write commit point = "in-place CXL write completes after all sharer ACK received".** Writer does NOT return success until: (a) directory.sharers \\ {self} 全部 ACK invalidate done, (b) CXL write durable (clflushopt + sfence completed). 这是 strict-A linearizability 的来源. | 偏差例: writer 在 invalidate-ACK 完成前先 publish CXL 字节 → 对端 reader 可能在 ACK-before window 读到新值, 但本端 cache 还有 stale → 跨 host 不一致. |
| I11 | **Cross-host write forwarding via N:1:1:N message (SPSC ring + sender/receiver thread)**, not via cross-host LFM lock. Forward message carries (op_type, key, value bytes); response carries (status, return_value if any). | 偏差例: 用 cross-host LFM lock 互斥 writer → 见 I2. |
| I12 | **No OpLog write/read in iter-4A.** Crash recovery uses broadcast `everyone_invalidate` + 全 client cache 清空 + on-demand re-fill from CXL. OpLog 框架代码保留, 等 future replication 设计时启用. | 偏差例: iter-4A 写 OpLog 但 reader 不读 → 浪费 CXL 流量 + 误以为有 fault tolerance. |

### II — CXL 物理布局（authoritative state）

CXL `/dev/dax0.0` mmap'd region 包含:

```
[0]              GlobalHeader (4 KB)
[4 KB]           BucketLockTable[num_buckets]    ← LFM mutex per bucket; iter-4A 后 only used for bucket-layout 修改互斥, NOT for read path
[align 64]       CxlKvBucket[num_buckets]        ← (fp, len, owner_node_id, pointer) per slot, NO inline value bytes
[align 64]       PerHostSpscRing[H][H][K]        ← N:1:1:N 通讯介质 (invalidate + forward + ACK)
[align 64]       AckChannel[H][H][K]
[align 64]       KvBlockpool                     ← variable-length KV blocks, size-classed
[align 64]       (OpLog area — reserved, not written by iter-4A)
```

**CXL 上不再有 inline u64 KV 路径**。所有 value bytes 在 KvBlockpool 里, slot 仅含 pointer.

### III — DRAM 物理布局 (per host, MAP_SHARED across same-host workers)

```
[0]              ShardingTable                       ← const, attach() 时初始化
[align 64]       BucketDirectory[num_buckets]        ← per-bucket sharers + state (唯一 directory layer)
[align 64]       KvCachePool                         ← (key → cached value bytes) hashmap, lazy stale flag, LRU evict
[align 64]       LocalSelfFlags                      ← per-bucket "我在 sharers 里" flag (reader self-tracked)
```

**Directory entry 大小** (only one type, per-bucket):

```c
struct BucketDirectoryEntry {       // 16 B total
  uint8_t   state;                  // 1 B  : I=0, S=1, M=2 (transient)
  uint8_t   sharer_bitmap;          // 1 B  : 8 hosts max (g34 H=2 uses 2 bits)
  uint8_t   spinlock;               // 1 B  : host-local PROCESS_SHARED spinlock
  uint8_t   pad1;                   // 1 B
  uint32_t  version;                // 4 B  : ABA防护 + observability
  uint64_t  pad2;                   // 8 B  : alignment + future fields
};
```

总大小: num_buckets × 16 B = 1 MB / host @ 65k buckets. 极小.

**Cache entry**: hashmap (key → entry pointer); entry 含 (key, **immutable** value bytes pointer, stale flag, LRU epoch, in_sharers flag). Value bytes 本身不在 cache entry 内 — 是 CXL KvBlockpool 上的 immutable block 的本地 DRAM 副本; 因为 CoW, 这副本被 invalidate 后下次 cache miss 走 register-then-fill path 自然指向新 block.

### I5/I6 修订理由 (post-Q1 review)

iter-4A 早期 spec 草稿有过 "in-place update preferred + 2-layer directory" 设计. 经回顾 FUSEE 原版 + Reviewer 角度 attack analysis 后, 此设计**被否决**:

- **FUSEE 原版选 CoW**: `Client::kv_insert/update/delete` 都是 allocate-new-block + RDMA-CAS slot pointer (`src/client.cc`). 跟 LSM, BwTree, Bigtable, 大多数 OLTP DB 的 MVCC 一致.
- **In-place 在 reviewer 角度有 6 个 attack vector** (crash consistency, concurrent reader, variable-length, fragmentation, 两套 code path, why differ from FUSEE original). CoW 全免疫.
- **CXL clflushopt + sfence 不为 > 8B value 提供原子性**, in-place > 8B 在 partial-write race 下 broken.
- **CoW 让 KV blocks immutable** → reader 通过 pointer indirection 看到 atomic snapshot, **不需要 KV-level directory tracking** → directory 简化为单层 (per-bucket).

Spillover hybrid (small inline + large external pointer) 是 classic 设计 (ext4 inode, Redis embstr, Memcached slab, PostgreSQL TOAST, RocksDB BlobDB) 但带来 "两套 path = 两套 correctness reasoning" 的复杂度, **iter-4A 不采用**. CoW unique path 的 simplicity 胜过节省的 latency.

### IV — Read path

```
read(key):
  shard = sharding_table[hash(key)]   # H 路 array
  
  # Fast path
  if entry = cache_pool.lookup(key) and not entry.stale:
    return entry.value                # ~50 ns 全本地, NO CXL access
  
  # Slow path - cache miss or stale
  if shard.owner == self_host:
    # 同 host 走本地路径
    register_with_local_directory(key, sharer = self_host)
    flush_line + load (bucket from CXL) + chase pointer + load (KV block)
  else:
    # 跨 host - 通过 N:1:1:N 拿 KV value 同时 register
    send cache_register_request(key) to shard.owner via N:1:1:N
    wait for response: ACK + value bytes
    # response 隐含 register 已完成
  
  cache_pool.insert(key, value, stale=false)
  return value
```

**Reader 永远不查 directory**。reader 唯一与 directory 交互的方式是发 register/evict 请求。

### V — Write path

```
write(key, new_value):
  shard = sharding_table[hash(key)]
  
  if shard.owner == self_host:
    execute_write_locally(key, new_value)
  else:
    forward_op_to(shard.owner, OP_WRITE, key, new_value)
    wait_for_response()
    return response.status

execute_write_locally(key, new_value):
  # 假设我是 owner host 上的 worker
  bucket_idx = hash(key) % num_buckets
  
  # 1. 拿 bucket directory 互斥 (本机内, host-local spinlock)
  acquire(BucketDirectory[bucket_idx].spinlock)
  
  # 2. 决定 op 类型 - 通过 cache 或 CXL 读 bucket 看 slot 状态
  bucket = bucket_cache.lookup_or_fetch(bucket_idx)
  slot_idx = find_slot(bucket, key)
  
  # 3. CoW: 一律 allocate new KV block (in-place 不用)
  new_block_addr = kv_blockpool.alloc(size_class(new_value))
  write_value_to_block(new_block_addr, new_value)
  flush_line + sfence (durable on CXL)
  
  # 4. invalidate sharers via N:1:1:N (sync wait ACK)
  sharers = BucketDirectory[bucket_idx].sharer_bitmap \\ {self_host}
  if sharers != empty:
    send_invalidate(bucket_idx, target_hosts=sharers)
    wait_for_all_acks()                         # ★ strict-A commit barrier
  
  # 5. atomic CAS slot.pointer to new_block_addr (★ commit point)
  CAS(bucket.slots[slot_idx].pointer, old_addr, new_block_addr)
  flush_line + sfence
  
  # 6. 更新 directory state
  BucketDirectory[bucket_idx].sharer_bitmap = {self_host}
  BucketDirectory[bucket_idx].state = M
  BucketDirectory[bucket_idx].version += 1
  release(BucketDirectory[bucket_idx].spinlock)
  
  # 7. 更新本地 cache (本 host 自己的 cache 也要更新)
  cache_pool.update(key, new_value, stale=false)
  
  # 8. 老 block 进 lazy GC (异步 fiber 收集, 等所有 reader 不再 reference 后释放)
  gc_queue.push(old_block_addr)
  
  return SUCCESS
```

**INSERT/DELETE 几乎相同**, 区别仅在 step 2-3:
- INSERT: 找空 slot (slot.key=empty), step 3 alloc new block, step 5 同时写 slot.key + slot.pointer (单 cacheline 内, atomic 16B 通过 SSE2 store)
- DELETE: 找 matching slot, step 3 跳过 (无 new block), step 5 atomic store slot.key=empty (8B atomic), 老 block 直接 GC

注意: INSERT 找空 slot + DELETE 改 slot.key=empty 改的是 **bucket layout**. 跨 host CAS 在同 slot 上有 ABA risk → 这种情况仍需 CXL LFM lock 跨 host 互斥 (与 UPDATE 不同). I2 的 sharding rule 把这种互斥也消除了 — owner host 内部 single host atomic CAS 即可，跨 host 不存在 INSERT/DELETE 同 bucket 竞争.

### VI — Synchronization primitives 使用规则

| 数据结构 | 物理位置 | 同步原语 | 理由 |
|---|---|---|---|
| BucketLockTable (CXL 上) | CXL | LFM (`shm_mutex_t`) | **post-sharding 几乎不用** — 仅在 sharding 表初始化失败 fallback 时 (degraded mode) 跨 host 互斥. iter-4A 正常路径不取 |
| BucketDirectory (DRAM, MAP_SHARED) | DRAM | `pthread_spinlock_t` PROCESS_SHARED | 只本 host worker 间互斥 — owner host 上多 worker 同时改 sharers |
| Cache pool entry | DRAM | per-bucket spinlock OR lock-free hashmap | hot path, 不能 contention |
| SPSC ring head/tail | CXL | std::atomic + clflushopt | SP/SC, 单 producer 单 consumer 不用锁, 仅用 atomic exchange 推进 |
| KvBlockpool free list | CXL | LFM (`shm_mutex_t`) | 跨 host 分配, 罕见 op |
| LocalSelfFlags | DRAM | per-flag std::atomic | reader 自己 set/clear, 跨 worker 共享 |

**绝对禁止**: 拿 LFM 保护 DRAM-only 数据结构 (会引入跨主机 cacheline 流量给本来不跨主机的字段, performance bug)。

### VII — Anti-patterns（设计错例 list）

下面这些是已经犯过或者很容易犯的错, 写在这里作为 review checklist:

1. **AP1: 把 hash bucket array 当 host-local authoritative 副本** — 违反 I1。CXL 是唯一 authoritative。
2. **AP2: 用 cross-host LFM 保护 same-key writer-writer 互斥** — 违反 I2。Sharding 后 writer 都在 owner host, 用 host-local lock 即可。
3. **AP3: per-worker cache pool (MAP_PRIVATE)** — 违反 I3。同 host 共享 cache pool, directory 维度跟 host 数。
4. **AP4: invalidation 路径上物理 free entry 内存** — 违反 I4。Lazy stale flag。
5. **AP5: 用 in-place value update 替代 CoW** — 违反 I6。In-place 在 CXL persistent memory + > 8B value 下没有原子性保证, 且引入 reviewer attack vectors (crash consistency, concurrent reader semantics, 两套 path).
6. **AP6: 把 directory 放 CXL** — 违反 I7. Directory home in DRAM, no CXL replica.
7. **AP7: 用 LFM 保护 directory** — 违反 I8. host-local atomic enough.
8. **AP8: reader 直接读 directory.sharers 决定读哪份 cache** — 违反 I9. reader self-tracks, 不查 directory.
9. **AP9: writer publish CXL bytes 在 invalidate ACK 完成前** — 违反 I10. Strict-A 语义破坏.
10. **AP10: writer 用 cross-host LFM 而非 forward** — 违反 I11. Sharding 的核心目的就是消除 cross-host writer 锁.
11. **AP11: iter-4A 写 OpLog 但 reader/recovery 不读** — 违反 I12. iter-4A OpLog 完全不动.
12. **AP12: 仍然支持 inline u64 KV 路径** — 不是不变式但是是 anti-pattern。新 A v2 必须切到 KvBlockpool variable-size path; inline u64 path 仅在 protocol C baseline 里保留。
13. **AP13: 把 phys_hosts_pr_, my_phys_host_pr_ 等 routing field 留 default value** — Finding-1 的失败模式。attach() 必须从 FUSEE_NUM_HOSTS 等 env 显式赋值, **没有合法的 default fallback**。
14. **AP14: silently 把 sharer set 退化成 broadcast** — directory entry 不能省。如果 directory 因为某些原因不 available（e.g. crash 后），必须显式 broadcast all hosts (recovery 路径)，不能让某次 op silently broadcast。
15. **AP15: cache fill 在 register ACK 之前完成** — 违反 I9 race-free invariant.

### VIII — Protocol comparison

| 维度 | RDMA-FUSEE 原版 | Protocol C (baseline, frozen) | Protocol A iter-3A (legacy, no-op N:1:1:N) | **Protocol A v2 (iter-4A target)** |
|---|---|---|---|---|
| Authoritative copy | each server's local DRAM (multi-replica) | CXL bucket array | CXL bucket array | **CXL bucket array** |
| DRAM role | client-side index cache + server-side primary | reader DRAM cache (whichever recently accessed) | reader DRAM cache | **DRAM = cache only**, MAP_SHARED across same-host workers |
| Writer-writer 互斥 | RDMA CAS (硬件原子) | per-bucket LFM | per-slot LFM | **host-local spinlock (post-sharding)**; LFM 仅用于 INSERT/DELETE bucket layout 修改 |
| Reader 一致性机制 | 多副本, primary 是真相 | seqlock on `write_epoch` | cache_epoch_ + cache_epoch_arr_ (no-op'd in 3A) | **directory-based MESI**, lazy stale flag, no read-side epoch check |
| Cross-host invalidation | RDMA WRITE to backup | none (lazy, reader self-detects) | designed N:1:1:N (no-op 3A) | **N:1:1:N 主动 push, sync ACK before write commit** |
| Stale read window | 0 (multi-replica fan-out is sync) | unbounded (until next read) | depends on intent | **0** (writer waits for all sharer ACKs) |
| Write throughput limiter | RDMA CAS rate × num_replicas | per-bucket LFM serialization | (no-op) | host-local atomic + N:1:1:N forwarding throughput |
| Fault tolerance | strong (multi-replica) | none | none | **none in iter-4A**; future via CXL backup tree |

### IX — Validation gates (任何 iter 必须验证)

iter-4A 任何 sweep / benchmark 必须报告:

1. **Hash-diff battery (cross-host correctness)**: 至少 5 reps × T={2,4,8,16} × workload A 100K UPDATE = 20 runs，cmp on bucket+KV bytes, **必须 100% PASS**。失败 = strict-A 不成立 = 必须 STOP 排查。
2. **Multi-rep stability**: 任何 headline Mops/s 数字必须有 ≥ 5 reps 中位数 + σ ≤ 中位数 5%。单 rep 数字不接受为 evidence (iter-3A sweep2 35.13 outlier 教训)。
3. **N:1:1:N 真实激活验证**: dispatch_and_wait 里的 cross-host enqueue loop 必须有 non-zero iteration count (以 sweep summary 报告, e.g. "all cells ran with `phys_hosts_pr_=2`, enqueue loop executed N times"); 否则 = AP13 的 silent failure.
4. **Directory hit rate**: invalidation 流量 = ops × avg_sharers_per_key, 必须报告 sharers 分布 (mean, p99); broadcast (sharers==H) 比例 < 5% (否则 directory 不起作用了, 等同 broadcast).
5. **Forward routing measured**: cross-host write 比例 + forward latency p50/p99; same-host write 比例 + forward overhead 0 (sanity check).


### X — Enforcement mechanisms (如何让 spec 真正成为约束)

iter-3A 的教训表明 spec 写出来不等于 spec 被遵守 — Finding-1 的 routing
field default value 整整两个 iter 没人发现. 为防止再次出现 silent design
drift, iter-4A 之后启用以下 5 层 enforcement, 从软到硬:

#### E1 — Soft: cross-reference from CLAUDE.md (auto-load every session)

`CLAUDE.md` 加一节 "Protocol A v2 spec compliance":

> Any change to `src/cxl_kv_ops_A*.{h,cc}`, `src/cxl_directory*`, or
> `src/cxl_sharding*` files MUST first read `docs/design_goals.md
> §Protocol A v2` in full. Commit messages for those files must
> reference invariant numbers (I1-I12) or anti-pattern numbers
> (AP1-AP15) that the change relates to.

效果: future Claude session 自动加载 CLAUDE.md → 看到这条 → 主动读 spec.

#### E2 — Code-level static / runtime enforcement

把 invariant 写成 build-time `static_assert` 或 attach-time runtime
abort:

| Invariant / AP | Enforcement code location |
|---|---|
| I3 (cache pool MAP_SHARED) | `cxl_cache_pool.cc::init` 内 `runtime_check(mmap_flags & MAP_SHARED)` else `abort` |
| I8 (host-local lock for directory) | `BucketDirectoryEntry::spinlock` typedef = `host_local_spinlock_t`; 跟 `shm_mutex_t` 是不同 type 编译期就拒绝 |
| I12 (no OpLog write in iter-4A) | `OpLog::begin/commit` 加 `[[deprecated("Forbidden in iter-4A per design_goals.md I12")]]` 或运行时 abort |
| AP13 (routing field defaults) | `attach()` 末尾 `if (per_host_rings_enabled_ && phys_hosts_pr_ == 1 && atoi(getenv("FUSEE_NUM_HOSTS"))!=1) abort("AP13 detected")` |
| AP14 (silent broadcast) | `invalidate_sharers()` 内 `if (sharers_count == H && !explicit_broadcast_flag) abort("AP14 silent broadcast")` |
| AP15 (cache fill before register ACK) | reader cache fill API 加 require `register_acked` flag, false 时 abort |

效果: 不读 spec 也写错代码, 进程在 attach 阶段 crash, build 阶段 fail.

#### E3 — Test-level enforcement (CI gate)

新增 `tests/protocol_a_v2_invariant_check.cc`. 每个 PR 改 protocol A
代码必须通过:

- **I1 check**: `cxl_region size > local_dram_kv_size` (CXL 必须能放下完整数据)
- **I2 check**: 注入跨 host write op, 检查 forward path counter 增加
- **I3 check**: `procfs /proc/self/maps` 验证 cache pool 有 `s` (shared) flag
- **I9 race test**: stress test reader cache fill, 验证 register ACK 必在 cache insert 前 (用 helgrind / TSan)
- **I10 race test**: writer commit 后立即从 peer host 读, 必须看到新 value (strict-A 实测 fixture)
- **AP13 check**: 启动后立即 SIGABRT 测试, 在 phys_hosts_pr_==1 但 NUM_HOSTS!=1 时

CI 失败 = PR 不允许 merge.

#### E4 — Sweep validation gates (per §IX, 强化版)

iter-4A 之后任何 scaling_ycsb sweep 的 `SUMMARY.log` 顶部必须包含 5 行
"validation gates passed":

```
# Validation gates per design_goals.md §Protocol A v2:
# G1 hash-diff: 5 reps × T={2,4,8,16} × workloadA 100K UPDATE = 20/20 PASS
# G2 multi-rep stability: σ ≤ median × 5% on every headline cell  
# G3 N:1:1:N actual activation: avg dispatch_loop_iter_per_op = X.X (must > 0)
# G4 directory hit rate: avg sharers per write = X.X (broadcast bound = H = N)
# G5 forward routing: cross-host write fraction = X%, forward p50/p99 = X/X µs
```

任何 gate 缺失 / fail = sweep 结果 invalid, 必须重跑. 这一条直接防御
iter-3A "silent no-op N:1:1:N 路径未被发现" 的失败模式.

#### E5 — Spec-change 流程

**任何对本节 (`docs/design_goals.md §Protocol A v2`) 的修改必须由 user
explicit approve.** Claude 在 PR 里看到要改 spec invariant/AP 时:

1. STOP, 不要先动代码
2. 输出 "我建议改 invariant Iₓ 为 Y, 因为 …" 给 user
3. 等 user 明确 confirm
4. 才能修改 spec + 实现

**绝对禁止 silent spec drift** — 即"先写代码 + 再改 spec 让实现合规".
这种模式在 iter-3A 实际发生过 (`cxl_architecture_plan.md` 的 single-copy
设计悄悄越界, 没人发现是违反 progress.md:410). 必须靠流程拦住.

### XI — PR review checklist (任何 protocol A 改动 PR 描述里 paste)

```
## Protocol A v2 spec compliance (design_goals.md §Protocol A v2)

For each invariant, check ✓ if change is consistent, ✗ if change
violates (and explain why violation is acceptable / spec needs update).

- [ ] I1 (CXL = authoritative; DRAM = cache only)
- [ ] I2 (Sharding: writer on owner_host)
- [ ] I3 (Same-host MAP_SHARED cache, sharers tracked at host level)
- [ ] I4 (Lazy stale flag, not physical delete on invalidate)
- [ ] I5 (Single-level directory: per-bucket only)
- [ ] I6 (Copy-on-write, not in-place update)
- [ ] I7 (Directory home in DRAM only, no CXL replica)
- [ ] I8 (Directory lock = host-local, NOT LFM)
- [ ] I9 (Reader fast path = local cache + stale check; slow path = register-then-fill)
- [ ] I10 (Write commit point = after all sharer ACK + CXL durable)
- [ ] I11 (Cross-host write via N:1:1:N forward, not LFM)
- [ ] I12 (No OpLog write in iter-4A)

For each anti-pattern, check ✓ if NOT triggered:

- [ ] AP1-AP15 (review each — spec §VII for full list)

Validation gates run on this branch:
- [ ] G1 hash-diff battery: __ / 20 PASS
- [ ] G2 stability check: σ/median = __%
- [ ] G3 N:1:1:N activation: avg loop_iter = __
- [ ] G4 directory hit rate: __ avg sharers
- [ ] G5 forward routing: __% cross-host

If any invariant violated or gate failed, paste user approval (link to
chat / commit) showing user explicitly approved the deviation.
```

### XII — Outstanding open questions (待 user confirm)

下面这几条 spec 草稿期间留的 default, 需要 user 明确 confirm 或 override
后才能 unblock iter-4A Phase 1 implementation:

| # | 问题 | 当前 default |
|---|---|---|
| O1 | Sharding hash function | `(hash(key) >> 31) & 1` (high bit of FNV-1a hash) |
| O2 | Directory entry size | 16 B per BucketDirectoryEntry, fields per §III |
| O3 | Forward op response | Option B — response carries value bytes for small values (≤ 48 B), Option A fallback for large. inline u64 / kv=256 都走 Option B |
| O4 | Crash recovery | Deferred to future iter (跟 replication 一起设计) |
| O5 | Size class allocator for KV blockpool | 用 cxl_kv_blockpool 现有 size class 划分; UPDATE 同 size class fast path (但仍 alloc new block 走 CoW), cross-class fall back 同 path |

---

## 元规则: 这份 spec 跟 CLAUDE.md / progress.md 的关系

`docs/fusee_cxl_progress.md:410` 的 "preserve original FUSEE design as
much as possible; only replace RDMA transport with CXL" **优先级最高** —
它是 user 在 2026-04-20 立的 project-level 约束. iter-3A 后我们识别出
当前协议 A 已偏离这条约束 (multi-replica → single CXL copy), 但 iter-4A
v2 设计出于 (i) reviewer-attack-免疫, (ii) sharding-based writer 互斥
消除等 architectural 收益, **保持 single-CXL-copy 模型** + 用 directory
+ MAP_SHARED cache 提供"DRAM 多副本 cache"的实际效果, 在功能等价层面
逼近 multi-replica.

如果 user 后续判断这种"功能等价"不够 — 仍坚持要回到 multi-DRAM-replica +
RDMA-style fan-out — 那 iter-4A 整个设计要重新评估, 可能需要回退到 sub
集合（先做 sharding 不做 directory cache, 类似 RDMA-FUSEE 的 client-side
index cache）. 此选择应 user-driven, 不应 implementation-driven.
