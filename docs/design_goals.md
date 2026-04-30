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
| I5 | **Single-level per-slot directory**. Each (bucket_idx, slot_idx) pair has one DirectoryEntry. KV value blocks are **immutable** (CoW: UPDATE allocates new block, slot.pointer CAS is the commit point) → no separate KV-level directory needed. Per-slot granularity (vs per-bucket) avoids false sharing — INSERT on slot[3] does not invalidate sharers caching slot[5]. Storage: 65k × 7 × 16B = 7.3 MB / host. | 偏差例: per-bucket directory → 一个 op 让 cache bucket 内所有 slot 的 sharers 全 invalidate (~7× false sharing on 7-slot bucket); 偏差例: KV blocks mutable + in-place update → 立即引发 reviewer attack 链 (见 "I5/I6 修订理由"). |
| I6 | **All UPDATE paths use Copy-on-Write (allocate-new-block)**, NOT in-place. Same-size and size-changing UPDATE go through identical path: alloc new KV block from blockpool → write value bytes to new block → `flush + sfence` → atomic CAS `slot.pointer` → old block enters lazy GC. This matches FUSEE original design and standard practice for persistent KV stores. | 偏差例: in-place update on shared CXL → > 8B value 缺乏原子性 → partial-write race; reviewer attack #1. 见 "I5/I6 修订理由". |
| I7 | **Directory home = owner host DRAM only.** No CXL replica, no cross-host directory query path. Reader does not query directory; reader self-tracks "I'm in sharers" via local flag set during `cache_register` ACK. | 偏差例: directory 放 CXL → 每次 directory update 付 1-2 µs CXL store, hot directory entry 在 hot bucket 上的 cacheline pingpong. |
| I8 | **Directory updates use host-local atomics or spinlock on shared mem (PROCESS_SHARED), NOT LFM.** LFM is reserved for genuinely cross-host CXL state. | 偏差例: 拿 LFM 保护本机 DRAM directory → 把 cross-host coordination 工具用在不需要的地方, 引入跨主机 cacheline 流量给本来不跨主机的字段. |
| I9 | **Read fast path = local cache lookup + stale check (no `acquire-load` on shared atomic, no CXL load).** Read slow path on cache miss = register with directory (mandatory N:1:1:N message + ACK) **before** filling cache. | 偏差例: reader 先 fill cache 后 register (race window: writer 已经 invalidate 完所有当前 sharers 但 reader 还没在 sharers 里 → reader 永远不被通知 stale). |
| I10 | **Write commit point = "in-place CXL write completes after all sharer ACK received".** Writer does NOT return success until: (a) directory.sharers \\ {self} 全部 ACK invalidate done, (b) CXL write durable (clflushopt + sfence completed). 这是 strict-A linearizability 的来源. | 偏差例: writer 在 invalidate-ACK 完成前先 publish CXL 字节 → 对端 reader 可能在 ACK-before window 读到新值, 但本端 cache 还有 stale → 跨 host 不一致. |
| I11 | **Cross-host write forwarding via N:1:1:N message (SPSC ring + sender/receiver thread)**, not via cross-host LFM lock. Forward message carries (op_type, key, **CXL pointer to value bytes in forward staging buffer**, value_size); forwarder pre-writes value bytes into its host-owned forward staging buffer on CXL; owner reads via clflushopt+load. Response messages carry only (status, optional slot.pointer for cache_register), **never inline value bytes**. | 偏差例: 用 cross-host LFM lock 互斥 writer → 见 I2. 偏差例: response 携带 inline value → 增加一套 code path 但 KV ≥ 256 B fits 不下, 不值. |
| I12 | **No OpLog write/read in iter-4A.** Crash recovery uses broadcast `everyone_invalidate` + 全 client cache 清空 + on-demand re-fill from CXL. OpLog 框架代码保留, 等 future replication 设计时启用. | 偏差例: iter-4A 写 OpLog 但 reader 不读 → 浪费 CXL 流量 + 误以为有 fault tolerance. |

### II — CXL 物理布局（authoritative state）

CXL `/dev/dax0.0` mmap'd region 包含:

```
[0]              GlobalHeader (4 KB)
[4 KB]           BucketLockTable[num_buckets]    ← LFM mutex per bucket; iter-4A unused (sharding); reserved for protocol C
[align 64]       CxlKvBucket[num_buckets]        ← (fp, len, owner_node_id, pointer) per slot, NO inline value bytes
[align 64]       PerHostSpscRing[H][H][K]        ← N:1:1:N 通讯介质 (invalidate + forward + register/evict + ACK)
[align 64]       AckChannel[H][H][K]
[align 64]       KvBlockpool[H]                  ← variable-length KV blocks, size-classed; partitioned per owner host
[align 64]       ForwardStaging[H]               ← per-forwarder-host staging area for OP_WRITE_FORWARD payloads (value bytes ≥ 256 B); short-lived, freed on forward ACK
[align 64]       (OpLog area — reserved, not written by iter-4A)
```

**CXL 上不再有 inline u64 KV 路径**。所有 value bytes 在 KvBlockpool 里, slot 仅含 pointer.

**ForwardStaging[H]** 物理布局: 每个 forwarder host 独占一段 (e.g. 1 MB / host), 用作 OP_WRITE_FORWARD value 的 cross-host transfer buffer. forwarder host 写 staging slot, owner host 读. 生命周期 = 1 forward roundtrip (forwarder 收到 forward-response ACK 后 free staging slot). 跟 KvBlockpool 隔离 (KvBlockpool 是 owner-host owned; ForwardStaging 是 forwarder-host owned).

### III — DRAM 物理布局 (per host, MAP_SHARED across same-host workers)

```
[0]              ShardingTable                                 ← const, attach() 时初始化
[align 64]       SlotDirectory[num_buckets][slots_per_bucket]   ← per-slot sharers + state (唯一 directory layer)
[align 64]       KvCachePool                                    ← (key → cached value bytes) hashmap, lazy stale flag, LRU evict
[align 64]       LocalSelfFlags                                 ← per-slot "我在 sharers 里" flag (reader self-tracked)
```

**Directory entry 大小** (only one type, per-slot):

```c
struct SlotDirectoryEntry {        // 16 B total
  uint8_t   state;                 // 1 B  : I=0, S=1, M=2 (transient)
  uint8_t   sharer_bitmap;         // 1 B  : 8 hosts max (g34 H=2 uses 2 bits)
  uint8_t   spinlock;              // 1 B  : host-local PROCESS_SHARED spinlock
  uint8_t   pad1;                  // 1 B
  uint32_t  version;               // 4 B  : ABA防护 + observability
  uint64_t  pad2;                  // 8 B  : alignment + future fields
};
```

总大小: num_buckets × slots_per_bucket × 16 B = 65k × 7 × 16 B = **7.3 MB / host** @ 65k buckets, 7 slots/bucket. 仍然小.

**为什么 per-slot 而不是 per-bucket**: per-bucket directory 让 INSERT/UPDATE/DELETE 一个 slot 的 op 让 cache bucket 内所有 slot 的 sharers 全 invalidate (false sharing). YCSB 实际 op 几乎都是单 slot 操作，per-slot 削掉 ~7× 的 false invalidation traffic. 存储 cost 7×, 仍 < 10 MB / host.

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
  shard = sharding_table[hash(key)]                       # plain load (DRAM, const after init)
  
  # Fast path (~50 ns total, NO CXL access, NO fence, NO flush)
  entry = cache_pool.lookup(key)                          # plain load on hashmap node (DRAM)
  if entry && !entry.stale:                               # plain load 1 byte (entry 同 cacheline, L1 hot)
    return entry.value                                    # plain memcpy from DRAM
  
  # Slow path - cache miss or stale
  # 设计简化: response 永远不携带 value bytes; 只携带 (status, slot.pointer).
  # 不再有 inline-value-in-response path (省一套 code path; KV ≥ 256 B 也无法 inline).
  # 不论 owner=self 还是 owner=peer, reader 自己 fetch value bytes from CXL.
  
  if shard.owner == self_host:
    # 同 host: 本机 directory 注册 (DRAM hardware coherent, 无 message)
    register_with_local_directory(key, sharer = self_host)
    cxl_slot_pointer = lookup_slot_via_cxl(bucket_idx, key)  # clflushopt+mfence+load bucket
  else:
    # 跨 host: 通过 N:1:1:N 让 owner host register 我, 回送 slot.pointer
    send_cache_register_request(key) to shard.owner       # via DRAM aggregator queue + sender thread
    wait for response                                     # spin on local ack_buf, atomic load (acquire) on DRAM
    cxl_slot_pointer = response.slot_pointer              # plain load from DRAM ack_buf (no value bytes here)
  
  # Fetch value bytes from CXL (always, regardless of owner)
  for off in 0 .. value_size by 64:
    flush_line(cxl_slot_pointer + off)                    # clflushopt (CXL, evict L3 stale)
  full_fence()                                            # mfence
  memcpy(value, cxl_slot_pointer, value_size)             # plain memcpy from CXL (now coherent)
  
  cache_pool.insert(key, value, stale=false)              # DRAM stores (atomic insert)
  set_local_self_flag(key, in_sharers=true)               # atomic byte store (DRAM, release)
  return value
```

**Reader 永远不查 directory**。reader 唯一与 directory 交互的方式是发 register/evict 请求 (走 N:1:1:N message)。

具体 primitive 见 §VI-B "Reader fast path / Reader slow path" subsections.

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
| ~~BucketLockTable (CXL 上)~~ | ~~CXL~~ | **NOT USED in Protocol A v2** | sharding 让所有同-bucket writer 落在 owner host, host-local spinlock 即可. 该字段在 CXL region 仍 mmap 但 protocol A v2 normal path 不 acquire. 仅 protocol C baseline 使用 LFM. |
| SlotDirectory (DRAM, MAP_SHARED) | DRAM | `pthread_spinlock_t` PROCESS_SHARED, OR atomic CAS on `state` byte | host-local — owner host 上多 worker 同时改 sharers; hardware coherence 保证, **不需要 LFM** |
| Cache pool entry (DRAM, MAP_SHARED) | DRAM | per-entry byte spinlock OR lock-free hashmap | hot path; hardware coherence; **不需要 LFM** |
| SPSC ring head/tail (CXL) | CXL | `std::atomic<u64>` + `clflushopt` + `sfence` | SP/SC, 单 producer 单 consumer; producer fetch_add tail; consumer plain load head; 没有 MP 争抢, **不需要 lock 不需要 LFM** |
| LocalAggregatorRegion (DRAM, MAP_SHARED, MPSC) | DRAM | atomic `tail.fetch_add` + acquire/release | host-local MPSC; producer 是 same-host worker; hardware coherent; **不需要 LFM** |
| KvBlockpool free list | DRAM (per-host metadata) + CXL (block bytes) | host-local spinlock on per-host free list metadata | per-owner-host partition: 每 host 自己一段 blockpool, 自己维护 free list (DRAM); 跨 host writer 不存在 (sharding); **不需要 LFM** |
| LocalSelfFlags (DRAM, MAP_SHARED) | DRAM | per-flag `std::atomic<bool>` | reader 自己 set/clear, host-local hardware coherent |

**Protocol A v2 LFM usage summary**: **iter-4A normal path 不使用 LFM**.
LFM 仍保留在 codebase, 但仅服务于 (a) Protocol C baseline (frozen),
(b) Protocol A legacy v1 (frozen), (c) 假设的"sharding 失败 degraded
mode" — iter-4A 选择 fail-fast (sharding init 失败直接 abort) 而不是
degrade, 所以 (c) 在正常 boot 永不进入. **绝对禁止**: 拿 LFM 保护
DRAM-only 数据结构 (引入跨主机 cacheline 流量给本来不跨主机的字段,
performance bug).

### VI-A — Memory operation primitive reference (must-cite when implementing)

CXL Type-3 在 g3+g4 没有跨主机 cache coherence (`docs/refs/sync_workflow.md`).
任何 "host A 的写需要让 host B 看到" 的字节都必须 explicit 走
"clflushopt 让 host A 的 dirty line 推到 CXL backing → sfence 排序 →
host B 的 clflushopt + mfence 让自己 stale L3 副本失效 → 重 load".
本节列各类 primitive 的精确语义.

#### Stores

| Primitive | Encoding | 语义 | 用 CXL 还是 DRAM? | 必须配什么 fence? |
|---|---|---|---|---|
| **Plain store** (`*p = v` / `mov`) | scalar mov | 写入 cacheline; 留在 L1 dirty 直到 evict 或 flush | DRAM ✓; CXL ✗ (host B 看不到) | 单独不需要; 跟 clflushopt 配 sfence |
| **Atomic store** (`std::atomic::store(release)`) | x86 上仍是 `mov`, but compiler enforces ordering | 跟 plain store 同一 instruction; release semantics 由 compiler 保证不 reorder; **x86 aligned ≤ 8B store hardware-atomic** | DRAM hardware coherent; CXL 跨 host 不保证 atomic visibility | release semantics 自动包含 store ordering; 跨 CXL 仍需 clflushopt + sfence |
| **Non-temporal store** (`_mm_stream_si64`, `_mm512_stream_si512`) | `movnti`/`movntdqa` family | bypass cache; 直写 WC buffer → memory; 不污染 L1/L2/L3 | **优选 for large CXL writes** (≥ 256 B); 避免 L3 cache 污染 | **必须 sfence** 才 visible to subsequent loads/stores |
| **CAS** (`__atomic_compare_exchange`) | `lock cmpxchgq` (8B) / `lock cmpxchg16b` (16B) | atomic compare-and-swap; **同 host 内 hardware atomic** | DRAM ✓; CXL **跨 host 不保证 atomicity** (no global coherence domain) | 自带 full fence semantics; CXL 上后续仍需 clflushopt 让对端看到 |

**CAS on CXL 跨 host 警示**: x86 `lock cmpxchgq` 在 single host 内是 atomic, 但跨 g3+g4 在同一 CXL 字节上同时 CAS **不保证有 winner** (Type-3 无 atomic broker). Protocol A v2 sharding 保证只有 owner host 在 CXL slot pointer 上做 CAS, 单 host 内 hardware atomic 足够; 跨 host 永不并发 CAS 同 slot.

#### Loads

| Primitive | Encoding | 语义 | 用 CXL 还是 DRAM? |
|---|---|---|---|
| **Plain load** (`v = *p` / `mov`) | scalar mov | 从 L1/L2/L3 或 memory 读; 可能命中 stale 副本 | DRAM ✓; CXL **可能读到本机 L3 上的 stale 副本** (因 host B 不知道 host A 改了 CXL backing) |
| **Atomic load** (`std::atomic::load(acquire)`) | x86 仍是 `mov` | acquire semantics by compiler; aligned ≤ 8B hardware-atomic | DRAM ✓ hardware coherent; CXL 仍可能 stale (与 plain load 同) |
| **CXL coherent load** (CACHELINE_LOAD pattern) | `clflushopt(p); mfence; v = *p;` | 显式 invalidate 本机 cacheline → 重新走 CXL fabric load → 看到 host A 最新 push 的 byte | **CXL** 跨 host visibility 唯一可靠方式 |

#### Cache management

| Primitive | Encoding | 语义 | 跟其他对比 |
|---|---|---|---|
| **`clflush`** | `clflush` | invalidate cacheline + write-back if dirty + flush from all CPU caches; **strongly ordered** (each clflush serializes) | 慢, 因为 strong ordering. 仅 legacy 兼容用 |
| **`clflushopt`** | `clflushopt` | 同 clflush 但 **weakly ordered** (多个 clflushopt 可并行); 必须 sfence 完成 ordering | **首选 for CXL flush**, ~5–10× faster than clflush |
| **`clwb`** | `clwb` | write-back dirty bytes to memory **but keep cacheline in cache** (E or S state); 比 clflushopt 更友好 (不丢 locality) | iter-4A **不用** — 我们 explicitly want stale L3 副本被淘汰 让对端 reader 重 load. clwb 留 stale 副本反而让对端 host 的 clflushopt+load 产生不必要的 backing-store 跑 read |
| **`wbinvd`** | `wbinvd` | write-back + invalidate **整个** CPU cache 系统; kernel-only | iter-4A 不用 |

**iter-4A primitive 选择: clflushopt (仅), 不用 clflush 也不用 clwb**.
理由: clflushopt 弱序更快, 跨 host 共享场景下 stale L3 副本必须淘汰
所以 clwb (保留 cacheline) 反而坏.

#### Fences

| Primitive | Encoding | 语义 |
|---|---|---|
| **`sfence`** | `sfence` | store fence; 之前的 stores 在之后的 stores 之前 globally visible. **跟 clflushopt 配套**, 让 clflushopt 的 evict-to-CXL 顺序确定 |
| **`lfence`** | `lfence` | load fence; 之前的 loads 完成才 issue 之后的 loads. 在 CACHELINE_LOAD 里跟 clflushopt 配 (但实际用 mfence 更稳) |
| **`mfence`** | `mfence` | full fence (store+load); 用在 CACHELINE_LOAD 里保证 clflushopt 完成才 issue load |
| **compiler barrier** (`asm volatile("":::"memory")`) | none | 编译器层面禁止 reorder; 不发任何 CPU 指令 |

#### Atomicity 边界

| 操作 | x86 单 host atomicity | CXL 跨 host atomicity |
|---|---|---|
| 1 B / 2 B / 4 B / 8 B aligned plain store | ✓ atomic (TSO) | ✓ atomic (single byte) but 跨 host visibility 需 clflushopt+sfence |
| 16 B aligned SSE2 `movdqa` | ✓ atomic | ✓ atomic 但同上 |
| 32 B / 64 B AVX-512 `vmovdqa64` | **不保证 atomic** (multiple internal ops) | 不保证 |
| `lock cmpxchgq` (8 B CAS) | ✓ atomic (lock prefix) | **✗ 不保证 atomic** (无 cross-host atomic broker) |
| `lock cmpxchg16b` | ✓ atomic | ✗ 不保证 |
| Non-aligned access | ✗ 不保证 | ✗ 不保证 |

**iter-4A 的 atomic 边界使用**: 所有 CXL 上的 atomic store ≤ 8 B,
对齐, single-writer (sharding 保证). 16 B `movdqa` 在 INSERT 同时 publish
slot.key + slot.pointer 时是 fast path 但 **要求严格 16 B 对齐 + single
writer + reader 用同样 `movdqa` 读** 才 atomic; 否则 publish_slot pattern
(value 先, sfence, key 后) 是 safer 默认.

### VI-A.bis — Plain store + clflushopt + sfence vs NT store + sfence (谨慎选择)

两种方式都能让 host A 写的字节让 host B 看到, 但物理路径不同, **选错性能 1.5–3× 差**.

#### 物理路径对比

```
路径 1: plain store + clflushopt + sfence
─────────────────────────────────────────
mov v, [p]            # CPU L1 收到 dirty cacheline (写命中 L1)
clflushopt p          # 把 L1 dirty line evict → L2 → L3 → CXL backing
sfence                # 排序栅栏

特点:
- 数据**经过 L1/L2/L3** (cache 污染但保留副本)
- 之后 owner 自己再读 [p] 命中 L1 (~1 ns)
- 多个 clflushopt 之间 weak ordered, 可并行
- 单 cacheline 写效率高
- WC buffer 不参与
```

```
路径 2: NT store + sfence
─────────────────────────
movnt v, [p]          # bypass L1/L2/L3, 直接进 CPU 的 Write-Combining buffer
                      # WC buffer 满 (4-8 个 64 B) 或 sfence 时 flush 到 CXL
sfence                # drain WC buffer

特点:
- 数据**不进 cache** (cache 不污染)
- 之后 owner 再读 [p] = cache miss → 走 CXL fabric (~700 ns)
- 多 NT store 连续可充分利用 WC buffer (4-8 个 cacheline 攒一起 flush)
- 大量 streaming write 吞吐高 (适合 12.5 GB/s CXL ceiling)
- 单 cacheline NT 浪费 WC capacity
```

#### Decision rule (谨慎选择)

```
要写 CXL bytes? 走以下决策树:
│
├── 是 atomic-required publish (CAS / atomic store on flag/pointer)?
│   └── lock cmpxchg / atomic store + clflushopt + sfence
│       (NT store 不是 atomic, 不能用)
│
├── 数据 < 256 B (1-3 cachelines)?
│   └── plain store + clflushopt + sfence
│       (NT 不充分利用 WC buffer; clflushopt 单 cacheline 高效)
│
├── 数据 ≥ 256 B (4+ cachelines)?
│   ├── 写后 owner 自己马上要再读这片 CXL 字节?
│   │   └── plain store + clflushopt + sfence
│   │       (留 cache 副本, 避免 owner 自己 reload)
│   │
│   └── 写后 owner 不再访问这片 CXL 字节?
│       └── NT store + sfence
│           (不污染 cache, WC 攒满高吞吐)
│
└── streaming bulk write (KB+) 不打算保留 cache locality?
    └── NT store + sfence (definitely)
```

#### iter-4A 各场景的具体选择

| Op | 数据 size | 后续 owner 是否读这片 CXL? | 选哪个? |
|---|---|---|---|
| 写 CXL **slot.pointer** (INSERT/UPDATE/DELETE 的 commit point) | 8 B | atomic CAS required | `lock cmpxchgq` + clflushopt + sfence |
| 写 CXL **slot.key** (INSERT/DELETE) | 8 B | atomic store required | `__atomic_store_n` (release) + clflushopt + sfence |
| 写 CXL **slot 完整 16 B publish** (INSERT fast path) | 16 B | atomic 16B store | SSE2 `movdqa` + clflushopt + sfence |
| 写 CXL **KV block bytes (small, < 256 B)** | < 256 B | owner 在 `cache_pool` 已有副本 → CXL 上的 block 不再被 owner 访问 | **but** 边界 case: < 256 B 用 NT 浪费 WC; **plain store + clflushopt + sfence** |
| 写 CXL **KV block bytes (medium, 256–1024 B)** | 256–1024 B | 同上, owner 不再访问 | **NT store + sfence** (4-16 cachelines, WC 充分利用, 不污染 cache) |
| 写 CXL **KV block bytes (large, ≥ 1 KB)** | ≥ 1 KB | owner 不再访问 | **NT store + sfence** (definitely) |
| 写 CXL **SPSC ring entry** (PerHostInvalEntry) | 64 B (1 cacheline) | sender 单写, 自己不读 | plain store + clflushopt + sfence (单 cacheline NT 不划算) |
| 写 CXL **SPSC tail counter / AckChannel.seq** | 8 B | atomic store required | `__atomic_store_n` (release) + clflushopt + sfence |

**关键 reasoning**:
- "owner 后续是否再读这片 CXL"决定 NT 是否亏。Protocol A v2 下 owner 写 CXL KV block 后, **owner 自己的 reader 走 `cache_pool` (DRAM 副本)**, 不再访问 CXL 上的同一片 block bytes → **NT store 是赢**.
- 单 cacheline op (slot pointer, ring entry) NT 浪费 WC, 用 plain.
- atomic-required op 都不能用 NT (NT 不是 atomic).

#### 量化预期 (g34 实测推算)

| 路径 | 256 B 单次 | 1 KB 单次 | 4 KB streaming |
|---|---|---|---|
| plain store + clflushopt + sfence | ~150 ns (4 × clflushopt) | ~600 ns (16 × clflushopt) | ~3-5 µs |
| NT store + sfence | ~100 ns (1 × WC flush) | ~150 ns (WC 满 2 次 + sfence) | ~600 ns (WC 充分利用) |

**写 KV block 用 NT 在 ≥ 256 B 时节省 30-90%**. 这是 layer-2 12.5 GB/s ceiling 的关键 — 协议 C iter-5 sweep 在 kv1024 = 73% utilisation 就是因为用 NT store.

#### NT store 的 atomic 边界提醒

NT store 不是 atomic:
- `_mm_stream_si64` (8 B): 单 8 B 在 hardware 是 atomic, 但 release ordering 需要 sfence
- `_mm_stream_si128` (16 B): **不保证 atomic** — 可能拆成 2 × 8 B
- `_mm512_stream_si512` (64 B): 不保证 atomic — 可能拆成 8 × 8 B

所以 **NT store 用于"先写 large data + sfence + 用 atomic publish 让 reader 看到"** 模式:
1. NT store value bytes to new block (non-atomic, 但 reader 还看不到因为 slot.pointer 没改)
2. sfence (NT visible to subsequent ops)
3. `lock cmpxchgq` slot.pointer to new block addr (atomic publish)
4. clflushopt slot.pointer + sfence (让 peer host see)

reader 通过 (3) 的 atomic publish 才能"看到"新 block, 即使 (1) 的 NT store 内部 non-atomic 也无所谓 — reader 在 (3) 之前永远不读 new block.

#### Quick decision card (1-page reference for implementer)

```
═══════════════════════════════════════════════════════════════
  PICKING STORE PRIMITIVE FOR CXL WRITES (Protocol A v2)
═══════════════════════════════════════════════════════════════

Q1: 是 atomic-required publish (CAS / atomic flag store)?
    YES → lock cmpxchg / __atomic_store(release) + clflushopt + sfence
                                                                [STOP]

Q2: 数据 < 256 B?
    YES → memcpy / plain store + clflushopt(per cacheline) + sfence
                                                                [STOP]

Q3: 写完后 owner 自己在 CXL 同地址再读这片字节?
    YES → memcpy / plain store + clflushopt + sfence
    NO  → _mm512_stream_si512 (NT store) + sfence
        (iter-4A KV block ≥ 256 B 走 NT — owner 走 cache_pool DRAM
         不再访问 CXL 上这块 KV bytes)

═══════════════════════════════════════════════════════════════
  PICKING LOAD PRIMITIVE
═══════════════════════════════════════════════════════════════

Q1: 数据在 DRAM (host-local 或 MAP_SHARED 同 host)?
    YES → plain load (atomic load if shared atomic)             [STOP]

Q2: 数据在 CXL 且**对端 host 可能改过**?
    YES → clflushopt(p) + mfence + plain load
        (CACHELINE_LOAD pattern, ~700 ns/cacheline)

Q3: 数据在 CXL 但**只有 self host 在改**?
    可选 → 仍建议 clflushopt + mfence + load (defensive)
         OR plain load (if 你能证明 self host 是唯一 writer 且 self
            host 自己 cache 副本仍 fresh)
═══════════════════════════════════════════════════════════════
  FENCE 选择
═══════════════════════════════════════════════════════════════

| 上下文 | fence |
|---|---|
| 跟在 store / clflushopt / NT store 后, 让对端见 | sfence |
| 跟在 clflushopt 后, 自己 issue load 之前 | mfence (注 1) |
| 纯 read sequence ordering | lfence 通常不需要 (x86 强序) |
| compiler 不重排 | __asm__ volatile("":::"memory") |

注 1: 严格说 clflushopt 之后 lfence 即可让 invalidate 对自己后续 load
       生效, 但 mfence 更稳 (覆盖 store + load), 实测 latency 几乎一样.
       iter-4A 默认用 mfence after clflushopt before load.

═══════════════════════════════════════════════════════════════
  CACHE 选择
═══════════════════════════════════════════════════════════════

iter-4A 只用 clflushopt. 不用 clflush (太慢), 不用 clwb (留 stale 副本
反而让对端 host clflushopt+load 跑不必要的 backing-store read).

═══════════════════════════════════════════════════════════════
  MESSAGE PAYLOAD POLICY (N:1:1:N)
═══════════════════════════════════════════════════════════════

iter-4A 测试 KV size 在 {256, 512, 1024} B 范围. **No inline payload
anywhere** in messages. 单一 out-of-band path:

  OP_INVALIDATE / OP_CACHE_REGISTER / OP_CACHE_EVICT:
      message 内 only carry 索引 (bucket_idx, slot_idx, key, version,
      etc.) ≤ 32 B; entry 64 B 充足.
  
  OP_RESPONSE (cache_register reply):
      carries (status, slot.pointer, value_size).
      NO value bytes inline — reader self-fetches via clflushopt+load.
  
  OP_WRITE_FORWARD:
      forwarder 先 NT-store value 到自己的 ForwardStaging[self] 区域 (CXL),
      message carries (key, staging_ptr, value_size, inner_op_type).
      Owner clflushopt+load from staging, executes write, ACK.
      Forwarder frees staging slot upon ACK.

PerHostMessage entry 维持 64 B (单 cacheline, 防 false sharing 历史教训).
═══════════════════════════════════════════════════════════════
```

### VI-B — Implementation cheat sheet (all read/write scenarios, copy-paste ready)

每段 code 直接抄. 每行已标注 primitive type / memory location / 是否需要 fence|flush. 实现 protocol A v2 时不允许 invent — 拿这表对应.

下面 22 个 scenario 覆盖 protocol A v2 全部读写 + message passing + cache 管理路径. **在这 22 个之外的任何 CXL 读写，触发 RAP §XIII**.

#### Scenario 1: Reader fast path (cache hit) — ~50 ns

```c
// CONTEXT: any host, any worker. Most common path (~95% of reads on hot data).
// PHYSICAL: DRAM only. NO CXL access. NO fence. NO flush.

ShardingEntry shard = sharding_table[hash(key)];          // plain load (DRAM, const)

KvCacheEntry *entry = cache_pool.lookup(key);             // plain load chain (DRAM hashmap)

if (entry && !entry->stale) {                             // plain load 1 byte (L1 hot, same cacheline)
    memcpy(out, entry->value_bytes, entry->value_size);   // plain memcpy (DRAM)
    cache_pool.touch_lru(entry);                          // atomic counter inc (DRAM)
    return SUCCESS;
}
goto slow_path;
```

#### Scenario 2: Reader slow path — same-host miss (owner == self) — ~1 µs

```c
// CONTEXT: cache miss, key's owner_host == self_host.
// PHYSICAL: register via local directory (DRAM); fetch from CXL.

ShardingEntry shard = sharding_table[hash(key)];          // plain load (DRAM)
// shard.owner == self_host

uint32_t bucket_idx = hash(key) % num_buckets;
SlotDirectoryEntry *de = NULL;
int slot_idx = -1;

// Step A: scan bucket from CXL with explicit coherence
flush_line(&buckets_[bucket_idx]);                        // clflushopt (CXL evict L3 stale)
flush_line((char*)&buckets_[bucket_idx] + 64);            // clflushopt 2nd cacheline (bucket = 128 B)
full_fence();                                             // mfence
CxlKvBucket b_snap = buckets_[bucket_idx];                // plain load (post-flush, coherent)

for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
    if (b_snap.slots[s].key == key) { slot_idx = s; break; }
}
if (slot_idx < 0) return KEY_NOT_FOUND;

// Step B: register self in directory (host-local)
de = &SlotDirectory[bucket_idx][slot_idx];
pthread_spin_lock(&de->spinlock);                         // DRAM spinlock (PROCESS_SHARED)
de->sharer_bitmap |= (1 << self_host);                    // plain store (DRAM byte, host-local atomic)
de->state = SLOT_STATE_S;
de->version++;
pthread_spin_unlock(&de->spinlock);

// Step C: fetch KV block from CXL
void *block_addr = (void*)b_snap.slots[slot_idx].pointer;
size_t block_size = size_class_to_bytes(b_snap.slots[slot_idx].size_class);

for (size_t off = 0; off < block_size; off += 64)
    flush_line((char*)block_addr + off);                  // clflushopt each cacheline (CXL)
full_fence();                                             // mfence

memcpy(local_value, block_addr, block_size);              // plain memcpy from CXL (now coherent)

// Step D: insert into local cache + set self flag
cache_pool.insert(key, local_value, block_size, stale=false);   // DRAM stores
__atomic_store_n(&local_self_flags[bucket_idx][slot_idx],
                 1, __ATOMIC_RELEASE);                    // atomic byte store (DRAM)

return SUCCESS;
```

#### Scenario 3: Reader slow path — cross-host miss (owner != self) — ~2-3 µs

```c
// CONTEXT: cache miss, owner_host is a peer.
// PHYSICAL: send cache_register via N:1:1:N; response carries (status, slot.pointer);
//           reader self-fetches value bytes from CXL via clflushopt+mfence+load.
//           NO inline value path — KV ≥ 256 B in scope, inline never useful.

ShardingEntry shard = sharding_table[hash(key)];          // plain load (DRAM)
host_id_t owner = shard.owner;                            // owner != self_host

// Step A: enqueue cache_register_request to MPSC aggregator (DRAM)
LocalAggregatorQueue *q = &aggregator_->queues[k_route];

uint64_t pos = __atomic_fetch_add(&q->hdr.tail, 1,
                                  __ATOMIC_ACQ_REL);      // atomic fetch_add (DRAM)
AggrEntry *e = &q->entries[pos % kAggrQueueDepth];

// wait slot free (lazy spin)
while (__atomic_load_n(&e->op_id, __ATOMIC_ACQUIRE) != 0)
    __builtin_ia32_pause();                               // atomic load acquire (DRAM)

uint64_t op_id = generate_op_id(self_host, my_worker_slot);

e->op_type        = OP_CACHE_REGISTER;                    // plain store (DRAM)
e->key            = key;
e->dst_host       = owner;
e->src_worker_slot= my_worker_slot;
std::atomic_thread_fence(std::memory_order_release);      // compiler+CPU release fence
__atomic_store_n(&e->op_id, op_id, __ATOMIC_RELEASE);     // atomic store last (DRAM, release pattern)
// NO clflushopt — DRAM hardware coherent across same-host workers / sender thread

// Step B: spin on worker_ack_buf (DRAM)
WorkerAckSlot *ack = &aggregator_->ack_bufs[k_route].slots[my_worker_slot];
while (__atomic_load_n(&ack->ack_op_id, __ATOMIC_ACQUIRE) != op_id)
    __builtin_ia32_pause();                               // atomic load acquire (DRAM)

// Step C: response carries (status, slot.pointer, value_size) — NO value bytes
if (ack->response_status != STATUS_OK) return KEY_NOT_FOUND;
void *block_addr = ack->response_slot_pointer;            // CXL address
size_t value_size = ack->response_value_size;

// Step D: self-fetch value bytes from CXL
for (size_t off = 0; off < value_size; off += 64)
    flush_line((char*)block_addr + off);                  // clflushopt each cacheline (CXL)
full_fence();                                             // mfence
memcpy(local_value, block_addr, value_size);              // plain memcpy from CXL (now coherent)

// Step E: insert into local cache
cache_pool.insert(key, local_value, value_size, stale=false);  // DRAM
return SUCCESS;
```

#### Scenario 4: Writer UPDATE — owner == self host — ~3-5 µs

```c
// CONTEXT: same-host write. Most common write path under sharding.
// PHYSICAL: directory lock (DRAM) + invalidate sharers via N:1:1:N + atomic CAS slot.pointer (CXL).

uint32_t bucket_idx = hash(key) % num_buckets;

// Step A: read current bucket layout from CXL (find slot for this key)
flush_line(&buckets_[bucket_idx]);                        // clflushopt (CXL)
flush_line((char*)&buckets_[bucket_idx] + 64);            // clflushopt 2nd cacheline
full_fence();                                             // mfence
CxlKvBucket b_snap = buckets_[bucket_idx];                // plain load (post-flush)

int slot_idx = scan_for_key(b_snap, key);
if (slot_idx < 0) return KEY_NOT_FOUND;

uint64_t old_pointer = b_snap.slots[slot_idx].pointer;
SlotDirectoryEntry *de = &SlotDirectory[bucket_idx][slot_idx];

// Step B: directory entry lock (DRAM, host-local)
pthread_spin_lock(&de->spinlock);                         // DRAM PROCESS_SHARED spinlock

// Step C: alloc new KV block (per-owner-host pool, host-local DRAM metadata)
size_class_t sc = size_class(new_value_size);
void *new_block = blockpool_alloc(sc);                    // host-local DRAM atomic on free list

// Step D: write value bytes to new block (CXL) — primitive depends on size
if (new_value_size >= 256) {
    // ≥ 256 B: NT store + sfence (avoid cache pollution)
    for (size_t off = 0; off < new_value_size; off += 64) {
        __m512i v = _mm512_loadu_si512((__m512i*)((char*)new_value + off));
        _mm512_stream_si512((__m512i*)((char*)new_block + off), v);  // NT store (CXL via WC buffer)
    }
    store_fence();                                        // sfence (drain WC to CXL)
} else {
    // < 256 B: plain store + clflushopt per cacheline + sfence
    memcpy(new_block, new_value, new_value_size);         // plain stores (CPU L1)
    size_t flushed = 0;
    for (size_t off = 0; off < new_value_size; off += 64) {
        flush_line((char*)new_block + off);               // clflushopt each cacheline
    }
    store_fence();                                        // sfence
}

// Step E: invalidate other-host sharers via N:1:1:N (sync ACK wait)
uint8_t sharer_set = de->sharer_bitmap & ~(1 << self_host);
if (sharer_set != 0) {
    uint64_t inval_op_id = enqueue_invalidate(bucket_idx, slot_idx, sharer_set);
    // Sender thread will publish to peer host(s); receiver applies; AckChannel returns.
    // We spin on local worker_ack_buf:
    WorkerAckSlot *ack = &aggregator_->ack_bufs[k_route].slots[my_worker_slot];
    while (__atomic_load_n(&ack->ack_op_id, __ATOMIC_ACQUIRE) != inval_op_id)
        __builtin_ia32_pause();                           // atomic load acquire (DRAM)
}
// ★ STRICT-A LINEARIZABILITY POINT: all peer caches invalidated.

// Step F: atomic CAS slot.pointer (★ COMMIT POINT)
//   sharding ensures single-host-writer; lock cmpxchgq is hardware atomic single-host.
bool cas_ok = __atomic_compare_exchange_n(
    &buckets_[bucket_idx].slots[slot_idx].pointer,
    &old_pointer, (uint64_t)new_block,
    /*weak=*/ false,
    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);                  // lock cmpxchgq (CXL, single-host atomic)
if (!cas_ok) { /* should not happen under sharding; abort */ }

flush_line(&buckets_[bucket_idx].slots[slot_idx]);        // clflushopt (CXL — peer hosts can see)
store_fence();                                            // sfence

// Step G: directory state update (DRAM, host-local)
de->sharer_bitmap = (1 << self_host);                     // plain store (DRAM)
de->state = SLOT_STATE_M;
de->version++;

pthread_spin_unlock(&de->spinlock);

// Step H: update local cache_pool (DRAM)
cache_pool.update(key, new_value, new_value_size, stale=false);  // DRAM stores

// Step I: queue old block for lazy GC (DRAM, host-local list)
gc_queue.push((void*)old_pointer);

return SUCCESS;
```

#### Scenario 5: Writer INSERT — owner == self host — ~3-5 µs

```c
// CONTEXT: same-host insert. Differs from UPDATE in: scan for empty slot,
//          publish slot.key + slot.pointer together (16B atomic or 2-step).

uint32_t bucket_idx = hash(key) % num_buckets;
flush_line(&buckets_[bucket_idx]);                        // clflushopt
flush_line((char*)&buckets_[bucket_idx] + 64);            // clflushopt
full_fence();                                             // mfence
CxlKvBucket b_snap = buckets_[bucket_idx];                // plain load

int empty_idx = -1;
for (int s = 0; s < kCxlKvSlotsPerBucket; s++) {
    if (b_snap.slots[s].key == key) return KEY_EXISTS;
    if (empty_idx < 0 && b_snap.slots[s].key == EMPTY_KEY) empty_idx = s;
}
if (empty_idx < 0) return BUCKET_FULL;

SlotDirectoryEntry *de = &SlotDirectory[bucket_idx][empty_idx];
pthread_spin_lock(&de->spinlock);                         // DRAM

void *new_block = blockpool_alloc(size_class(value_size));// DRAM atomic
// write value bytes — same as Scenario 4 Step D (NT vs plain by size)
if (value_size >= 256) {
    /* NT store loop + sfence */
} else {
    /* memcpy + clflushopt loop + sfence */
}

// invalidate sharers on the empty slot's directory (someone may have cached the empty slot's stale state)
uint8_t sharer_set = de->sharer_bitmap & ~(1 << self_host);
if (sharer_set != 0) {
    uint64_t op_id = enqueue_invalidate(bucket_idx, empty_idx, sharer_set);
    while (__atomic_load_n(&ack->ack_op_id, __ATOMIC_ACQUIRE) != op_id)
        __builtin_ia32_pause();
}

// publish slot — 2 options:

// Option 5a (simple, recommended): publish_slot pattern (value first, then key)
buckets_[bucket_idx].slots[empty_idx].pointer = (uint64_t)new_block;  // plain store (CXL)
flush_line(&buckets_[bucket_idx].slots[empty_idx]);       // clflushopt
store_fence();                                            // sfence
__atomic_store_n(&buckets_[bucket_idx].slots[empty_idx].key,
                 key, __ATOMIC_RELEASE);                  // atomic 8B store (CXL)
flush_line(&buckets_[bucket_idx].slots[empty_idx]);       // clflushopt
store_fence();                                            // sfence

// Option 5b (faster, requires 16B alignment): SSE2 16B atomic publish
__m128i new_slot = _mm_set_epi64x((int64_t)new_block, (int64_t)key);
_mm_store_si128((__m128i*)&buckets_[bucket_idx].slots[empty_idx], new_slot);  // 16B atomic
flush_line(&buckets_[bucket_idx].slots[empty_idx]);       // clflushopt
store_fence();                                            // sfence
// Note: requires reader to use _mm_load_si128 to see atomic 16B; since reader uses
// plain load + key check, Option 5a is safer default.

// directory state update
de->sharer_bitmap = (1 << self_host);                     // DRAM
de->state = SLOT_STATE_M;
de->version++;
pthread_spin_unlock(&de->spinlock);

cache_pool.insert(key, new_value, value_size, stale=false);    // DRAM
return SUCCESS;
```

#### Scenario 6: Writer DELETE — owner == self host — ~2-4 µs

```c
// CONTEXT: same-host delete. Set slot.key = EMPTY_KEY; old block to GC immediately.

uint32_t bucket_idx = hash(key) % num_buckets;
flush_line(&buckets_[bucket_idx]);                        // clflushopt
flush_line((char*)&buckets_[bucket_idx] + 64);            // clflushopt
full_fence();                                             // mfence
CxlKvBucket b_snap = buckets_[bucket_idx];                // plain load

int slot_idx = scan_for_key(b_snap, key);
if (slot_idx < 0) return KEY_NOT_FOUND;

SlotDirectoryEntry *de = &SlotDirectory[bucket_idx][slot_idx];
pthread_spin_lock(&de->spinlock);                         // DRAM

uint64_t old_pointer = b_snap.slots[slot_idx].pointer;

// invalidate sharers
uint8_t sharer_set = de->sharer_bitmap & ~(1 << self_host);
if (sharer_set != 0) {
    uint64_t op_id = enqueue_invalidate(bucket_idx, slot_idx, sharer_set);
    while (__atomic_load_n(&ack->ack_op_id, __ATOMIC_ACQUIRE) != op_id)
        __builtin_ia32_pause();
}

// atomic store empty marker (commit point)
__atomic_store_n(&buckets_[bucket_idx].slots[slot_idx].key,
                 EMPTY_KEY, __ATOMIC_RELEASE);            // atomic 8B store (CXL)
flush_line(&buckets_[bucket_idx].slots[slot_idx]);        // clflushopt
store_fence();                                            // sfence

de->sharer_bitmap = 0;                                    // DRAM
de->state = SLOT_STATE_I;
de->version++;
pthread_spin_unlock(&de->spinlock);

cache_pool.invalidate(key);                               // DRAM
gc_queue.push((void*)old_pointer);                        // DRAM
return SUCCESS;
```

#### Scenario 7: Cross-host write forward (owner != self) — via ForwardStaging buffer

```c
// CONTEXT: Self-host worker decided owner is peer; forward op to peer via N:1:1:N.
// PHYSICAL: forwarder writes value bytes into its own ForwardStaging buffer (CXL),
//           message carries CXL pointer to staging slot.
//           Owner-side worker reads from staging via clflushopt+load,
//           executes local write (Scenario 4/5/6), sends ACK back.
//           Forwarder frees staging slot upon receiving ACK.
//           NO inline value path (KV ≥ 256 B in scope).

// === Forwarder side ===
host_id_t owner = sharding_table[hash(key)].owner;

// Step A: alloc staging slot from forwarder's own ForwardStaging[self_host]
void *staging_addr = forward_staging_alloc(self_host, value_size);  // host-local DRAM metadata

// Step B: write value bytes to staging buffer on CXL (size ≥ 256 B → NT store path)
for (size_t off = 0; off < value_size; off += 64) {
    __m512i v = _mm512_loadu_si512((__m512i*)((char*)value + off));
    _mm512_stream_si512((__m512i*)((char*)staging_addr + off), v);  // NT store (CXL)
}
store_fence();                                            // sfence: drain WC to CXL

// Step C: enqueue OP_WRITE_FORWARD message with staging pointer (no inline payload)
LocalAggregatorQueue *q = &aggregator_->queues[k_route];
uint64_t pos = __atomic_fetch_add(&q->hdr.tail, 1, __ATOMIC_ACQ_REL);
AggrEntry *e = &q->entries[pos % depth];
while (__atomic_load_n(&e->op_id, __ATOMIC_ACQUIRE) != 0) __builtin_ia32_pause();
e->op_type        = OP_WRITE_FORWARD;                     // plain store (DRAM)
e->inner_op_type  = OP_UPDATE | OP_INSERT | OP_DELETE;
e->dst_host       = owner;
e->key            = key;
e->staging_ptr    = (uint64_t)staging_addr;               // CXL address
e->value_size     = value_size;
e->src_worker_slot= my_slot;
std::atomic_thread_fence(std::memory_order_release);
__atomic_store_n(&e->op_id, op_id, __ATOMIC_RELEASE);     // atomic store last

// Step D: spin on worker_ack_buf (DRAM)
while (__atomic_load_n(&ack->ack_op_id, __ATOMIC_ACQUIRE) != op_id) __builtin_ia32_pause();
int status = ack->response_status;

// Step E: free staging slot (forwarder side — no message needed; staging life-cycle is
//          forwarder-side managed; ACK from owner = "I no longer reference staging")
forward_staging_free(self_host, staging_addr);            // host-local DRAM
return status;

// === Owner side (in receiver thread, OP_WRITE_FORWARD case in Scenario 10) ===
// On receipt of OP_WRITE_FORWARD:
//   1. Copy value bytes from staging:
//        for off in 0..value_size by 64: clflushopt(staging_ptr + off);
//        mfence; memcpy(local_value, staging_ptr, value_size);
//   2. Execute Scenario 4/5/6 (UPDATE/INSERT/DELETE) with the copied value
//   3. Send response via reverse N:1:1:N (ack only, no payload)
```

#### Scenario 8: Cache evict (LRU pressure) — host-local — ~200 ns

```c
// CONTEXT: cache pool reached size limit; LRU thread picks oldest entry to evict.
// PHYSICAL: drop entry from local cache + send cache_evict to owner.

KvCacheEntry *e = lru_pick_victim();                      // DRAM
uint32_t bucket_idx = e->bucket_idx;
int slot_idx = e->slot_idx;
host_id_t owner = sharding_table[hash(e->key)].owner;

// remove from local cache (mark stale or remove from hashmap)
e->stale = 1;                                             // atomic byte store (DRAM)
__atomic_store_n(&local_self_flags[bucket_idx][slot_idx], 0, __ATOMIC_RELEASE);

if (owner == self_host) {
    // local: directly modify directory
    SlotDirectoryEntry *de = &SlotDirectory[bucket_idx][slot_idx];
    pthread_spin_lock(&de->spinlock);                     // DRAM
    de->sharer_bitmap &= ~(1 << self_host);               // DRAM
    de->version++;
    pthread_spin_unlock(&de->spinlock);
} else {
    // remote: send cache_evict via N:1:1:N (fire-and-forget; no ACK needed)
    LocalAggregatorQueue *q = &aggregator_->queues[k_route];
    uint64_t pos = __atomic_fetch_add(&q->hdr.tail, 1, __ATOMIC_ACQ_REL);
    AggrEntry *ae = &q->entries[pos % depth];
    while (__atomic_load_n(&ae->op_id, __ATOMIC_ACQUIRE) != 0) __builtin_ia32_pause();
    ae->op_type = OP_CACHE_EVICT;                         // DRAM
    ae->dst_host = owner;
    ae->bucket_idx = bucket_idx;
    ae->slot_idx = slot_idx;
    ae->src_worker_slot = my_slot;
    std::atomic_thread_fence(std::memory_order_release);
    __atomic_store_n(&ae->op_id, op_id, __ATOMIC_RELEASE);
    // No spin on ack — fire-and-forget
}

cache_pool.remove(e->key);                                // DRAM hashmap remove
blockpool_free_local_dram_entry(e);                       // DRAM (entry struct itself is DRAM)
```

#### Scenario 9: Sender thread — drain DRAM MPSC → publish CXL SPSC ring

```c
// CONTEXT: 1 thread per host per channel. Drains the K aggregator queue,
//          batches per-dst entries, publishes to CXL SPSC ring with single sfence per batch.

LocalAggregatorQueue *q = &aggregator_->queues[k_id];

uint64_t mpsc_tail = __atomic_load_n(&q->hdr.tail, __ATOMIC_ACQUIRE);  // DRAM
uint64_t head = q->hdr.head;                              // plain load (single-consumer)

// Local pending batch buffers (DRAM)
PerHostInvalEntry pending[kMaxPhysicalHosts][kBatchMax];
uint32_t batch_count[kMaxPhysicalHosts] = {0};

while (head < mpsc_tail) {
    AggrEntry *ae = &q->entries[head % kAggrQueueDepth];
    uint64_t op_id = __atomic_load_n(&ae->op_id, __ATOMIC_ACQUIRE);  // atomic load (DRAM)
    if (op_id == 0) break;                                // not yet published by producer
    int dst = ae->dst_host;
    if (batch_count[dst] >= kBatchMax) break;             // batch full
    pending[dst][batch_count[dst]] = (PerHostInvalEntry){
        .bucket_idx = ae->bucket_idx,
        .slot_idx = ae->slot_idx,
        .op_type = ae->op_type,
        .src_worker_slot = ae->src_worker_slot,
        .src_worker_op_id = op_id
    };
    batch_count[dst]++;
    __atomic_store_n(&ae->op_id, 0, __ATOMIC_RELEASE);    // free DRAM aggregator slot
    head++;
}
q->hdr.head = head;                                       // plain store (single-consumer)

// For each dst with pending: publish batch to CXL ring
for (int dst = 0; dst < phys_hosts_pr_; dst++) {
    if (dst == my_phys_host_pr_ || batch_count[dst] == 0) continue;
    PerHostSpscRing *r = &per_host_rings_->rings[my_phys_host_pr_][dst][k_id];
    uint64_t tail_pos = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);  // single producer

    for (uint32_t i = 0; i < batch_count[dst]; i++) {
        PerHostInvalEntry *e = &r->entries[(tail_pos + i) % kPerHostSpscDepth];

        // wait slot free (consumer cleared op_id=0)
        for (;;) {
            flush_line(e);                                // clflushopt (CXL)
            full_fence();                                 // mfence
            if (e->src_worker_op_id == 0) break;          // plain load (post-flush, coherent)
            __builtin_ia32_pause();
        }

        // write entry fields to CXL (plain stores; not yet visible)
        e->bucket_idx       = pending[dst][i].bucket_idx;
        e->slot_idx         = pending[dst][i].slot_idx;
        e->op_type          = pending[dst][i].op_type;
        e->src_worker_slot  = pending[dst][i].src_worker_slot;
        compiler_barrier();
        e->src_worker_op_id = pending[dst][i].src_worker_op_id;       // last (release pattern)
        compiler_barrier();
        flush_line(e);                                    // clflushopt (CXL — push to backing)
    }
    tail_pos += batch_count[dst];

    __atomic_store_n(&r->tail, tail_pos, __ATOMIC_RELEASE);// atomic tail store (CXL)
    flush_line(&r->tail);                                 // clflushopt
    store_fence();                                        // sfence (single batch barrier)

    // wait for AckChannel.seq to reach tail_pos (peer receiver acked all entries)
    AckChannel *ach = &per_host_rings_->acks[my_phys_host_pr_][dst][k_id];
    for (;;) {
        flush_line(&ach->seq);                            // clflushopt (CXL)
        full_fence();                                     // mfence
        if (__atomic_load_n(&ach->seq, __ATOMIC_ACQUIRE) >= tail_pos) break;
        __builtin_ia32_pause();
    }

    // flip worker_ack_buf for each worker that contributed an entry
    for (uint32_t i = 0; i < batch_count[dst]; i++) {
        WorkerAckSlot *aks = &aggregator_->ack_bufs[k_id]
                              .slots[pending[dst][i].src_worker_slot];
        __atomic_store_n(&aks->ack_op_id,
                         pending[dst][i].src_worker_op_id,
                         __ATOMIC_RELEASE);              // atomic store (DRAM)
    }
}
```

#### Scenario 10: Receiver thread — drain CXL ring → apply directory updates → ack

```c
// CONTEXT: 1 thread per host per channel. Drains rings[*][me][k_id], applies
//          invalidations / cache_register / forward / cache_evict to local
//          directory + cache pool, then publishes ack_seq.

for (int src = 0; src < phys_hosts_pr_; src++) {
    if (src == my_phys_host_pr_) continue;
    PerHostSpscRing *r = &per_host_rings_->rings[src][my_phys_host_pr_][k_id];

    // read tail from CXL
    flush_line(&r->tail);                                 // clflushopt
    full_fence();                                         // mfence
    uint64_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);  // atomic load (post-flush)
    uint64_t head = r->head;                              // plain load (single consumer)

    while (head < tail) {
        PerHostInvalEntry *e = &r->entries[head % kPerHostSpscDepth];
        flush_line(e);                                    // clflushopt
        full_fence();                                     // mfence
        uint64_t op_id = e->src_worker_op_id;             // plain load (post-flush)
        if (op_id == 0) break;                            // sender hasn't published yet

        // Apply to local directory + cache, depending on op_type
        switch (e->op_type) {
        case OP_INVALIDATE: {
            // peer (owner) tells us to drop our cache entry for (bucket, slot)
            uint32_t b = e->bucket_idx, s = e->slot_idx;
            // mark cache stale for any entry whose bucket/slot match
            cache_pool.set_stale_by_slot(b, s);           // atomic byte store (DRAM)
            __atomic_store_n(&local_self_flags[b][s], 0, __ATOMIC_RELEASE);
            break;
        }
        case OP_CACHE_REGISTER: {
            // peer (reader) registering itself in our directory
            SlotDirectoryEntry *de = &SlotDirectory[e->bucket_idx][e->slot_idx];
            pthread_spin_lock(&de->spinlock);             // DRAM
            de->sharer_bitmap |= (1 << src);              // DRAM
            de->state = SLOT_STATE_S;
            de->version++;
            uint64_t slot_pointer = buckets_[e->bucket_idx]
                                    .slots[e->slot_idx].pointer; // plain load (post-flush done in step above)
            uint32_t value_size = size_class_to_bytes(
                buckets_[e->bucket_idx].slots[e->slot_idx].size_class);
            pthread_spin_unlock(&de->spinlock);
            // Build response (pointer + size only, NO inline value bytes)
            enqueue_response(src, e->src_worker_slot, e->src_worker_op_id,
                             STATUS_OK, slot_pointer, value_size);
            break;
        }
        case OP_CACHE_EVICT: {
            // peer dropped its cache; remove from sharer set
            SlotDirectoryEntry *de = &SlotDirectory[e->bucket_idx][e->slot_idx];
            pthread_spin_lock(&de->spinlock);
            de->sharer_bitmap &= ~(1 << src);
            de->version++;
            pthread_spin_unlock(&de->spinlock);
            // no response needed (fire-and-forget)
            break;
        }
        case OP_WRITE_FORWARD: {
            // peer is forwarding a write op via ForwardStaging buffer.
            // Step 1: read value bytes from forwarder's staging
            char local_value[kMaxValueSize];
            for (size_t off = 0; off < e->value_size; off += 64)
                flush_line((char*)e->staging_ptr + off);   // clflushopt (CXL)
            full_fence();                                   // mfence
            memcpy(local_value, (void*)e->staging_ptr, e->value_size);  // plain memcpy from CXL
            // Step 2: execute local write (Scenario 4/5/6)
            execute_local_write(e->key, local_value, e->value_size, e->inner_op_type);
            // Step 3: send forward-response (status only, no value bytes)
            enqueue_response(src, e->src_worker_slot, e->src_worker_op_id,
                             STATUS_OK, /*slot_pointer=*/0, /*value_size=*/0);
            break;
        }
        }

        // free CXL ring slot
        __atomic_store_n(&e->src_worker_op_id, 0, __ATOMIC_RELEASE);  // atomic 8B store (CXL)
        compiler_barrier();
        flush_line(e);                                    // clflushopt (CXL)
        head++;
    }
    r->head = head;                                       // plain store (single consumer)

    // publish AckChannel.seq advance
    AckChannel *ach = &per_host_rings_->acks[src][my_phys_host_pr_][k_id];
    __atomic_store_n(&ach->seq, head, __ATOMIC_RELEASE);  // atomic store (CXL)
    flush_line(&ach->seq);                                // clflushopt
    store_fence();                                        // sfence (single barrier per batch)
}
```

#### Scenario 11-22: Smaller ops (table form)

These ops are common but small enough to express as 1-2 line primitive sequences:

| # | Op | Primitive sequence |
|---|---|---|
| 11 | Sharding lookup | `ShardingEntry s = sharding_table[hash(k)]` — plain load (DRAM, const) |
| 12 | Cache LRU touch on hit | `__atomic_fetch_add(&entry->lru_epoch, 1, __ATOMIC_RELAXED)` — DRAM |
| 13 | Cache stale flag set (receiver) | `__atomic_store_n(&entry->stale, 1, __ATOMIC_RELEASE)` — DRAM 1 byte |
| 14 | Cache stale flag check (reader) | plain load 1 byte (no atomic needed for x86 single-byte) |
| 15 | LocalSelfFlag set after register ACK | `__atomic_store_n(&local_self_flags[b][s], 1, __ATOMIC_RELEASE)` — DRAM |
| 16 | Directory.version observability read | `__atomic_load_n(&de->version, __ATOMIC_RELAXED)` — DRAM |
| 17 | KV blockpool alloc | host-local DRAM spinlock on free list head; `__atomic_store_n(head, head->next, RELAXED)` |
| 18 | KV blockpool free (lazy GC) | DRAM spinlock + push to free list head |
| 19 | MPSC tail fetch_add (worker side) | `__atomic_fetch_add(&q->tail, 1, ACQ_REL)` — DRAM (sender consumes) |
| 20 | MPSC head update (sender side) | `q->head = new_head` — plain store (single consumer) |
| 21 | SPSC tail update (sender side, CXL) | `__atomic_store_n(&r->tail, new_tail, RELEASE)` + clflushopt + sfence |
| 22 | SPSC head update (receiver side, CXL) | `r->head = new_head` — plain store (single consumer reads only) |

**注意**: 上面 SPSC head 在 CXL 但只有 receiver 写, sender 不读 (sender 只读 ackChannel.seq), 所以 head 不需要 clflushopt — 直接 plain store 即可.

#### Quick-reference: 何时不需要 clflushopt + fence?

| 场景 | 不需要 flush/fence 因为... |
|---|---|
| 同 host 多 worker 间共享 DRAM (MAP_SHARED) | hardware coherence 自动保证 |
| 单 host 内 thread/process 间 atomic ops | x86 hardware atomicity for ≤ 8B aligned |
| sharding_table read-only | initialized once, no mutation |
| MPSC aggregator queue (DRAM) | DRAM coherent |
| LocalSelfFlags (DRAM) | DRAM coherent |
| Cache pool entries (DRAM, MAP_SHARED) | DRAM coherent across same-host workers |

#### Quick-reference: 必须 clflushopt + fence 的场景

| 场景 | 理由 |
|---|---|
| 任何 host 写 CXL bytes 让对端 host 看到 | 无跨 host coherence |
| 任何 host 读 CXL bytes 假定对端 host 改过 | 自己 L3 可能 stale |
| 写 SPSC ring entry (CXL) | sender 写, receiver 在对端 host 读 |
| 写 AckChannel.seq (CXL) | receiver 写, sender 在对端 host 读 |
| 写 slot.pointer / slot.key (CXL) | writer 写, reader 在所有 host 可能读 |
| 写 KV block bytes (CXL) | writer 写, reader 在对端 host 可能 follow pointer 读 |
| 读 SPSC ring entry / AckChannel.seq / slot 字段 (CXL) | 对端可能改过 |

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


### X — Enforcement mechanisms

> 严肃看待: spec 写出来不等于被遵守. iter-3A 的 Finding-1 整整两 iter
> 才发现, 因为靠 "Claude 应该读 spec" 这种声明式约束没用.
>
> 下面分两类: **Hard enforcement** (违反就 fail, 不依赖人/Claude 自觉)
> 和 **Process discipline** (依赖人/Claude follow, 但带反馈机制).
>
> 声明式条款 (e.g. "应该这样做") 不进 enforcement, 只算 documentation
> intent.

#### Hard enforcement (违反就 fail / 不能 merge / 不能 ship)

**H1 — Compile-time / runtime asserts**

Spec 里每条 invariant/AP 必须评估能否转成 assert; 能转的就转, 不能转
的在 spec 里**显式标注** "advisory only" 跟 "hard-enforced" 区分.

| Invariant / AP | Assert / abort 形式 | Coverage |
|---|---|---|
| I3 (cache MAP_SHARED) | `cache_pool::init` 检查 mmap flag, else `abort` | hard |
| I8 (directory lock = host-local) | `SlotDirectoryEntry::spinlock` 用 distinct type `host_local_spinlock_t`, 不能赋 `shm_mutex_t*` (编译期 reject) | hard |
| I12 (no OpLog) | OpLog write API `[[deprecated]]` + runtime `abort` if called | hard |
| AP13 (routing field defaults) | `attach()` 末尾 `if (per_host_rings_enabled_ && phys_hosts_pr_==1 && atoi(getenv("FUSEE_NUM_HOSTS"))!=1) abort("AP13")` | hard |
| AP14 (silent broadcast) | `invalidate_sharers()` 内 `if (sharers_count==H && !explicit_broadcast) abort("AP14")` | hard |
| AP15 (cache fill before register ACK) | reader cache_fill API 必须传 `register_acked=true` flag, false 时 abort | hard |
| I1, I2, I5, I6, I9, I10, I11 | **结构性约束, 无单点 assert 可表达** | **advisory — 靠 H2/H3/P1/P2 覆盖** |

**H2 — CI invariant tests** (`tests/protocol_a_v2_invariant_check.cc`)

每个改 protocol A 文件的 PR 必须通过:
- I1 storage check: cxl_region size > local_dram footprint
- I2 forward injection: 注入跨 host write 验证 forward counter ↑
- I3 mmap flag check: procfs 验证 cache pool MAP_SHARED
- I9 / I10 race tests (TSan / helgrind): register-before-fill, write-then-peer-read
- AP13 trip wire: 启动 SIGABRT test 在 phys_hosts_pr_==1 但 NUM_HOSTS!=1 时

CI fail = PR 不能 merge.

**H3 — Sweep validation gates with hard fail**

iter-4A 之后所有 sweep 的 `SUMMARY.log` 必须有 5 行 gate result. **gate
缺失或失败让 sweep script 直接 abort, 数据不写出来** (而非只标 invalid):

```
# G1 hash-diff: __/20 PASS  (must = 20)
# G2 stability σ/median: __% (must ≤ 5%)
# G3 N:1:1:N activation: avg dispatch_loop_iter = __ (must > 0)
# G4 directory hit rate: avg sharers/write = __
# G5 forward routing: cross-host write % = __, p50/p99 forward = __/__ µs
```

直接防御 iter-3A "silent no-op 路径没人发现" 的失败模式 — 有 trip-wire,
不是 advisory.

**H4 — Pre-commit grep hook**

`.git/hooks/pre-commit` 检查: 改 `src/cxl_kv_ops_A*.{h,cc}` /
`src/cxl_directory*` / `src/cxl_sharding*` / `src/cxl_cache_pool*` 文件
的 commit message 必须 grep 到 `\b[Ii][1-9][0-2]?\b` 或 `\bAP[0-9]+\b`
(invariant 编号或 AP 编号), 否则 reject commit.

强制 commit author 显式说"这次改动跟 I3/I8/AP13 有关", 不能写"random
fix" 蒙混.

#### Process discipline (依赖人/Claude follow, 但有反馈)

**P1 — Reviewer Attack Process (RAP) for any design proposal**

见 §XIII. **任何提议新 design choice / optimization 的输出必须包含 RAP
analysis subsection**, 否则 user 直接 reject proposal. 这是 user-side
强制 — 由 user 看 proposal 时检查格式, 不依赖 Claude 自觉. RAP 强制
设计提案者 stress-test 自己的提议, 把 reviewer attack 内化为决策流程.

**P2 — Iter retrospective spec-drift audit**

每个 iter 结束写 retrospective doc 时**必须**包含 "spec drift check"
section: 逐条 grep 当前 protocol A 实现, 反查每个 I/AP 是否在实现中
对应到具体 code location. 找不到对应或对应错的, 立即 user-escalate.

iter-3A 的 Finding-1 在这个 audit 流程下能在 iter 结束时发现 (audit
应该问: "I2 sharding 的 owner_host 路由在哪个文件? 跟 phys_hosts_pr_
是同一个变量吗? phys_hosts_pr_ 在 attach() 里赋值了吗?" — 走一遍这流程
立刻发现 Finding-1).

**P3 — User-side audit on design proposal**

User 看 Claude 的 design proposal 时**应该**逐条问 "是否违反 I/AP".
这把 enforcement 从 Claude-side (不可靠 — 我自己上一轮就没主动逐条 check)
转移到 User-side (可靠 — user 会主动问).

User 的 audit checklist 在 §XI (PR review checklist), 但 design proposal
阶段就要 inline review, 不等到 PR.

#### 哪些原 enforcement 删除 (诚实 reality check)

- **原 E1 (CLAUDE.md cross-reference "应该读 spec")**: **删除**, 无强制
  力. CLAUDE.md auto-load ≠ Claude 真的逐条对照 spec. iter-4A 不指望靠
  这个 enforce.
- **原 E5 (Spec change 走 user)**: **降级为 P3** documentation discipline,
  靠 user 主动 enforce, 不靠 Claude 主动 escalate.

### XI — PR review checklist (任何 protocol A 改动 PR 描述里 paste)

```
## Protocol A v2 spec compliance (design_goals.md §Protocol A v2)

For each invariant, check ✓ if change is consistent, ✗ if change
violates (and explain why violation is acceptable / spec needs update).

- [ ] I1 (CXL = authoritative; DRAM = cache only)
- [ ] I2 (Sharding: writer on owner_host)
- [ ] I3 (Same-host MAP_SHARED cache, sharers tracked at host level)
- [ ] I4 (Lazy stale flag, not physical delete on invalidate)
- [ ] I5 (Single-level per-slot directory, not per-bucket)
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
| O3 | Forward / cache_register response | **NO inline value bytes anywhere**. Response carries (status, slot.pointer, value_size); reader self-fetches value bytes from CXL via clflushopt+load. Forward op (OP_WRITE_FORWARD) value bytes go through per-host ForwardStaging buffer on CXL (§III). 决策理由: 测试 KV size 都 ≥ 256 B, inline 不可能 fit; 单 oob path 省一套 code path 跟 correctness reasoning. |
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


### XIII — Reviewer Attack Process (RAP) — design 决策的 mandatory 模板

> Inspired by user observation: "用 reviewer 会如何攻击 design 来反思
> design choice 效果不错". 这是 metacognitive tool, 强制设计提案者
> stress-test 自己的提议, 把"顶会 reviewer 角度"内化为标准决策流程.

#### 何时触发 RAP

任何下列场景 Claude 输出 design 提议时**必须**包含 RAP analysis
subsection:

- 提出新 architecture choice
- 提出 optimization (改 algorithm, 改 data structure, 改 sync 原语)
- 选 default value (e.g. timeout, batch size, threshold)
- 在两个 implementation alternative 间做选择
- 修改本 spec (任何 I/AP 变更)
- 决定接受/拒绝某个 user 提议

如果 Claude 跳过 RAP 直接给 implementation 建议, **user 直接 reject
proposal** 让重出.

#### RAP 6 步格式 (mandatory output)

```
STATE
─────
[一句话提议: "我建议 X, 因为 Y"]

ATTACK VECTORS (≥ 6, 必须从下面 6 类各选 1+)
──────────────────────────────────────────
1. PERFORMANCE attack:
   - 真有改善吗? 跟 baseline (e.g. FUSEE 原版 / 当前 implementation) 比?
   - Worst case 是什么? p99/p999 怎么样?
   - Resource cost: memory, CPU, CXL bandwidth?

2. CORRECTNESS attack:
   - Crash consistency? Concurrent reader/writer race?
   - 在所有 invariant (I1-I12) 下都 hold?
   - 重试 / GC / 异步 fiber 引入的 ordering 问题?

3. GENERALITY attack:
   - 什么 workload 失效? Edge cases?
   - 假设 (e.g. value size 固定, key 分布 uniform) 在实际 workload 下成立吗?
   - Variable-length, skewed access, hot key 下表现?

4. COMPLEXITY attack:
   - 几条 code path? 几套 correctness reasoning?
   - 多了多少 LOC? 多少 new abstraction?
   - 后续维护 cost? Debug 难度?

5. PRIOR ART / 跟现有系统对比 attack:
   - FUSEE 原版怎么做的? LSM / BwTree / state-of-art 怎么做的?
   - 为什么我们的不同? Ablation 在哪?
   - 有 published 的反例吗?

6. IMPLEMENTATION FEASIBILITY attack:
   - 项目 budget 内能落?
   - 依赖什么 (硬件, OS, 库)? 不可用怎么办?
   - 测试可行性: 怎么 verify 设计成立?

ABLATION CHECK
──────────────
[这优化跑过 baseline 对照吗? 收益可归因到具体 mechanism 吗?
 还是只是"看上去应该快"? 如果还没数据, 提议先做 ablation 再决策.]

PRIOR ART CHECK  
───────────────
[有 published 工作做过类似事吗? 成功 / 失败原因? 我们的 setting 哪里
 不同, 为什么 prior art 的失败模式在这里不会重现?
 如果完全没 prior art, 显式承认 "no prior art known".]

VERDICT
───────
对每个 attack 给 1-2 句 response:
- 哪些 attack 能回应 (具体说明 why)
- 哪些 attack 是 fatal (无法回应 → REJECT or MODIFY proposal)
- 哪些 attack 是 acceptable cost (清楚说明 cost 范围)

DECISION
────────
ACCEPT | MODIFY (说明改成什么) | REJECT
+ concrete reason linked to verdict
```

#### 完整范例 (in-place vs CoW, 上一轮真实 RAP)

```
STATE
─────
我建议 UPDATE 走 in-place value update 当 size class 同, fall back to
CoW 当 size class 变.

ATTACK VECTORS
──────────────
1. PERFORMANCE: in-place 省一次 alloc, 但 CoW 的 alloc 是 size-class
   pool 拿一个 free slot (~50 ns), 节省其实 marginal.
2. CORRECTNESS: > 8B value 在 CXL persistent memory 下没原子性. clflushopt
   + sfence 保证 cacheline visibility, 不保证 mid-write atomicity. Crash
   会导致 partial-write. **Fatal under strict-A + CXL persistence**.
3. GENERALITY: variable-length value 必须 fall back to CoW; 两套 path.
   Workload value-size skew 下 in-place fast path 比例不可预测.
4. COMPLEXITY: 两套 code path = 两套 correctness reasoning + 两套 test
   matrix. iter-4A spec scope 下 LOC ↑ ~40%.
5. PRIOR ART: FUSEE 原版选 CoW (Client::kv_update RDMA WRITE new block
   then CAS pointer). LSM/BwTree/Bigtable/PostgreSQL MVCC 全选 CoW.
   Memcached/Redis 选 in-place 但都纯 in-memory cache 无 persistence
   要求. **No published persistent KV that chose in-place over CoW**.
6. IMPLEMENTATION: 两 path 实现 + test 1.4× 工作量, 项目 budget 紧.

ABLATION CHECK
──────────────
没跑 in-place vs CoW micro-bench 数据. PRIOR ART 表明 CoW 在 KV size
≤ 1KB 时性能不输 in-place. 没数据支持 in-place gain claim.

PRIOR ART CHECK
───────────────
FUSEE/LSM/BwTree/Bigtable/PostgreSQL 全 CoW. Memcached/Redis 是 cache
不持久. 没 prior art 在 persistent KV 上选 in-place over CoW.

VERDICT
───────
- Attack 2 (CORRECTNESS) FATAL: CXL persistent + > 8B value 无原子性是
  硬伤, 改不了.
- Attack 5 (PRIOR ART) FATAL: 无任何 published 工作 support, reviewer
  问 "why differ from FUSEE original?" 时无法回答.
- Attack 1 (PERFORMANCE) 不成立: 没 ablation 数据 support gain.
- Attack 3, 4, 6 是 cost, 不 fatal but bad value.

DECISION
────────
REJECT in-place. ADOPT CoW only as the unique UPDATE path. Matches
FUSEE original (preserve-original 硬约束 progress.md:410), eliminates
correctness risk, simpler, immune to all 6 reviewer attacks.
```

这个 RAP 让上一轮 in-place vs CoW 的决策**有可审计的 reasoning trace**.
没有 RAP, 决策依据 invisible, 容易 silent 错。

#### RAP 失败模式

RAP 不是万能。常见失败:

- **Attack 列得不充分** (只 list 2-3 个轻 attack 跳过 6 类): user 看到立即 reject 让重出
- **Attack 列了但 verdict 摇摆** (没明确判 fatal/acceptable): user 要求每条 attack 必须 binary 判定
- **PRIOR ART 写 "no prior art"** 但实际有 (Claude 偷懒): user 反向 attack "你查过 X paper 没"
- **Ablation 写 "可以以后跑"**: user 要求要么现在跑要么 proposal hold

#### 反馈 loop

每个 iter retrospective (P2) 必须包含 "RAP retrospective" section: 列
当 iter 走过 RAP 的 design choice, 实际 implementation 跑出来后 attack
预测中哪些被验证 / 哪些没. 这建立 attack quality calibration —
"crash consistency attack 在 CXL persistent 下确实 fatal" 这种 lesson 内化.
