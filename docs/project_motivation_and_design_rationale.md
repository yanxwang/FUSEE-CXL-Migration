# FUSEE-CXL: Design Rationale and Research Retrospective

**Status**: living draft (paper seed) | **Last updated**: 2026-06-05
**Scope**: project-level design motivation, lineage placement, choice-tree justification, performance-data anchoring. Drafted to (a) be defended in advisor discussions, (b) serve as the starting point for a future paper's Introduction / Background / Motivation sections.
**Companion docs**: per-iter summaries under [docs/iters/](iters/); CXL/RDMA reference PDFs under [documents/](../documents/).

---

## 中文 TL;DR (orientation only — full argument is in the English body)

FUSEE-CXL 是 CXL 时代的 "Clover → FUSEE": 把集中式 metadata/buffer-fusion server 从 CXL 全 disaggregate KV store 中拿掉. 由于 CXL 2.0 不提供跨 host 硬件 cache coherence (这是和 RDMA 的本质区别), 我们用**静态 key-range partition + 软件 directory** 在协议层重建跨 host 一致性. 关键 leverage: 单机 coherence domain (一个 host + 它本机的 CXL view) 在 CXL 2.0 上是完整的, 静态 partition 把每块可变状态 confine 到一个单机 domain, 把"重建跨 host 一致性"降级成"在 domain 之间路由消息". 一个 CXL message ring 同时承担两个轴 — 写请求路由 (→ owner) + invalidation 路由 (owner → sharer).

---

## 1. Introduction

The disaggregated-memory (DM) architecture separates compute and memory into independent network-attached pools, promising higher resource utilization for cloud-native systems [FUSEE FAST'23]. Two distinct hardware fabrics have been used to realize DM in practice: RDMA, which has now matured for nearly a decade of KV-store research, and CXL Type 3, which has only become commercially viable since the first CXL 2.0 switches shipped in 2024.

In the RDMA era, the field went through a clear architectural progression: from *semi-disaggregated* designs that store KV pairs in the memory pool but retain a centralized metadata server (Clover [Tsai ATC'20]), to *fully-disaggregated* designs that move metadata management onto the clients themselves (FUSEE [Shen FAST'23]). The semi-to-full transition was driven by the observation that centralized metadata servers waste CPU resources and bottleneck the system.

The CXL era is, so far, repeating only the first half of that progression. PolarCXLMem [Yang SIGMOD-C'25] places database buffer pages directly on a CXL switch fabric, but inherits the buffer-fusion server architecture from its RDMA predecessor PolarDB-MP [Yang SIGMOD-C'24]. The fusion server is still there; the fabric underneath it changed. No CXL-based, fully-disaggregated, peer-only KV store has been built. **FUSEE-CXL fills that gap.**

The argument of this document is twofold:

1. **Architectural placement.** FUSEE-CXL is the CXL counterpart of FUSEE's removal of the metadata server. It is the *full-disaggregation* cell in a 2×2 matrix of {RDMA, CXL} × {semi-disagg, full-disagg} that no prior work occupies.

2. **A non-trivial design problem CXL forces on us.** Unlike RDMA, CXL 2.0 provides **no cross-host hardware cache coherence**. The RDMA family solved cross-host atomicity by riding on RNIC-level CAS/FAA primitives that the underlying PCIe coherence domain makes visible to every CPU; CXL 2.0's Multi-Logical Device (MLD) model gives each host an isolated view, with no cross-host snoop. FUSEE-CXL therefore cannot port FUSEE's RDMA-CAS-based replication protocol. We need a new design.

Our answer is a pair of coupled choices: **static key-range partition** for write coherence (challenge C4 below) and a **software directory with targeted invalidation** for tiered cache coherence (challenge C5 below). Both choices ride on the same CXL-resident message ring, giving us one mechanism with two routing axes. The choices are coupled in a useful way: static partition restores a single-host coherence domain on the owner side, which makes the software-directory's job collapse from "rebuild full cross-host coherence" to "horizontal invalidation only."

The remainder of this document walks through the lineage placement (§2), the CXL hardware property gap (§3), the five-challenge framing of the design space (§4), why prior work doesn't close the gap (§5), the C4 and C5 choice trees (§6, §7), and the resulting architecture together with the performance-data lineage that anchors the design (§8). §9 catalogs open questions for the paper write-up.

---

## 2. Lineage: Two Parallel Disaggregation Stories

### 2.1 RDMA era: Clover → FUSEE

Clover [Tsai ATC'20] was an early KV store on disaggregated memory. It stored KV pairs in the memory pool but retained a *monolithic metadata server* that managed the index and memory-allocation state. The FUSEE paper documents the cost of this design directly: "at least six additional cores have to be assigned [to the metadata server] until the metadata server is no longer the performance bottleneck" [FUSEE §2, Fig. 2]. Clover is thus *semi-disaggregated*: the data path is disaggregated, the control path is not.

FUSEE [Shen FAST'23] removes the metadata server. Clients manage the replicated hash index and memory allocation themselves, using one-sided RDMA verbs (READ, WRITE, and atomic CAS/FAA) on memory-node DRAM. Linearizability of the replicated index is achieved via the SNAPSHOT replication protocol, which leverages `RDMA_CAS` on slot replicas to resolve write conflicts collaboratively among clients. FUSEE is *fully-disaggregated*: there is no centralized control component.

The pivot from semi to full required a *replacement* of the centralized control function with a peer-only protocol. FUSEE's substitute — RDMA-CAS-based slot replication — works because the underlying RDMA fabric provides cross-host atomicity at the hardware level.

### 2.2 CXL era: PolarDB-MP → PolarCXLMem → (missing cell)

PolarDB-MP [Yang SIGMOD-C'24] is a multi-primary OLTP database on RDMA-based disaggregated shared memory. Its core component, the Polar Multi-Primary Fusion Server (PMFS), provides transaction fusion, buffer fusion, and lock fusion — i.e., a centralized control plane for the disaggregated buffer pool.

PolarCXLMem [Yang SIGMOD-C'25] migrates PolarDB-MP's design to a CXL 2.0 switch fabric. Buffer pages move from RDMA-accessed remote memory to CXL devdax, accessed via native load/store. The paper reports up to 2.1× pooling throughput and 1.55× sharing throughput improvements over the RDMA baseline. *Critically, however, PolarCXLMem retains the buffer fusion server*: "Similar to PolarDB-MP, we employ a buffer fusion server to manage the metadata of the distributed buffer pool" [PolarCXLMem §3.3].

The CXL era thus mirrors the RDMA-era starting position: semi-disaggregated, with the fusion server as the control plane. No corresponding *full* disaggregation step has been taken. FUSEE-CXL completes the matrix:

| | **RDMA fabric** | **CXL Type 3 fabric** |
|---|---|---|
| **Semi-disagg** (centralized metadata) | Clover, PolarDB-MP | PolarCXLMem |
| **Full-disagg** (peer-only) | FUSEE | **FUSEE-CXL (this work)** |

PolarDB-MP is shown in the semi/RDMA cell because PolarCXLMem inherits its architecture directly — the lineage from PolarDB-MP to PolarCXLMem is the CXL-side analog of Clover-to-FUSEE *if* the latter had not taken the full-disaggregation step.

---

## 3. The CXL Hardware Property Gap

The CXL-era progression we just described is not just a fabric swap. CXL Type 3 has fundamentally different properties from RDMA, and a naive port of FUSEE's design to CXL would fail because FUSEE depends on a cross-host hardware primitive that CXL does not provide.

### 3.1 Latency profile

CXL Type 3 latency sits between DRAM and RDMA. PolarCXLMem reports the following on an XConn CXL 2.0 switch [PolarCXLMem §2.3, Table 1+2]:

| | DRAM | CXL w/o switch | CXL w/ switch | RDMA |
|---|---|---|---|---|
| Local load latency (ns) | 146 | 265 | **549** | n/a |
| 64 B read latency (µs) | n/a | n/a | **0.78** | 4.55 |

Our own measurements on the g3/g4 testbed (iter-15A, iter-16A) yield ~630 ns for a local CXL load, consistent with PolarCXLMem's 549 ns plus a small NUMA hop. At 64 B granularity, CXL is ~6× faster than RDMA; DirectCXL [Gouk USENIX'22] reports an 8.3× factor under different conditions. This latency profile motivates a design that uses CXL as the bulk-memory tier *and* as a low-latency message transport, but still keeps a host-local DRAM cache for the hottest keys.

### 3.2 Multi-Logical Devices and the cross-host coherence gap

CXL 2.0 supports memory pooling via the Multi-Logical Device (MLD) model: a single physical CXL memory device exposes multiple Logical Devices, each of which can be bound to one host at a time. From any given host's perspective, its local CXL view participates in that host's PCIe.cache coherence domain — load/store from the CPU sees the same byte view as the CPU cache, with normal MESI semantics on the host side.

What MLD does *not* provide is cross-host coherence. When host A writes a cacheline on CXL, host B's CPU cache holding the same line is not snooped. PolarCXLMem states this directly: *"Since the CXL 2.0 switch lacks inherent cache coherency, a node updating a data page in CXL memory cannot invalidate the same page in another node's CPU cache"* [PolarCXLMem §3.3].

This is the central architectural property FUSEE-CXL has to design around.

### 3.3 RDMA gave cross-host atomicity for free; CXL does not

RDMA's verb-level atomic operations (CAS, FAA) execute at the target HCA, serialized by the HCA's atomic unit, and propagate to the target host's CPU caches through the PCIe coherence domain. From a programmer's perspective, an `RDMA_CAS` from host A on a memory-node DRAM location is observed by host B's CPU as atomic, ordered, and globally visible — without any software protocol on the host CPUs. This is the primitive FUSEE's SNAPSHOT protocol depends on, and it has no CXL 2.0 analog.

CXL 2.0 does include a small set of atomic opcodes (e.g., CAS via the M2S protocol), but commercial switches do not, in general, support atomic forwarding from a host to a remote-host-attached cache. (We have not exhaustively confirmed XConn's behavior here; this is one item flagged for verification in §9.) Even if such forwarding existed, it would address only single-cacheline atomic operations, not the broader question of cache coherence between hosts.

### 3.4 CXL 3.0 is not (yet) the answer

The CXL 3.0 specification introduces hardware cross-host coherence (host-attached fabric coherency). However, no commercial CXL 3.0 hardware was available at the time of this work, and PolarCXLMem (which has the most recent CXL deployment we know of) is still CXL 2.0. A KV-store design targeting *currently deployable* CXL fabrics must assume the CXL 2.0 property set.

---

## 4. The Five Design Challenges

The CXL fabric imposes a five-part design space on any disaggregated KV-store work. We split the five challenges into two groups: *opportunities* the fabric enables, and *obligations* the fabric forces on us.

### 4.1 Opportunities (C1, C2, C3)

- **C1 — CXL as cheap bulk storage.** Place the KV value bytes on CXL devdax, exploiting per-device capacities much larger than DRAM at a lower cost per byte. (DRAM is reported as ~50% of Azure server cost and ~40% of Meta rack cost [PolarCXLMem §1].)

- **C2 — CXL as a message bus.** Replace RDMA-based RPCs between hosts with CXL-resident message rings, accessed via load/store. This removes NIC doorbell contention and queue-pair scaling limits, both of which have been identified as RDMA bottlenecks at high concurrency [PolarCXLMem §2.2].

- **C3 — DRAM is still faster.** A few-hundred-ns CXL load is much faster than a 4 µs RDMA round trip, but it is still ~4× slower than a local DRAM load. For workloads dominated by read traffic on a hot key set, a host-local DRAM read cache amortizes the CXL-tier latency.

### 4.2 Obligations (C4, C5)

- **C4 — Write coherence without hardware coherence.** When N hosts attempt to modify the same byte on CXL, the system must guarantee atomicity, visibility, and ordering. CXL 2.0 provides none of this at the fabric level.

- **C5 — Tiered coherence without hardware coherence.** When each host maintains a local DRAM cache (C3) on top of a shared CXL data tier, the system must keep the per-host DRAM caches consistent with each other *and* with the CXL data tier. Again, CXL 2.0 provides no fabric support.

C4 and C5 are not opportunities; they are *unavoidable* if a CXL-based, multi-host KV store is to behave correctly. They are also the main novel contribution area for FUSEE-CXL.

### 4.3 The coupling between C4 and C5

C4 and C5 cannot be designed independently. How writes are routed and serialized (C4) determines what stale states a DRAM cache can hold (C5). For example, a software-lock-based C4 solution would require cache flushes on every lock release, dominating the C5 cost. A static-partition C4 solution restores per-host cache consistency on the owner side for free, making the C5 problem much smaller.

§§6-7 walk through the C4 and C5 option trees with this coupling made explicit.

---

## 5. Why Prior Work Does Not Close the Gap

### 5.1 FUSEE: built on RDMA's hardware atomicity

FUSEE addresses none of C1, C2, C3, or C5 in a way usable on a CXL fabric:

- **C1, C2:** N/A — FUSEE targets RDMA, not CXL.
- **C3:** Explicitly avoided. The FUSEE paper states the design assumption that "the memory pool generally lacks the compute power to manage data and metadata" [FUSEE §2.1]. The disaggregated-memory model FUSEE was built for assumes a thin compute layer on memory nodes; client-side caching was not pursued. RACE hashing is one-sided RDMA reads on the index and `RDMA_CAS` on slot updates — all riding on RDMA hardware primitives.
- **C5:** N/A — there is no DRAM cache to coordinate.
- **C4:** Partially addressed via the SNAPSHOT replication protocol, which leverages `RDMA_CAS` and a last-writer-wins conflict-resolution scheme to achieve linearizability over replicated index slots [FUSEE §4.3]. This solution depends on cross-host hardware CAS, which CXL does not provide. The protocol does not port.

### 5.2 PolarCXLMem: still server-centric

PolarCXLMem addresses C1 and C2 correctly: buffer pages live on CXL devdax, accessed via native load/store. The remaining challenges are handled in ways we cannot reuse for a peer-only KV store:

- **C3:** PolarCXLMem argues *against* tiered DRAM caching, reporting that a CXL-resident buffer pool (CXL-BP) is within 7% of a DRAM-resident buffer pool (DRAM-BP) on Sysbench point-select [PolarCXLMem §2.3, Fig. 3]. This is correct for OLTP page caching, where a single access reads or modifies hundreds of bytes within a 16 KB page and the per-page latency floor is amortized over many byte accesses. It is *not* correct for fine-grained KV. A KV value can be 8 to 1024 bytes, and a single CXL load constitutes a much larger fraction of the per-operation critical path. The "no tiered cache" conclusion does not transfer to our setting.

- **C4:** Distributed page locks held through the buffer fusion server. Writers acquire a page write-lock from the BFS, perform `clflush` on the page, and release the lock; the BFS then propagates an `invalid` flag to other nodes [PolarCXLMem §3.3]. The protocol is correct, but the BFS is a centralized control component — the very thing FUSEE-CXL is trying to remove.

- **C5:** The same BFS maintains per-page sharer metadata (the `invalid` and `removal` flags), and propagates state changes to each node. Again, correct, and again server-centric.

The Polar lineage never escapes the central fusion server. A KV-store design that aspires to full disaggregation cannot adopt either the C4 or C5 mechanism wholesale.

### 5.3 The key insight: the single-host coherence domain is intact

The architectural gap PolarCXLMem leaves open is, in retrospect, also the *leverage point* a full-disaggregation design can exploit. Even on CXL 2.0, the *single-host* coherence domain — one host's CPU caches plus its local view of the CXL fabric, governed by PCIe.cache — is intact. CXL 2.0 only removes coherence *between* hosts, not *within* a host.

This observation is the foundation for the choices in §§6-7. If a design can confine each piece of mutable state to one single-host coherence domain at any moment, the problem downgrades from "rebuild a full cross-host coherence protocol" to "route messages between domains." The former is the hard problem the Polar lineage answers with a centralized server; the latter has cheap, scalable solutions.

We have empirical evidence that the single-host domain *is* exploitable in practice. In [iter-19A](iters/iter19A_summary_v3_final.md), removing an owner-self `clflushopt+sfence` from the write path — i.e., trusting host-internal MESI to make the owner's write visible to the owner's own subsequent reads, without an explicit flush — yielded a 5.9× throughput improvement at zipf-1.5. The flush had been a defensive measure inherited from earlier write-path drafts; the iter-19A B-H3 fix proved it was redundant. Owner-side writes and reads share a single-host coherence domain; MESI does the work for free.

---

## 6. Choosing C4: Static Key-Range Partition

We considered three candidate mechanisms for write coherence on a no-HW-coherence fabric.

### 6.1 Option (a): software locks on CXL

A per-object or per-bucket spinlock placed in CXL memory. Each acquire must read the lock state through the CXL-tier coherence boundary (or via an explicit `clflush` to bypass stale CPU cache), and each release must `clflush+sfence` the new lock state so peer hosts see it.

We measured the cost of a single `clflushopt+sfence` on the g3/g4 testbed in [iter-16A](iters/iter16A_summary_20260521.md) at approximately 66 ns. A single acquire-release pair would therefore cost on the order of ~120-130 ns of unavoidable cache management, before any actual work happens. For a fine-grained KV operation whose total critical path is on the order of a few hundred nanoseconds, this is a ceiling effect on the throughput, not an overhead that can be amortized away.

The option is rejected on per-operation cost.

### 6.2 Option (b): static key-range partition (chosen)

Each key K has a fixed *owner host*, determined by a hash of K. All writes for K execute on the owner. Non-owner hosts forward the write to the owner via a CXL-resident message ring.

The crucial property of this design is that the owner-side write stays inside *one single-host coherence domain*. The owner's CPU performs the write to CXL via normal store instructions, and the owner's subsequent reads of the same line are served by the owner's CPU cache through MESI — zero cross-host atomic primitives are needed. PolarCXLMem's BFS-mediated lock + flush + invalidate sequence simplifies, on the owner side, to a single in-cache store.

The cost shifts to the forwarding path: non-owner hosts pay a message-ring round trip to reach the owner. This is one CXL message ring round trip plus owner-side enqueue/dequeue, which the iter-16A microbenchmarks measured at a single-flow floor near 5 µs. The throughput numbers in §8 demonstrate that this cost is well-amortized at production thread counts.

Side benefit: the same CXL message ring used for write forwarding can carry invalidation messages for C5. We get a single mechanism with two routing axes.

### 6.3 Option (c): append-only MVCC

Writers append a new version of the value with a monotonic sequence number; readers traverse the version chain to find the latest visible version. This sidesteps coherence by never overwriting in place.

Two problems disqualify it for our setting. First, CXL capacity is bulk but not infinite; an append-only KV store would require background garbage collection, which is itself a coherence problem (when is a version safe to reclaim?). Second, monotonic version numbers across hosts require a *global* sequence authority, which reintroduces the centralized server FUSEE-CXL is trying to remove.

### 6.4 Empirical anchoring

The static-partition choice has been progressively wired up and measured:

- [iter-2A-rev](iters/) — first cut: one sender thread, one receiver thread per host. Workload-A peak throughput 1.27 Mops/s, a 2.4× gain over the iter-1A baseline (0.54 Mops/s) that lacked partitioning.
- [iter-3A](iters/) — K-channel sender/receiver: 4.01 Mops/s peak, a further 3.2× gain.
- [iter-17A](iters/iter17A_summary.md) — multi-receiver fan-out across 8 groups: YCSB-C cluster throughput of 27 Mops/s.
- [iter-19A](iters/iter19A_summary_v3_final.md) — removed an over-conservative owner-self flush (B-H3): a single-cell 5.9× gain at zipf-1.5. This is the strongest direct evidence that owner-side writes need no cross-host coherence machinery.

The combined progression confirms that static partition is not only correct but is the lever that lets throughput grow with thread count and host count.

---

## 7. Choosing C5: Software Directory with Targeted Invalidation

We considered two candidate mechanisms for tiered cache coherence on a no-HW-coherence fabric. The choice is, again, shaped by what C4 has already decided.

### 7.1 Option (a): software directory + targeted invalidation (chosen)

For each key, a small *directory entry* in CXL memory records the current set of sharer hosts — the hosts whose DRAM cache may hold a copy of the value. When the owner host writes a new value, it consults the directory and sends an `INV` message via the CXL message ring to each sharer host's receiver. Sharers process the INV by evicting the corresponding entry from their DRAM cache; the next read on the sharer side then refetches from CXL.

The protocol achieves *linearizability*: the write's linearization point is the moment all relevant invalidations are acknowledged (in our implementation, `ack_wait` on the writer side), after which no sharer can serve a stale read. The same correctness standard FUSEE adopts [FUSEE §3.1] is preserved here.

### 7.2 Option (b): lazy release + epoch invalidation

A weaker alternative: caches refresh at epoch boundaries (e.g., transaction commit, or periodic epoch advances), and writes do not actively invalidate sharers. This is cheaper per write but exposes release-consistent semantics to the application.

Release consistency requires the application to insert memory barriers at every synchronization point. A hash-table API does not have a synchronization-point abstraction; from the application's perspective, a `GET(K)` after a `PUT(K)` from another host should return the new value, period. Lazy release would break this expectation. We reject it on semantics.

### 7.3 How C4 collapses C5 to horizontal-only

Here the C4 choice pays off. Under static partition, the *vertical* coherence relationship — between an owner host's DRAM cache and the owner host's CXL writes — is already maintained for free by host-internal MESI (this is the leverage from §5.3, and the iter-19A B-H3 evidence). The software directory does not need to handle vertical coherence.

What remains is the *horizontal* relationship: invalidating non-owner hosts' DRAM caches when the owner has written. In a 2-host or 4-host CXL deployment, the per-write fanout of horizontal invalidations is at most N-1; the directory size is bounded; the INV ring throughput is the only first-order scalability question, and is the one [iter-17A](iters/iter17A_summary.md)'s multi-receiver fan-out was designed to address.

The combined design — C4 = static partition, C5 = software directory + INV — is therefore *minimal*: each mechanism does only what cannot be done by the other or by hardware MESI. There is no stacked coherence subsystem. One CXL message ring carries both write-forwarding traffic (axis 1) and invalidation traffic (axis 2).

---

## 8. Putting It Together

### 8.1 One ring, two routing axes

The end-to-end data path for a write originating on a non-owner host:

```
Host A worker  ─── (CXL msg ring, axis 1: write forwarding) ───>  Host B receiver
                                                                        │
                                                              Host B owner-self write
                                                              (host-internal MESI)
                                                                        │
                                                              consult directory entry
                                                                        │
                                       (CXL InvalRing, axis 2: invalidation) ───>  Host A,C,D sharers
                                                                        │
                                                              sharers evict DRAM cache entry
                                                                        │
                                                              ack back to Host B
                                                                        │
                                                              Host B's worker returns to Host A
```

The owner-self write (Host B's local store to CXL) costs no cross-host primitive. The two cross-host traffic flows — write forwarding and INV — both ride the CXL message ring, with the InvalRing as a logically separate sub-channel. PolarCXLMem's BFS is not present; there is no centralized control component.

### 8.2 Performance lineage anchoring the design

Each iter in the development progression contributes a specific piece of evidence to the design argument above. The table below maps headline numbers to the design choices they confirm or refine.

| Iter | Headline | Role in the argument |
|---|---|---|
| [iter-1A](iters/) | YCSB-A peak 0.54 Mops/s | Baseline before partitioning; surfaces S3 broadcast + S4 ack_wait as 74% of latency at low T. |
| [iter-2A-rev](iters/) | YCSB-A peak 1.27 Mops/s | Static partition first wired (1 sender / 1 receiver per host). 2.4× over iter-1A confirms partitioning works directionally. |
| [iter-3A](iters/) | YCSB-A peak 4.01 Mops/s | K-channel sender/receiver. Per-slot LFM ported from Protocol C cuts S1 lock contention 3-5× at high T. YCSB-C cache=off T=82 K=2: 35.13 Mops/s ✓ (≥ 20 Mops/s bar). |
| [iter-16A](iters/iter16A_summary_20260521.md) | Stage-decomposition framework | 15 probes, 11 latencies, 8 stages. Identifies stage 5 (ack_wait for INV completion) as 72-99% of write-path latency at single receiver. Sets the direction for iter-17A. |
| [iter-17A](iters/iter17A_summary.md) | YCSB-C cluster 27 Mops/s | Multi-receiver fan-out across 8 groups; 11× cluster scaling over single-receiver baseline. Confirms the INV ring is the right place to scale C5 horizontally. |
| [iter-18A](iters/iter18A_summary.md) | YCSB-C cluster 53.9 Mops/s (2.7× target) | xhost_read path optimization (C3 pause removal, single-flush). Confirms the read path scales independently of the C4/C5 mechanism. |
| [iter-19A](iters/iter19A_summary_v3_final.md) | B-H3: owner-self flush removal → 5.9× single-cell gain at zipf-1.5 | Direct empirical confirmation that owner-side vertical coherence is free under static partition (§5.3, §7.3 punchline). |
| iter-20A (in flight) | Cleanup: -700 LOC; YCSB-C cluster 61 Mops/s | Removed STAGING + RCU + BATCHED + TLS dead paths. Confirms the C4 + C5 design is *minimal-sufficient*: removing the alternatives we considered does not regress correctness or throughput. |

This is not a clean linear progression — several iters revisited prior assumptions (iter-19A retracted overclaims from iter-15A; iter-20A removed code paths that iter-5 through iter-11 had introduced). The progression is, instead, a series of *increasingly sharp* empirical statements about which parts of the C4/C5 design are load-bearing and which are not.

### 8.3 Failure boundary (design promise, not yet measured)

Under FUSEE-CXL's design, any single host can crash without losing CXL-resident data — KV bytes, directory entries, and message-ring metadata all survive the crash because they reside on CXL devdax under independent power. Recovery is per-shard: the surviving host(s) take over the failed host's owner shard, re-construct the in-flight INV state from CXL directory entries, and resume.

This is a design promise, not an empirically tested claim. The last fault-injection sweep was in [iter-4A](iters/), well before the C4/C5 architecture stabilized. A modern fault-injection sweep is queued for a future iter; it is not part of the work documented above.

---

## 9. Open Questions for Future Paper Drafting

Items the present argument depends on but has not fully verified. Each should be confirmed before paper submission; some may motivate additional iters.

1. **CXL HW CAS support.** §3.3 claims commercial CXL 2.0 switches do not forward host-issued atomic opcodes to remote-host-attached caches. We have not exhaustively verified this on the XConn XC50256. If atomic forwarding is supported in some restricted form, the C4 option tree may need a fourth branch.

2. **CXL version timeline.** §3 mentions CXL 1.0 / 2.0 / 3.0 in passing but does not lay out the version-to-feature mapping (single-host attach → switch + pooling → native coherence). A short timeline figure may be worth adding for advisors and reviewers unfamiliar with CXL.

3. **Clover citation venue.** §2.1 cites Clover as [Tsai ATC'20]. The exact venue (USENIX ATC 2020 vs USENIX OSDI 2020) and title of the canonical Clover paper should be confirmed against the FUSEE bibliography before paper submission.

4. **Multi-host scale.** §7.3 argues that horizontal INV fanout is bounded by N-1 hosts. Current empirical evidence is from 2-host (g1/g2 or g3/g4) deployments only. Scale to ≥ 4 hosts is a design assumption; ≥ 8 hosts has not been verified at all. If the paper claims general scalability, additional sweeps are needed.

5. **Failure-injection.** §8.3 states the failure boundary as a design promise. A modern fault-injection sweep would substantially strengthen the paper.

6. **Comparison to PolarCXLMem in absolute terms.** The performance lineage in §8.2 anchors design choices but does not directly compare FUSEE-CXL to PolarCXLMem. A like-for-like benchmark (same hardware, same workload) is the cleanest paper artifact for the experimental section.

7. **Rebuttal of "DRAM cache unnecessary" for KV.** §5.2 contains a brief KV-vs-OLTP argument against PolarCXLMem's no-tiered-cache conclusion, but the argument is mostly analytical (KV value size vs DB page size). An empirical measurement on the same hardware — KV throughput with and without the DRAM read cache — would make the rebuttal much harder to push back on.

---

## References

- **[Shen FAST'23]** Shen et al., *FUSEE: A Fully Memory-Disaggregated Key-Value Store (Extended Version).* USENIX FAST 2023. [PDF](../documents/fast23_FUSEE_Extended_Version.pdf).
- **[Yang SIGMOD-C'24]** Yang et al., *PolarDB-MP: A Multi-Primary Cloud-Native Database via Disaggregated Shared Memory.* SIGMOD-Companion 2024. [PDF](../documents/Yang%20et%20al.%20-%202024%20-%20PolarDB-MP%20A%20Multi-Primary%20Cloud-Native%20Database%20via%20Disaggregated%20Shared%20Memory.pdf).
- **[Yang SIGMOD-C'25]** Yang et al., *Unlocking the Potential of CXL for Disaggregated Memory in Cloud-Native Databases.* SIGMOD-Companion 2025. [PDF](../documents/Yang%20et%20al.%20-%202025%20-%20Unlocking%20the%20Potential%20of%20CXL%20for%20Disaggregated%20Memory%20in%20Cloud-Native%20Databases.pdf).
- **[Tsai ATC'20]** Tsai et al., *Disaggregating Persistent Memory and Controlling Them Remotely.* USENIX ATC 2020. (Clover; citation venue to verify, see §9 #3.)
- **[Gouk USENIX'22]** Gouk et al., *DirectCXL: Direct Access for CXL Disaggregated Memory.* USENIX ATC 2022. (Cited via PolarCXLMem.)

---

## Iter document references

The iter summaries cited in this document, in numerical order:

- [iter-15A summary](iters/iter15A_summary_20260520.md) — 4-path microbench baseline; primitive bench numbers used for CXL latency calibration.
- [iter-16A summary](iters/iter16A_summary_20260521.md) — stage decomposition framework; ack_wait dominance identification; clflushopt+sfence cost measurement.
- [iter-17A summary](iters/iter17A_summary.md) — multi-receiver fan-out scaling.
- [iter-17A xhost write path opt](iters/iter17A_xhost_write_path_opt_summary.md) — xhost write path optimization details.
- [iter-18A summary](iters/iter18A_summary.md) — xhost read path opt, YCSB-C cluster 53.9 Mops/s.
- [iter-19A summary (v3 final)](iters/iter19A_summary_v3_final.md) — Anomaly A + B RCA, B-H3 owner-self flush removal (5.9× single-cell gain), 4-path flush+fence audit.
- [iter-19A 4-path stage decomposition](iters/iter19A_4path_stage_decomposition.md) — consolidated reference for all four CXL paths and their stage breakdowns.
- [iter-20A plan](iters/iter20A_plan.md) — STAGING + RCU + BATCHED + TLS cleanup.
