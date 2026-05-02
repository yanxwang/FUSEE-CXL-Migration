# Design

> Overleaf-ready content. 严格 academic prose; 不堆砌; 每个 design choice
> 给出明确 rationale + tradeoff. `\section`/`\subsection` 标记可以直接
> 转 LaTeX. 数字 (e.g. 4.5×) 是 g3+g4 testbed 实测/外推, 引用
> `docs/design_goals.md` Hardware baseline section.

---

\section{Design}

\subsection{System Model}
\label{sec:model}

We target a cluster of $H$ peer compute nodes sharing a CXL Type-3
memory expander over a PCIe switch. Each node runs application
workers that issue KV operations directly against the shared CXL
store, and owns a static shard of the keyspace.

\paragraph{Hardware constraints.}

\textbf{(C1) No cross-host coherence or atomicity on the shared
device.} CXL Type-3 provides no global coherence domain across
hosts: a write by host~$A$ does not invalidate host~$B$'s cached
copy of the same line, and \texttt{lock}-prefixed atomics hold only
within a single host's coherence domain. Cross-host visibility and
mutual exclusion must therefore be built in software, on top of
\texttt{clflushopt}, \texttt{sfence}, and plain
\texttt{load}/\texttt{store}.

\textbf{(C2) CXL is markedly slower than local DRAM in both latency
and bandwidth} (Table~\ref{tab:hw}). The realistic per-host write
ceiling on devdax under user-space AVX-512 NT-stream with per-4~KiB
\texttt{sfence} is $\sim 12.5~\text{GB/s}$---roughly $4\times$ below
mlc's NT-write peak. We attribute this gap primarily to devdax
exposing the device as a single uninterleaved region (forfeiting
the kernel-managed channel striping that mlc's system-ram
measurement enjoys), with \texttt{sfence} pacing as a secondary
contributor; the two contributions are not independently measured.

\begin{table}[t]
\centering
\small
\begin{tabular}{lrrr}
\toprule
                              & DRAM (node 0) & CXL (node 1) & DRAM / CXL \\
\midrule
Idle load latency             & 134.3~ns      & 604.0~ns     & $4.50\times$ \\
Peak read BW (mlc)            & 391.8~GB/s    & 51.6~GB/s    & $7.60\times$ \\
Peak NT-write BW (mlc)        & 330.2~GB/s    & 54.2~GB/s    & $6.09\times$ \\
Userspace NT-write (devdax)   & ---           & 12.5~GB/s    & --- \\
\bottomrule
\end{tabular}
\caption{Memory ceilings on the g3 + g4 testbed (Intel Xeon, dual
host, shared CXL Type-3 expander). Rows~1--3 measured with Intel
MLC; row~4 measured with our AVX-512 NT-stream microbenchmark.}
\label{tab:hw}
\end{table}

\paragraph{Consistency goal.}
Operations on the same key appear in a single total order across
the cluster, and a write, once acknowledged, is visible to every
subsequent read on every host (\emph{strict linearizability}).



\subsection{Architecture Overview}
\label{sec:arch}

The design rests on three principles, each addressing one of the
constraints in \S{}3.1:

\textbf{(P1) CXL is the authoritative store; DRAM is a
software-managed cache} (addresses C2). The hashtable index and all
value blocks reside in the shared CXL region as the single source of
truth; each host's DRAM holds a partial replica of recently-accessed
entries, treated as a coherence-managed cache. Reads are served
locally on cache hit and otherwise fall through to a CXL fetch. No
host owns an authoritative DRAM replica---``ownership'' is a
write-routing role, not a storage tier.

\textbf{(P2) Static sharding routes every write to a single owner
host} (addresses C1). A sharding function $\sigma : K \to H$ assigns
each key to exactly one owner; all updates to a key execute on its
owner. Clients on non-owner hosts forward writes through a one-way
message channel (P3 below). \emph{Cross-host writer--writer mutual
exclusion is thereby eliminated by construction}: no two hosts ever
contend for the same slot, so the per-slot commit reduces to a
single-host atomic operation that C1 already permits.

\textbf{(P3) Inter-host messages flow through a fixed
$N{:}1{:}1{:}N$ aggregation} (addresses C1, scaling). Workers within
a host first feed messages into a per-host \emph{sender} thread
(an $N{:}1$ DRAM MPSC); the sender is the sole producer on a
per-pair SPSC ring in CXL; the destination's \emph{receiver} thread
is the sole consumer and fans the message out to local workers
($1{:}N$ DRAM). This serves three purposes: it avoids the
$O(H^2 \cdot N^2)$ wiring of all-to-all worker-to-worker channels;
it gives invalidations a single arrival point per host so that one
acknowledgement covers all same-host sharers; and it provides the
natural serialization point at which a writer awaits invalidation
completion before committing (the strict-linearizability barrier of
\S{}3.4.4).

Figure~\ref{fig:arch} shows the resulting layered system.
\S{}3.3 details the on-CXL and on-DRAM data layout; \S{}3.4 walks
through the read and write paths.

\subsection{Memory Layout}
\label{sec:layout}

Memory is split across two physical tiers (Table~\ref{tab:layout}):
a single CXL region shared by all hosts holds the authoritative
data, and per-host DRAM regions---each \texttt{MAP\_SHARED} across
that host's worker processes only---hold the cache and the
coordination metadata.

\begin{table}[t]
\centering
\small
\begin{tabular}{@{}llr@{}}
\toprule
\textbf{Region} & \textbf{Tier} & \textbf{Approx.\ size} \\
\midrule
Hashtable          & CXL  & $B \cdot S \cdot 24$\,B \\
KV blockpool       & CXL  & workload-dependent \\
Forward staging    & CXL  & $\sim$1\,MB per forwarder \\
SPSC rings + acks  & CXL  & $\propto H^2$ \\
\midrule
Sharding table     & DRAM & $H$ entries, read-only \\
Slot directory     & DRAM & $B \cdot S \cdot 16$\,B \\
Local KV cache     & DRAM & LRU-bounded \\
\bottomrule
\end{tabular}
\caption{Physical memory layout. CXL = single region shared by all
hosts; DRAM = per host, \texttt{MAP\_SHARED} across same-host
worker processes.}
\label{tab:layout}
\end{table}

\paragraph{CXL regions.}
\begin{itemize}\setlength{\itemsep}{2pt}
\item \textbf{Hashtable}: $B$ buckets of $S$ slots ($S{=}7$ in our
  implementation; bucket size is a tunable parameter). Each slot
  stores a key, a size-class identifier, and a pointer to a value
  block. Slot pointers are written by the owner host only; a
  naturally aligned 8-byte store followed by
  \texttt{clflushopt}+\texttt{sfence} suffices for atomic
  cross-host publication---no cross-host atomic primitive is
  required, since sharding eliminates any competing writer.
\item \textbf{KV blockpool}: variable-size value blocks. The pool
  is partitioned into $H$ disjoint segments, each segment owned by
  one host: only that host's workers allocate from it. Within a
  segment, allocation is size-classed (free lists at e.g.\ 64\,B,
  128\,B, $\dots$, 1\,KB) to bound fragmentation; same-host workers
  serialise on a DRAM-resident free-list spinlock. Cross-host
  blockpool contention is therefore zero by construction.
\item \textbf{Forward staging}: per-host buffer holding the value
  bytes for cross-shard \emph{write} operations whose owner is
  remote. The host's sender thread (\S{}3.2 P3), acting as
  \emph{forwarder} in this role, writes the bytes here before
  enqueueing the forward message; the message itself carries only
  \emph{(op, key, pointer, size)} and never inline value bytes
  (\S{}3.4.1). The staging slot is freed on the forward
  acknowledgement. Reads do not stage---their cross-host message
  is a cache-registration request carrying no value bytes.
\item \textbf{SPSC rings + acks}: one SPSC message ring per ordered
  host pair (no per-worker dimension), paired with a separate
  acknowledgement counter that the receiver advances after
  processing each message; ring head/tail manage slot reuse, while
  the ack counter signals protocol-level completion to the sender.
  Together they carry all $N{:}1{:}1{:}N$ traffic (\S{}3.5).
\end{itemize}

\paragraph{Per-host DRAM regions.}
\begin{itemize}\setlength{\itemsep}{2pt}
\item \textbf{Sharding table}: static $\sigma : K \to H$, populated
  at startup, read-only thereafter.
\item \textbf{Slot directory}: per-slot coherence state---sharer
  bitmap, MESI state, host-local spinlock, and a self-flag (design
  detailed below).
\item \textbf{Local KV cache}: hashmap (key $\to$ local value
  buffer) with a co-located 1-byte stale flag; serves cache-hit
  reads at DRAM latency.
\end{itemize}

\paragraph{Slot directory: a software snoop filter with back
invalidation.}
\begin{itemize}\setlength{\itemsep}{2pt}
\item \emph{Sharer bitmap = snoop filter.} A writer reads it to
  send invalidations only to current sharers; never broadcasts.
\item \emph{Invalidation + ACK = back invalidation.} The writer
  withholds the slot-pointer CAS until every listed sharer ACKs;
  this round-trip is the exclusive-acquire barrier of the write
  path (\S{}3.4.4).
\item \emph{MESI state ($S$/$I$/$M$).} Lets the next writer decide
  in $O(1)$ whether any invalidation is needed at all.
\item \emph{Self-flag.} Per-host counterpart of the bitmap; the
  read fast path (\S{}3.4.3) checks it to confirm cache validity,
  never queries a remote directory.
\item \emph{Partitioned, not replicated.} Each entry lives only on
  the DRAM of the host owning that slot's current key. No CXL or
  peer replica. Safe because sharding (P2) makes the owner the
  sole mutator---one home, no consistency protocol needed.
\item \emph{DRAM, not CXL.} Directory traffic is hot and
  owner-local (every miss, eviction, and write commit mutates it).
  DRAM stores under hardware coherence: $\sim 50$\,ns; CXL
  placement would need \texttt{clflushopt}+\texttt{sfence} per
  mutation, $\sim 10\times$ cost. Peers never read remote
  directories anyway---they receive state via targeted messages.
\end{itemize}

The layout enforces two cross-cutting invariants: (i)~every CXL
address has at most one writer at any time---by owner-partitioning,
by sharding, or by SPSC discipline---and (ii)~every DRAM region is
touched only by same-host workers. Together they confine all
cross-host coordination to the explicit message channels of
\S{}3.5.

\subsection{Protocol Mechanics}
\label{sec:protocol}

\subsubsection{Request routing}

Every operation begins with $\sigma(k)$, a single DRAM load
against the sharding table. If $\sigma(k) = \text{self}$, the
worker executes the operation locally. Otherwise routing diverges
by op type:

\begin{itemize}\setlength{\itemsep}{2pt}
\item \textbf{Cross-shard write.} The worker pre-stages the value
  bytes to its host's forward staging buffer on CXL, enqueues a
  write-forward message \emph{(op, key, staging pointer, size)}
  to the local sender, and spins on a DRAM ACK slot until the
  owner responds. The owner reads the value bytes from staging
  via \texttt{clflushopt}+load, executes the write
  (\S{}3.4.4), and ACKs.
\item \textbf{Cross-shard read.} The worker sends only a
  cache-registration message; the owner returns
  \emph{(slot pointer, value size)} and the requester self-fetches
  the bytes from CXL (\S{}3.4.3). Reads are not forwarded as
  full ops---only the registration crosses hosts.
\end{itemize}

We use $\sigma(k) = \text{HighBits}(\text{FNV-1a}(k))$ for fixed-$H$
deployments; replacing it with consistent hashing for dynamic $H$
is a drop-in change.

\subsubsection{Software cache coherence}

The slot directory's state evolves through three values: $I$ (no
host caches), $S$ (one or more peers cache), and a transient $M$
window during a write. Three operations drive transitions, each
taken on the slot's owner host under the entry's host-local
spinlock:

\begin{itemize}\setlength{\itemsep}{2pt}
\item \emph{Cache-registration} (read miss, \S{}3.4.3) adds the
  requester's bit; $I \to S$ on the first registration.
\item \emph{Cache-eviction} (local LRU pressure) clears the
  evicting host's bit; $S \to I$ if the bitmap empties.
\item \emph{Write commit} (\S{}3.4.4) drives $S \to M \to S$ (or
  $\to I$); the writer holds the spinlock across the entire
  invalidation-ACK round-trip, so concurrent registrations and
  evictions on the same slot serialise behind the writer.
\end{itemize}

Readers never enter the state machine: they observe local validity
through the self-flag, set on the registration response and cleared
on eviction. On invalidation, the receiver thread flips a 1-byte
stale flag co-located with the cache entry rather than erasing the
entry from the hashmap; the read fast path's stale check is
essentially free, and the invalidation receiver avoids the cost
of a hashmap erase.

\subsubsection{Read path}

\textbf{Fast path (cache hit, $\sim 50$\,ns).} Look up the local KV
cache, check the stale flag, return the buffer. No atomic, no
fence, no cache-line management---the absence of these is
precisely why hits run at DRAM speed.

\textbf{Slow path (cache miss).}
\begin{enumerate}\setlength{\itemsep}{2pt}
\item Enqueue a cache-registration message to the slot's owner.
\item Spin on a DRAM ACK slot until the owner responds with
  \emph{(slot pointer, value size)}.
\item \texttt{clflushopt} the affected CXL cache lines,
  \texttt{mfence}, copy the value bytes from CXL into the local KV
  cache.
\item Set the self-flag.
\end{enumerate}

The response carries no inline value bytes; the requester
self-fetches from CXL. A single out-of-band fetch path keeps the
protocol simple at the cost of one CXL load per miss, which at our
target sizes ($\geq 256$\,B) is amortised by the cross-host
round-trip anyway.

\textbf{Ordering invariant.} The owner adds the requester to the
sharer bitmap \emph{before} returning the response, under the
slot's spinlock. A concurrent write must acquire the same spinlock
and therefore observes the new sharer; the requester's freshly
filled cache entry is invalidated as part of that write's barrier
(\S{}3.4.4). The fill cannot leave behind an undetected stale
read.

\subsubsection{Write path}
\label{sec:writepath}

A write to a key on its owner host proceeds in eight steps:

\begin{enumerate}\setlength{\itemsep}{2pt}
\item \textbf{Acquire} the slot's host-local spinlock.
\item \textbf{Allocate} a new value block from this host's
  blockpool segment.
\item \textbf{Write} the value bytes to the new block on CXL,
  using non-temporal stores plus \texttt{sfence}.
\item \textbf{Invalidate sharers}: enqueue an invalidation
  message to every host in the bitmap (excluding self), and spin
  on the local DRAM ACK slot until all return.
\item \textbf{Commit}: \texttt{lock cmpxchg} the slot pointer to
  the new block, then \texttt{clflushopt}+\texttt{sfence}.
\item \textbf{Update} the directory: bitmap reduces to
  $\{\text{self}\}$, MESI to $S$ (or $I$); release the spinlock.
\item \textbf{Refresh} the local KV cache with the new value.
\item \textbf{Retire} the old block to a lazy GC queue, drained
  asynchronously once no reader could still hold its pointer.
\end{enumerate}

Steps 4 and 5 are the protocol's commit point. \emph{Step 4 is
the strict-linearizability barrier}: until every previous sharer
ACKs, no host can serve a stale read of this slot. \emph{Step 5
publishes the new value}: any host that subsequently re-fetches
follows the new pointer; any reader whose copy was invalidated in
step 4 finds its stale flag set and re-enters the slow path
(\S{}3.4.3), where it serialises against in-flight writes via the
same spinlock.

\subsubsection{Storage: copy-on-write blocks}

UPDATE allocates a fresh value block rather than overwriting
in-place. This serves two correctness ends. \emph{Crash
consistency}: blocks larger than 8\,B cannot be published as a
single observable unit on CXL, so an in-place overwrite interrupted
by a crash leaves a torn block; CoW makes the slot-pointer CAS
(step 5 of \S{}3.4.4) the sole commit point, so readers always
observe the old or new block, never a partial write.
\emph{Reader/writer isolation}: a CoW block is immutable once any
pointer to it has been published, so concurrent reads on the slow
path (\S{}3.4.3) need no value-byte synchronisation---they follow
whichever pointer they observed and read a consistent snapshot. CoW
also avoids a bimodal in-place fast path that would otherwise be
needed for same-size updates and would carry its own
crash-consistency argument.

The blockpool is partitioned across owner hosts: each owner
allocates from its own CXL segment with size-class sub-allocation
and keeps the free list in DRAM, eliminating cross-host free-list
contention.

\subsection{Cross-Host Communication via $N{:}1{:}1{:}N$ Aggregation}
\label{sec:comm}

The protocol carries four message types over the same
infrastructure: invalidation, cache-registration, cache-eviction,
and write-forwarding. All four follow the same data flow:

\begin{enumerate}\setlength{\itemsep}{2pt}
\item \textbf{Worker $\to$ sender} (DRAM, hardware-coherent):
  workers \texttt{fetch\_add} a tail counter on the per-host MPSC
  aggregator; the sender thread drains it and batches up to $K$
  messages per destination.
\item \textbf{Sender $\to$ receiver} (CXL, SPSC): the batched
  entries are written to the per-pair ring; a single
  \texttt{sfence} terminates the batch.
\item \textbf{Receiver $\to$ worker} (DRAM): the receiver
  dispatches each message to its local effect (directory update,
  cache stale-flag set, response delivery), then advances the ack
  counter once per batch.
\end{enumerate}

Three properties matter:
\begin{itemize}\setlength{\itemsep}{2pt}
\item \emph{DRAM-only MPSC contention.} Multi-producer worker
  traffic contends on a single DRAM cache line---hardware-coherent
  at $\sim 5$\,ns---rather than on a CXL line.
\item \emph{Per-batch \texttt{sfence}.} One fence drains the batch
  rather than every producer paying its own.
\item \emph{Per-batch ack advance.} CXL ack-counter traffic scales
  with the number of batches, not the number of messages.
\end{itemize}

The cost is two extra hand-offs per round-trip ($\sim 1$\,µs each,
measured), absorbed by the strict-A protocol's CXL-bound
round-trip budget.

\subsection{Synchronization Primitives}

Three primitives, each matched to where its data lives:

\begin{itemize}\setlength{\itemsep}{2pt}
\item \textbf{Host-local spinlock on shared DRAM}
  (\texttt{pthread\_spinlock\_t} with \texttt{PROCESS\_SHARED}).
  Protects the slot directory against same-host worker contention;
  hardware coherence within one host's domain makes it no costlier
  than its thread-only equivalent.
\item \textbf{Single-host CAS on CXL} (\texttt{lock cmpxchg}).
  Updates the slot pointer at write commit (\S{}3.4.4). CXL
  Type-3 has no cross-host atomic semantics, but sharding (P2)
  guarantees a single-host writer per slot, so single-host
  atomicity suffices.
\item \textbf{Lock-free SPSC on CXL.} Tail and head counters with
  \texttt{clflushopt}+\texttt{sfence} ordering between an entry
  write and the tail advance; SP/SC discipline removes any need
  for mutex or cross-host atomic.
\end{itemize}

Lamport's Fast Mutex, the predecessor design's cross-host
critical-section primitive, is therefore unused on the normal path
of this protocol; it is retained only as a fallback for degraded
modes.

\subsection{Design Choices Discussion}

The four most consequential decisions and their abandoned
alternatives, summarised:

\begin{itemize}\setlength{\itemsep}{2pt}
\item \emph{Per-slot vs.\ per-bucket directory.} Per-slot, to
  avoid a $\sim S{=}7\times$ false-invalidation amplification under
  uniform key access; the $\sim$7\,MB / host metadata cost is
  negligible (\S{}3.3).
\item \emph{Lazy stale flag vs.\ physical eviction.} Lazy, since
  the 1-byte stale check is co-located on the cache entry and
  costs no extra fence; mirrors hardware MESI's
  invalidate-but-keep-line behaviour (\S{}3.4.2).
\item \emph{Static sharding vs.\ consistent hashing.} Static for
  the fixed-$H$ testbed---a single DRAM load per request;
  consistent hashing is a drop-in replacement when $H$ becomes
  dynamic (\S{}3.4.1).
\item \emph{Copy-on-write vs.\ in-place updates.} CoW, for crash
  consistency of $> 8$\,B blocks and to keep value bytes immutable
  for concurrent readers; in-place would require a second
  size-class fallback path and a separate crash-consistency
  argument (\S{}3.4.5).
\end{itemize}
