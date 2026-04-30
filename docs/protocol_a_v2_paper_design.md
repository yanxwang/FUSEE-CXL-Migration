# Design

> Overleaf-ready content. 严格 academic prose; 不堆砌; 每个 design choice
> 给出明确 rationale + tradeoff. `\section`/`\subsection` 标记可以直接
> 转 LaTeX. 数字 (e.g. 4.5×) 是 g3+g4 testbed 实测/外推, 引用
> `docs/design_goals.md` Hardware baseline section.

---

\section{Design}

\subsection{System Model and Assumptions}

We target a system with $H$ unified compute nodes (each running both
client logic and KV-store services), all sharing a CXL Type-3 memory
expander connected through a PCIe switch. We assume:

\textbf{(A1) No cross-host cache coherence on the shared device.} CXL
Type-3 lacks a global coherence domain spanning multiple hosts. A
write performed by host $A$'s CPU does not automatically invalidate
host $B$'s cached copy of the same physical line; visibility must be
established through explicit cache-line management instructions
(\texttt{clflushopt}) and store fences (\texttt{sfence}). Likewise,
no cross-host atomic primitive is available: a \texttt{lock cmpxchg}
instruction is hardware-atomic within a single host's coherence
domain but provides no guarantees against a concurrent
\texttt{lock cmpxchg} from another host on the same physical address.

\textbf{(A2) CXL bandwidth is the binding throughput constraint.} On
the testbed (Intel Xeon, 2 hosts), the CXL write bandwidth ceiling
under user-space \texttt{movnt} streaming is approximately
$12.5~\text{GB/s}$ per host, with idle load latency
$\approx 600~\text{ns}$. By contrast, local DRAM offers
$\approx 134~\text{ns}$ idle latency and $\approx 392~\text{GB/s}$
read bandwidth---a $4.5\times$ latency advantage and $7.6\times$
bandwidth advantage over CXL.

\textbf{(A3) Strict linearizability is required.} A write must become
visible to every host's subsequent read in real time after the write
returns success. Operations to the same key must appear in a single
total order across the cluster.

These assumptions force three architectural decisions. First, since
no cross-host hardware atomic exists, software protocols on
\texttt{load}/\texttt{store}/\texttt{clflushopt}/\texttt{sfence}
substitute for the hardware CAS that prior RDMA-based systems rely
on~\cite{fusee}. Second, since CXL latency is over $4\times$ local
DRAM, locality matters: any read served from local DRAM is roughly
$5\times$ faster than the equivalent read forced through the CXL
fabric. Third, since CXL bandwidth is finite at $\sim 12.5~\text{GB/s}$
per host, write-path byte amplification (e.g., redundant cache-line
flushes, broadcast invalidations) directly caps achievable throughput.

\subsection{Architecture Overview}

We adopt three guiding principles, motivated directly by (A1)-(A3):

\textbf{(P1) CXL is the authoritative store; DRAM is a software-managed
cache.} The hashtable index and all variable-length value blocks
reside in shared CXL memory as the single source of truth. Each host's
DRAM holds a partial replica of recently-accessed entries, treated
strictly as a coherence-managed cache: hits are served at DRAM speed
($\sim 50~\text{ns}$); misses pay one cross-host round-trip plus a
CXL fetch ($\sim 2{-}3~\text{µs}$).

\textbf{(P2) Static key-space partitioning routes every write to a
unique owner host.} A sharding function $\sigma: K \to H$ assigns
each key to exactly one host. All updates to a key are executed on
its owner host; clients on other hosts that issue a write to a
non-owned key forward the operation through a one-way message channel.
The owner host serializes concurrent writes to the same key with a
host-local spinlock and commits the new value to CXL with a
single-host atomic compare-and-swap. \emph{Cross-host mutual exclusion
between writers is therefore eliminated by construction:} no writer
on host $B$ ever competes with a writer on host $A$ for the same key.

\textbf{(P3) Inter-host communication is aggregated into a fixed
$N{:}1{:}1{:}N$ topology.} Workers within a host first feed messages
to a per-host \emph{sender} thread (an $N{:}1$ aggregation through a
DRAM MPSC queue); the sender thread is the sole producer on a
per-(src, dst) SPSC ring in CXL memory; the receiving host's
\emph{receiver} thread is the sole consumer. The receiver dispatches
each message to local workers within the destination host (a
$1{:}N$ fan-out via DRAM). This avoids the $O(N^2)$ traffic that
arises if every client communicates directly with every remote
client; the cost of the design is two extra hand-offs per round-trip,
which we measure empirically at $\approx 1~\text{µs}$ each.

Figure~\ref{fig:arch} illustrates the resulting layered system: CXL
memory holds the hashtable, KV blocks (immutable; new blocks
allocated on every update), per-(src, dst) SPSC rings, and acknowledgment
channels. Each host's DRAM holds a sharding lookup table, a per-slot
directory tracking which hosts cache each entry, and a value cache
mapped \texttt{MAP\_SHARED} across all worker processes on the same
host. The protocol thereby trades a slightly more elaborate software
stack for the elimination of three classes of overhead: cross-host
cache coherence (which CXL Type-3 does not provide), cross-host
synchronization (eliminated by sharding), and broadcast invalidation
($O(N)$ traffic per write reduced to $O(\text{sharers})$).

\subsection{Memory Layout}

\textbf{CXL memory.} The shared region contains:
(i)~the hashtable, a fixed array of $B$ buckets, each holding $S=7$
slots, where each slot stores a key, a size class identifier, an
owner-host tag, and a pointer to a value block;
(ii)~the KV blockpool, partitioned into $H$ disjoint segments (one
per owner host) with size-class-aware sub-allocation; only the
owner host allocates from its own segment;
(iii)~the SPSC rings, one per (source host, destination host, channel)
triple, each carrying fixed-size 64-byte messages (invalidation,
registration, eviction, write-forward, response);
(iv)~the acknowledgment channels, mirroring the rings;
(v)~per-host forward staging buffers, each owned by its forwarder
host, used as a short-lived cross-host transfer area for value
bytes carried by write-forward messages.

Although CXL bytes are accessed across hosts, the only structures
read or written by multiple hosts on the same physical addresses
are the SPSC rings (with non-conflicting single-producer
single-consumer semantics) and the hashtable slot pointers (with
single-host-writer guaranteed by sharding). All other CXL
state---KV blocks, ring entries, ack counters, staging
buffers---is touched by at most one host on each address.

\textbf{Per-host DRAM.} Each host allocates four
\texttt{MAP\_SHARED} regions visible to all of its worker processes:
(i)~a sharding lookup table (read-only after initialization);
(ii)~a per-slot directory tracking the set of hosts caching each
hashtable slot, the directory state, and a host-local spinlock;
(iii)~the value cache, a hashmap from keys to value-byte buffers
with a 1-byte ``stale'' flag co-located with each entry;
(iv)~per-slot self-flags indicating whether the local host is
currently registered as a sharer.

\textbf{Aggregation point.} Within each host, an MPSC queue in
\texttt{MAP\_SHARED} DRAM is the sole hand-off between client workers
and the per-host sender thread; entries carry message type, key,
target host, and---for messages requiring large payload---a CXL
pointer into the host's forward staging buffer rather than the bytes
themselves. Inline payload is deliberately not supported: at the
target value sizes ($\geq 256$~B), inline storage cannot fit in the
fixed-size message slot, so a single out-of-band path keeps the
protocol simple.

\subsection{Protocol Mechanics}

\subsubsection{Request routing}

A client receiving an operation on key $k$ first computes
$\sigma(k)$ to determine the owner host. If
$\sigma(k) = \text{self}$, the worker executes the operation
in-place. If $\sigma(k) \neq \text{self}$, the worker enqueues a
forward request to the local sender thread, then spins on a
DRAM acknowledgment slot until the owner returns a response. We use
$\sigma(k) = \text{HighBits}(\text{FNV-1a}(k))$, which costs a
single DRAM load on the read side and trivially partitions the key
space when $H$ is fixed at boot. Consistent hashing is a
straightforward replacement when $H$ becomes dynamic.

The cost of forwarding---$\sim 2{-}3~\text{µs}$ across the SPSC ring
plus a return path---falls in the same envelope as the per-host
mutex acquisition that an unsharded design would otherwise pay
($\sim 1.5{-}2~\text{µs}$ for a Lamport's Fast Mutex acquire on
non-coherent CXL memory under contention) and degrades far more
gracefully under high concurrency: forward queues are sequential
SPSC drains rather than the multi-line cache-coherence pingpong
that plagues mutex-based designs at high thread counts.

\subsubsection{Software cache coherence}

Each per-slot directory entry tracks an MESI-style state plus a
bitmap of hosts caching the slot. We use only the \emph{Shared}
and \emph{Invalid} states; the \emph{Modified} state appears
transiently as the writer's intermediate state during the
write commit sequence below. Reads do not consult the directory;
instead, each host self-tracks its membership in the sharer set
via a local flag set during cache fill and cleared during cache
eviction. The directory thus serves only the writer's purpose
of identifying which hosts must be invalidated.

\textbf{Per-slot vs.\ per-bucket granularity.} We chose per-slot
directory tracking despite the higher metadata cost
($B \times S \times 16~\text{B} = 7~\text{MB/host}$ at $B=2^{16}$,
versus $1~\text{MB/host}$ for per-bucket). Per-bucket tracking
introduces false sharing: an INSERT to slot 3 of bucket $b$ would
invalidate any host caching slot 5 of $b$, even though those
slots are independent. Under YCSB-style uniform key access, this
amplifies invalidation traffic by $\approx S = 7\times$. The 7~MB
storage cost is comparatively trivial.

\subsubsection{Read path}

A read first checks the local cache (DRAM hashmap lookup, $\sim 50~\text{ns}$);
if the entry is present and its stale flag is unset, the value bytes
are returned directly. The fast path performs no atomic load on
shared state, no fence, and no cache-line management instruction:
the absence of those operations is precisely why the path achieves
DRAM-class latency.

On cache miss, the worker enqueues a registration request to the
owner host through the local sender thread. The owner adds the
requesting host to the slot's sharer bitmap and returns a small
response carrying the slot's current pointer and value size. The
requester then dereferences this CXL pointer through a sequence of
\texttt{clflushopt} instructions followed by an \texttt{mfence} and
plain loads, copying the value bytes from CXL into its local cache.
We deliberately avoid any inline-payload variant in which the
response itself carries value bytes: such a path saves at most one
CXL load per cache miss, but introduces a second code path that
must be reasoned about separately, and provides no benefit at the
value sizes (256~B and above) targeted by our evaluation.

The ordering invariant for cache-fill correctness is that the
requester must be added to the sharer bitmap \emph{before} it
populates its local cache; otherwise a concurrent write completing
between the cache fill and the registration would not invalidate
the new entry, leaving a stale read undetected. We achieve this
ordering trivially: the owner updates the directory under its
spinlock as part of processing the registration request, and the
requester writes its cache only after observing the response.

\subsubsection{Write path}

Writes are always executed on the key's owner host. The owner-side
worker:
(1)~acquires the host-local spinlock on the slot's directory entry;
(2)~allocates a new value block from its blockpool segment;
(3)~writes the value bytes to the new block on CXL, using
non-temporal stores plus \texttt{sfence} for blocks $\geq 256~\text{B}$
and standard stores plus per-line \texttt{clflushopt} plus
\texttt{sfence} for smaller blocks (rationale below);
(4)~enqueues an invalidation message to each host in the
sharer bitmap (excluding self) through the sender thread, and
spins on a DRAM acknowledgment slot until all invalidations have
been confirmed by the receiving hosts;
(5)~atomically updates the slot's pointer field on CXL via
\texttt{lock cmpxchg}, followed by \texttt{clflushopt} and
\texttt{sfence};
(6)~updates the directory state to reflect that only the writer
host now caches the slot, and releases the spinlock;
(7)~updates the local cache;
(8)~enqueues the now-orphaned old block onto a lazy garbage-collection
queue.

Step 4 is the strict-linearizability barrier: by the time the writer
proceeds past step 4, all hosts that previously cached the slot have
removed it from their caches. Step 5 then makes the new value visible
to any host that subsequently re-fetches.

The single-host atomicity of \texttt{lock cmpxchg} suffices because
sharding guarantees that no two hosts concurrently CAS the same
slot pointer. Were sharding absent, this CAS would have to be
replaced by a software cross-host mutex (e.g., Lamport's Fast
Mutex), at a cost of approximately $1.5{-}2~\text{µs}$ per write
under uncontended conditions and unbounded latency under
contention. Sharding eliminates that cost entirely.

\subsubsection{Storage: copy-on-write blocks}

We allocate a fresh value block for every UPDATE rather than
modifying the existing block in place. This decision aligns with
the prior design of FUSEE~\cite{fusee} as well as standard
practice in persistent and disaggregated KV stores
(e.g., LSM-trees, B$^w$-tree, Bigtable's MVCC). Three reasons drive
this choice over an in-place alternative.

First, value blocks larger than 8~bytes cannot be updated atomically
on CXL: the platform offers no instruction to publish more than 8
contiguous bytes as a single observable unit, and a partial write
interrupted by a crash leaves a torn block. CoW restores
crash-consistency by treating the slot pointer's CAS as the single
commit point, ensuring readers always observe either the old or
new block, never a partially-written one.

Second, concurrent readers proceeding through a slot pointer
inherently observe an immutable block---the writer of a CoW system
never mutates a block once any reader could potentially follow a
pointer to it. This frees the read path from any value-byte
synchronization and lets us defer GC until reference safety is
clearly established.

Third, in-place update would require a fallback to allocate-new-block
whenever a new value's size class exceeds the old, producing a
bimodal latency distribution---a regression that reviewer scrutiny
of an in-place design would invariably surface.

The blockpool itself is partitioned across owner hosts: each host
owns one disjoint segment of the CXL blockpool region and manages
its segment's free list in its own DRAM, eliminating cross-host
free-list contention.

\subsection{Cross-Host Communication via $N{:}1{:}1{:}N$ Aggregation}

The protocol has four message types: invalidation, cache-registration,
cache-eviction, and write-forwarding. All four flow through the
same shared infrastructure: a DRAM aggregator queue per host (an
MPSC structure shared by client workers and the per-host sender
thread); a CXL SPSC ring per (source host, destination host,
channel); a CXL acknowledgment channel per ring; and a DRAM
worker-acknowledgment buffer per host.

When a client initiates a request, it enqueues into the local
aggregator queue using a \texttt{fetch\_add} on the queue's tail
counter (a hardware-atomic operation in DRAM). The local sender
thread drains the aggregator and batches up to $K$ messages per
destination before issuing a single batch publish to the destination's
SPSC ring; the writes to the ring are followed by a single
\texttt{sfence} per batch rather than per message, amortizing the
fence cost over $K$ entries. Receivers read entries through
\texttt{clflushopt}-mediated loads, dispatch by message type, apply
the effect to local state, and advance the acknowledgment channel
counter.

The aggregation provides three benefits:
\textbf{(a)}~Within each host, multi-producer client traffic
contends only on a DRAM cache line---hardware-coherent and fast
(\textasciitilde 5~ns)---rather than on a CXL line that would otherwise
suffer cross-host coherence pingpong.
\textbf{(b)}~Per-CXL-batch \texttt{sfence} amortization: a single
fence drains an arbitrary number of preceding writes, rather than
each writer paying its own fence.
\textbf{(c)}~The receiver-side acknowledgment is also batched
(one counter advance per drained batch), reducing the ack-channel
update frequency by the same factor $K$.

The cost is one extra hand-off per direction (worker
$\rightarrow$ sender, receiver $\rightarrow$ worker), measured at
$\approx 1~\text{µs}$ per hand-off. This is acceptable in the
context of a strict-linearizability protocol whose round-trip
latency budget is dominated by the CXL load-store cycle anyway.

\subsection{Synchronization Primitives}

Three classes of synchronization are used in the protocol, each
matched to its memory location:

\textbf{Host-local atomics on shared DRAM.} Directory entries,
the cache hashmap, and the worker-acknowledgment buffer all reside
in \texttt{MAP\_SHARED} DRAM regions visible to every worker on the
same host. These structures are protected by \texttt{pthread\_spinlock\_t}
(with \texttt{PTHREAD\_PROCESS\_SHARED} attribute) or
\texttt{std::atomic} operations. Hardware coherence on the same
host's coherence domain makes these primitives no more expensive
than their thread-only equivalents.

\textbf{Single-host CAS on CXL.} The hashtable slot pointer is
updated via \texttt{\_\_atomic\_compare\_exchange\_n} (compiling to
\texttt{lock cmpxchg}). Although CXL Type-3 does not provide
cross-host atomic semantics, sharding guarantees a single-host
writer per slot, and \texttt{lock cmpxchg} is hardware-atomic
within a single host's coherence domain.

\textbf{Lock-free SPSC rings on CXL.} The cross-host message
infrastructure exploits the single-producer single-consumer
discipline imposed by the architecture: each ring has one writer
(the source host's sender thread) and one reader (the destination
host's receiver thread). Tail and head counters are
\texttt{std::atomic<uint64\_t>} on CXL, with
\texttt{clflushopt}+\texttt{sfence} ordering between the producer's
entry write and tail advance. No mutex, no compare-and-swap, no
cross-host atomic primitive is required.

A consequence of the above is that Lamport's Fast Mutex, which
served as the cross-host critical-section primitive in the
predecessor design, is unused in the normal write path of this
protocol. The mutex implementation is retained as a fallback for
degraded operating modes and for the (frozen) baseline protocol.

\subsection{Design Choices Discussion}

We summarize the four most consequential decisions and their
alternatives.

\textbf{Per-slot vs.\ per-bucket directory tracking.} Per-bucket
tracking would reduce metadata by $S = 7\times$ but multiply
invalidation traffic by the same factor under YCSB-style access
patterns, since a write to one slot would invalidate all hosts
caching any slot of the same bucket. The 7~MB DRAM cost of per-slot
tracking is negligible against the 86 cores per host on our
testbed.

\textbf{Lazy stale flag vs.\ physical eviction on invalidation.}
We mark cache entries as stale rather than removing them. The 1-byte
flag check on the read fast path is co-located on the same cache
line as the value pointer and is therefore essentially free. This
mirrors hardware MESI behavior, in which an invalidated line stays
in the cache until reused, and substantially simplifies the
invalidation path: the receiver thread updates a single byte rather
than performing a hashmap erase.

\textbf{Static sharding vs.\ consistency hashing.} On a fixed-size
testbed (two hosts), static high-bits hashing imposes no cost
beyond a single DRAM load per request and divides the key space
exactly in half. Consistency hashing is a strict generalization
that would let the system tolerate dynamic node membership; we
view this as an orthogonal extension and out of scope for the
current design.

\textbf{Copy-on-write vs.\ in-place updates.} Discussed above
(\S{}5): CoW preserves crash-consistency for blocks larger than
8 bytes, frees the read path of value-byte synchronization, and
matches FUSEE's original design. In-place updates would offer
marginal latency gains for small blocks at the cost of a
two-path architecture and a class of crash-consistency reviewer
attacks we cannot easily counter.
