"""Add comprehensive speaker notes to every slide of tigon_osdi25.pptx.

Run AFTER build_pptx.py:
    python3 speaker_notes.py

Overwrites the notes on every slide. Keeps the slides themselves untouched.
"""

from pathlib import Path
from pptx import Presentation

HERE = Path(__file__).parent
PPTX = HERE / "tigon_osdi25.pptx"

NOTES = {
1: """Welcome. I'll present Tigon, an OSDI 2025 paper out of UT Austin and
UIUC. The headline: it's the first distributed in-memory database that
synchronizes cross-host accesses through CXL memory instead of through
the network.

Plan: about 60 minutes. I'll motivate the problem, walk through why
existing solutions don't cut it, then dig into the design, and end with
evaluation and discussion. Please interrupt with questions at any time.""",

2: """Quick roadmap. I'll spend ~10 minutes on background because the
problem only makes sense if you understand CXL and CXL pods, which are
fairly new. Then ~5 minutes stating the research problem, ~8 minutes on
existing solutions and why each is insufficient, ~20 minutes on the
design (the meat of the paper), ~15 minutes on evaluation, and close
with discussion.

If you're familiar with CXL, feel free to nudge me to go faster through
the background.""",

3: """Let's start with background. Three things I need on the table:
what makes distributed transactional databases hard, why RDMA-based
approaches still leave performance on the table, and what CXL and a
CXL pod actually are.""",

4: """Distributed transactional databases. People have worked on these
since at least System R* in the 80s. The architecture almost everyone
uses is shared-nothing: partition the data, each host owns a shard,
route operations to the shard owner. Single-partition transactions
stay local and are fast.

The problem is multi-partition transactions. A multi-partition txn
needs data from two or more hosts. That means RPC round-trips during
execution, and then a two-phase commit at the end to guarantee
atomicity. A network RTT is in the microseconds; local DRAM access is
in the hundreds of nanoseconds — a factor of 100-1000.

The consequence: performance is fine until you have too many
multi-partition txns, and then it collapses. TPC-C by default has
10-15% multi-partition — real workloads have more.""",

5: """RDMA was the natural next step. Bypass the kernel, bypass the
TCP stack, directly read remote memory. Two styles of systems:

One, keep partitioned shared-nothing, use RDMA to speed up the
messages and 2PC. FaRM, FaSST, DrTM are in this family.

Two, go all the way — disaggregate memory. The compute nodes have no
local data; everything is on the remote memory server accessed via
one-sided RDMA. FORD, Motor, XSTORE, Taurus. This avoids the
multi-partition problem by not partitioning.

Both still hit a ceiling. One-sided RDMA is fast by network standards
but one to two orders of magnitude slower than local DRAM. Every tuple
read could be a microsecond. Motor, the paper's baseline, tops out at
~100K TPC-C txn/s.""",

6: """Now CXL. Compute Express Link is an open standard on top of
PCIe 5/6. The key protocol for us is CXL.mem, which lets the CPU access
memory on a PCIe device using regular load/store instructions — no
RDMA verbs, no message passing API.

Three specification versions matter:
- CXL 1.1: one host per device. Just memory expansion.
- CXL 3.0: cacheline-granularity sharing across hosts, with optional
  hardware cache coherence via back-invalidations.
- CXL 3.2: the current spec, refines 3.0.

Real hardware exists but is early. SK Hynix Niagara 2.0 supports up to
8 hosts but no HW cache coherence yet. Microsoft has a 2-host prototype.
The paper's design targets the 3.0 generation.

The figure on the right is Tigon's architecture — we'll unpack it
later; for now note the 'HWcc' box (the small hardware-coherent region)
and the 'SWcc' box (the larger region where Tigon provides coherence
in software).""",

7: """A CXL pod is a specific topology: a small number of hosts — 8 to
16 — all directly connected to a single shared CXL memory device
through a multi-headed device, or MHD. An MHD is a CXL chip with
multiple host ports — no switch, which would add latency.

Why is this interesting? It's an intermediate architecture. It's not
a shared-memory multiprocessor because inter-host coherence is
expensive. It's not a fully distributed system because memory is
load/store addressable across hosts.

Architectural debates about the limits of SMP scalability — Kimberly
Keeton's 'The Machine', Keaton and Patterson's arguments — have been
around for years. CXL pods give us a new point. Tigon tries to
navigate the specific tradeoffs of that point.""",

8: """But CXL is not local DRAM. Memory latency measurements from Sun
et al., the study the paper cites: CXL access is 214 to 394 nanoseconds,
depending on the device and pattern. Local DRAM is 111 to 117. So
1.5-3x latency penalty.

Bandwidth is the scarier number. Local DRAM on a modern server gives
you 200+ GB/s. CXL gives you 18-52 GB/s read-only, sometimes lower
depending on access pattern. That's roughly 10-20% of local DRAM
bandwidth.

Three design implications, which will recur throughout Tigon's design:
- You cannot put all your data on CXL; the hot path must hit local DRAM.
- You should copy data into CXL sparingly, not eagerly.
- You should minimize bandwidth-heavy operations like non-temporal
  stores and uncached loads.""",

9: """There's an extra limitation specific to cross-host sharing:
hardware cache coherence. CXL 3.0 says the device can provide
coherence via back-invalidations, like a processor snoop filter.

The problem is silicon area. A snoop filter that covers every
cacheable line on every host costs huge area. AMD did the math: to
hardware-cover 16 hosts × 504 MB of cacheable data on an Intel
Granite Rapids you'd need 7.9 GB of tag storage. Not happening.

So vendors will ship devices that keep a limited region hardware-
coherent — tens to hundreds of megabytes. Everything else is still
CXL memory, still addressable, still shareable — but software has to
handle coherence.

This is the single most important constraint Tigon designs around.""",

10: """OK, enough background. Let's pin down the research problem.""",

11: """Everything I showed you up to now is about sending messages
across hosts — RPC, RDMA verbs, whatever. CXL lets us do something
fundamentally different: let hosts synchronize by running atomic
operations on shared memory, the way threads in a single process
synchronize on locks.

That's Tigon's core proposal. Synchronize through memory, not through
the network.

Why isn't this trivial? Three CXL limitations that bite:
- Latency higher than local DRAM, so we can't indiscriminately access
  CXL.
- Bandwidth lower than local DRAM, so we can't move data freely.
- Hardware cache coherence is limited to a small region, so we can't
  assume everything in CXL memory is seen the same way by every host.

The research question is: can we convert cross-host message exchanges
into data-structure operations on shared memory — efficiently — under
these constraints?""",

12: """Six design goals for a good CXL-pod database. G1-G2 are about
end-to-end performance — single-partition should not regress compared
to shared-nothing, and multi-partition should degrade gracefully. G3
is the HWcc budget constraint — the design has to work in a few
hundred MB. G4 is bandwidth — CXL bandwidth is precious. G5 is
standard database correctness: serializability, durability, recovery.
G6 is the 2PC tax — if we truly synchronize via memory, we should be
able to commit without 2PC.

Any database design for CXL pods should check these boxes.""",

13: """Before presenting Tigon's design, let me sharpen the picture by
walking through four natural approaches and explaining why each one
falls short. This is the motivation for Tigon's specific design
choices.""",

14: """Approach 1: the classic. Shared-nothing partitioning with 2PC
at the end. Sundial, DS2PL, H-Store, Spanner all fit here.

Why it doesn't cut it for us: every multi-partition transaction
involves many messages. Read/write RPCs during execution, then a
prepare round, then a commit round. The paper measures 3.3 messages
per transaction for Sundial on TPC-C 60/90, 4.1 for DS2PL.

You'll see in the evaluation that even with the best 2PL or OCC
protocol, performance drops sharply as multi-partition ratio grows,
and 2PC is the inescapable tax.""",

15: """Approach 2: disaggregated memory with RDMA. Motor is the most
recent published example. No partitioning — tuples live on memory
servers, compute nodes read them over RDMA.

No multi-partition problem, no 2PC. But every tuple read is a network
RTT. The paper's evaluation shows Motor is bandwidth-limited at ~30K
txn/s on their 25 Gbps NICs — but even in Motor's own paper, with
100 Gbps, it tops out around 100K/s.

CXL memory is load/store-addressable and much faster. RDMA leaves a
lot of performance on the table.""",

16: """Approach 3: keep shared-nothing, but swap the network for CXL
message queues. HydraRPC-style. The paper actually implements this as
a baseline — Sundial-CXL and DS2PL-CXL. Shows a clean 2x improvement
at 60/90 multi-partition.

But: you still exchange messages. You still run 2PC. You're using CXL
as a better pipe. You're not exploiting its unique capability, which
is load-store-accessible shared memory with atomic operations.

Tigon asks: what if we get rid of messaging on the critical path
altogether?""",

17: """Approach 4: put every tuple in CXL memory. Now every host
directly accesses every tuple — no partitioning needed.

Three problems:
- CXL latency is 1.6-3x local DRAM, so your hot path is 1.6-3x slower.
- CXL bandwidth is 13% of local DRAM, so you'll saturate it.
- HW cache coherence only covers ~200 MB; a real database doesn't fit.

So there's no free lunch. You can't just move everything to CXL.
Data placement matters — hot data local, shared data on CXL.""",

18: """So where are we? Four natural approaches, each with a fatal
flaw. Tigon's insight connects them: at any point in time, only a
small set of tuples is concurrently shared across hosts — the
Cross-host Active Tuple set, or CAT. This set is small — megabytes.
That's exactly the size where HWcc memory is feasible.

Tigon: keep the CAT in CXL memory. Everything else in local DRAM.
Use atomic operations on CXL-resident metadata for synchronization
instead of messages. Co-design concurrency control and logging so
that 2PC is unnecessary.

Let's dig in.""",

19: """Section 4: Tigon's design. I'll cover the key insight, the
architecture, how data is organized, the software cache coherence
protocol, concurrency control, and logging. This is the largest
section — about 20 minutes.""",

20: """The foundational insight. Databases can be huge — gigabytes,
terabytes. But the set of tuples actually being touched concurrently
by running transactions is tiny.

Why? Simple arithmetic. In-memory OLTP has roughly one transaction
per CPU core at a time. Each transaction touches maybe 10-40 tuples.
Say 1000 cores, 40 tuples each, ~200 bytes per tuple: 7 MB. That's
it. That's the working set of shared data at any instant.

This is the CAT — Cross-host Active Tuples. The paper's north star:
keep the CAT in CXL memory, use atomic operations on it for
synchronization. Nothing else needs to live in CXL memory all the
time.""",

21: """Here's the high-level architecture. Each host owns a data
partition, which lives in its local DRAM. Normal DRAM, hardware
cache-coherent within the host, fast.

Shared CXL memory is split into two regions. A small HWcc region at
top — this is where latches, the CXL index, and the HWcc record for
each CAT tuple live. A larger SWcc region at bottom — this holds
tuple bodies and larger metadata that we manage coherence for in
software.

When a host needs a tuple it doesn't own, Tigon moves that tuple
into CXL memory — from then on it's part of the CAT, accessible by
all hosts. If the HWcc budget gets tight, tuples get evicted back
to their owner's local DRAM.""",

22: """An end-to-end example — Figure 2. Two hosts, transaction 1 on
Host 1 wants to read A (local) and write C (owned by Host 2).

Step 1: T1 reads A locally, takes read lock.
Step 2: T1 needs C. It sends a message to Host 2 asking it to move C
        into CXL memory.
Step 3: Host 2 copies C into the SWcc region and creates an HWcc
        record for it.
Step 4: T1 grabs the write lock on C's HWcc record via an atomic CAS,
        updates C to C=9.
Step 5: Meanwhile, T2 on Host 2 also wants C. It looks up C via the
        CXL index, sees the lock held, aborts under NO_WAIT.

At the end, T1 commits locally — no 2PC. It wrote all tuple changes
to its local log. The CAT here is literally one tuple, C.

This is the key trick: all coordination on C happens through the
HWcc latch, not through messages.""",

23: """The data organization. This is Figure 3 — the densest figure
in the paper.

Three things to look at. Left: each host has a local index in its
DRAM, mapping keys to 'local rows'. A local row holds a local latch,
a 2PL lock, a shortcut-ptr, an is-valid flag, an epoch-version for
logging, and the tuple itself.

Middle: when a tuple is moved into CXL memory, the row is split. An
8-byte HWcc record goes to the HWcc region; an SWcc row goes to the
non-HWcc region. The local row's shortcut-ptr now points to the HWcc
record.

Right: the CXL index — a B+-tree per table-partition — lives in HWcc
memory and indexes all CAT tuples from that partition. All hosts can
look up tuples through it.""",

24: """Let's zero in on the 8-byte HWcc record. Every bit matters
because HWcc memory is scarce.

1 bit for a cross-host latch (HWcc-latch). 8 bits for the 2PL lock
(counts readers, holds a writer bit). 1 bit has-next-key, needed for
correct next-key locking — I'll come back to this. 1 bit is-dirty —
says whether the tuple was modified since moving to CXL. 1 bit
clock-bit for the eviction policy. 16 bits SWcc-bitmap — one bit per
host, used by the software cache coherence protocol. 36 bits
SWcc-row-ptr — a compact offset pointer to the full SWcc row.

Total: 8 bytes. Tiny. That's what lets Tigon pack thousands of CAT
tuples into the HWcc budget.

Note everything here is HW-coherent: the latch, the lock, the tiny
flags. The SW-coherent stuff, the actual tuple body, lives elsewhere.""",

25: """Indexes. Each host has a local B+-tree in DRAM, indexing its
own partition. Standard.

Plus a CXL index in HWcc memory, indexing the CAT. All hosts can
read/write the CXL index via HWcc atomics.

An optimization: when the owner of a tuple sees that its tuple has
been moved to CXL, it caches a shortcut pointer on the local row
pointing directly to the HWcc record. This saves the owner a CXL
index lookup — important because the owner accesses its own tuples
most often.

The non-trivial part is keeping shortcut pointers correct under
concurrent data movement. The rule is: only the owner host can
initiate data movement for its own tuples. Non-owners must send a
move request. Owner-side operations are serialized by the local-latch
on the local row. So the shortcut pointer is always either null or
pointing at a valid HWcc record. Non-owners, who race with movement,
are protected by the HWcc-latch and the is-valid flag — if a tuple
gets moved back to local DRAM, is-valid becomes false and stale
pointers fail safely.""",

26: """Now the software cache coherence protocol — one of the key
novelties.

Most of CXL memory is NOT hardware cache coherent. If hosts cache
SWcc rows, you get stale data. Classic solution: bypass the cache
with non-temporal loads, or pin reads to a single host. Both are slow.

Tigon's insight: the database already has latches. Every access to a
tuple goes through a latch. So piggy-back a coherence protocol on the
latch.

The 16-bit SWcc-bitmap has one bit per host. When a host reads a
tuple, it checks its bit. If set, its cached copy is valid — use a
normal cached load. If not set, flush the cacheline, read from CXL,
set the bit. When a host writes, it unsets the bits for all other
hosts.

Result: coherence granularity is a tuple (coarser than a cacheline,
so cheaper), metadata is one word per tuple, and normal cached loads
work for readers once they've populated the cache. Much better than
non-temporal loads for workloads that reuse tuples.

The catch: this requires database-side changes. It's not a
transparent protocol; it's co-designed with the latching.""",

27: """Tuples move into CXL memory when non-owners ask for them.
They have to move back out when HWcc memory fills up.

Perfect policy would be LRU on cross-host access — evict whichever
tuple is least likely to be remotely reused. But LRU needs a linked
list threaded through all CAT tuples, and every access updates it.
That's expensive: HWcc memory for the list, contention on list
pointers.

Tigon uses CLOCK. One bit per tuple: set on any non-owner access. A
circular cursor scans CAT tuples. Bit set → clear it and move on. Bit
clear → evict.

Ablation in the eval: under tight HWcc budget, CLOCK is 2.4x faster
than LRU. Under unlimited HWcc, CLOCK is still 17% faster (less lock
contention). CLOCK wins on metadata efficiency.""",

28: """Concurrency control. Tigon uses strong strict 2PL — acquire
locks during execution, release at commit. This is classical and easy
to reason about.

Locks live in the 2pl-lock byte of the HWcc record. Acquired via an
atomic compare-and-swap on HWcc memory. From the perspective of any
host, taking a lock on a CAT tuple is just a local atomic op — no
messages.

Deadlock handling: NO_WAIT. If you can't get the lock, abort and
retry. Prior work (Yu et al.) shows NO_WAIT scales best for in-memory
DBs. The paper goes with that.

OCC and MVCC would be interesting but are explicitly future work.
Adapting them to CXL pods is non-trivial and the authors wanted to
focus on the more widely deployed 2PL first.""",

29: """The phantom problem. Classical example: a range scan sees keys
10, 20, 30. Meanwhile another transaction inserts 15. If we just
locked the individual keys we saw, 15 can be inserted — violating
serializability.

Standard fix: next-key locking. On insert/delete/scan, lock not just
the key but the 'next key' in the ordered index.

In Tigon, this is harder because the CXL index only contains the
subset of keys that are in the CAT. The 'next key' in the CXL index
might not be the true next key in the local index.

Fix: add a has-next-key bit to each CXL-index entry. Bit is set if
the CXL-index next key is the same as the local-index next key.
Maintained by the owner as inserts, deletes, and moves happen. If the
bit isn't set, the transaction must ask the owner to move the real
next key into CXL before locking.

Cost: 10-12% throughput hit for correctness. Reasonable — and a
variant that ignores phantoms actually outperforms Sundial+ even on
TPC-C default.""",

30: """Logging and recovery — and, crucially, how Tigon avoids 2PC.

Two observations.

One: because the CAT lives in CXL memory, the transaction worker on
one host can complete all tuple modifications of its transaction —
it takes locks, it writes tuples, all through atomic operations on
CXL. No cooperation from other hosts is needed to mutate state.

Two: indexes can be reconstructed from tuples during recovery. So we
don't need to log index changes; we only need to log tuple changes.

Combine these: one host has all the state changes for its transaction
in its own log. It can commit locally, durably. No 2PC.

Mechanics, borrowed from SiloR: epoch-based group commit, 10 ms
epochs; parallel value logging — each worker thread writes its own
buffer to SSD; each tuple carries (epoch, version) so recovery can
pick the latest write. The global epoch counter is in HWcc memory;
advancing it is a cheap atomic op.

This is the logging/CC co-design that makes 2PC unnecessary.""",

31: """Implementation notes.

Built on Lotus — about 18K lines of existing C++, plus 5K new lines.
Standard DB API: read, write, insert, delete, range query,
parameterized transactions.

B+-tree with optimistic crabbing, extended for next-key locking.
Offset pointers make CXL-resident structures position-independent —
the base address can differ across hosts. CXL memory appears as a
CPU-less NUMA node in Linux; they modified mimalloc to allocate from
it.

Message transport, used only for non-critical-path things like move
requests: lock-free MPSC ring buffers, metadata (head/tail) in HWcc,
payload in non-HWcc. Fast.

Epoch-based reclamation (Fraser's EBR) for memory safety — per-worker
local epoch counters in HWcc.

All open source at github.com/ut-datasys/tigon.""",

32: """Evaluation. Five questions the paper asks. End-to-end
performance vs state-of-the-art. How HWcc budget affects performance.
How much software cache coherence helps. Logging impact on latency.
Ablations of the various optimizations. I'll cover all five.""",

33: """Setup. Emulated CXL pod — because real HW with inter-host cache
coherence doesn't exist yet.

One physical machine: Intel Xeon Platinum 8568Y+, 512 GB DRAM, 128 GB
CXL 1.1 device on PCIe 5.0 x8. Measured latency/bandwidth: CXL 259 ns
vs DRAM 159 ns, CXL 31.8 GB/s vs DRAM 238 GB/s.

8 VMs on that machine, each with 5 vCPUs and 10 GB DRAM. They share
the CXL device to emulate an 8-host CXL pod. Inter-VM cache coherence
(provided by the physical CPU) stands in for inter-host HW coherence.
This is optimistic; the paper analyzes a more pessimistic case later.

HWcc budget: 200 MB by default. Paper varies this down to 10 MB in a
sensitivity study.

Baselines: Sundial+ (OCC reads + 2PL writes), DS2PL+ (2PL), Motor
(RDMA-disaggregated). All optimized.

Workloads: full TPC-C at 24 warehouses (2.2 GB), and a YCSB variant
(2.4M keys × 1 KB values, Zipf 0.7 and 0.99).""",

34: """A detour on the baselines. Sundial and DS2PL were originally
TCP-based. An unoptimized comparison vs Tigon would be unfair.

So the paper improves them:
- Replace network transport with a CXL message queue (same queue
  Tigon uses). That alone gives them 2x at 60/90.
- Convert the now-unneeded I/O thread into a worker thread. Another
  factor.
- Add durable logging (same SiloR-style protocol Tigon uses).
- Add next-key locking for phantom avoidance.
- Fill in missing TPC-C stored procedures (insert, delete, scan) so
  they actually run all 5 TPC-C transactions.

Net: the improved baselines are up to 4.2x faster than their
originals. This is a much stronger fence Tigon has to clear. All
comparisons in the rest of the talk are vs these improved baselines.""",

35: """The headline result. TPC-C throughput. X-axis: percent of
multi-partition transactions, from 0/0 to 60/90. 0/0 means standard
TPC-C with 0% remote NewOrder and Payment; 60/90 is the most
cross-host-heavy configuration.

At 0/0: Sundial+ beats Tigon by 37%. This is because Sundial uses OCC
for reads (faster for read-heavy) and doesn't do phantom avoidance.
Expected, fair loss for Tigon.

At 60/90: Tigon is 75% faster than Sundial+ and 2.5x faster than
DS2PL+. Sundial+ and DS2PL+ throughput drops sharply as
multi-partition ratio grows — the message and 2PC tax.

Motor: 15.9 to 18.5x slower than Tigon across the board, because
one-sided RDMA latency is the bottleneck on the testbed.""",

36: """Where does Tigon's win come from on TPC-C?

At 60/90, Sundial+ sends 3.3 messages per transaction, DS2PL+ sends
4.1. Tigon sends zero on the critical path. When T1 wants a remote
tuple, the tuple is already in CXL memory — it just does a CAS on the
HWcc lock.

How? Tigon moves the hot tables (CUSTOMER and STOCK) into CXL during
warmup. 720K + 2.4M tuples, 176 MB HWcc, 1.6 GB total CXL. It fits in
the HWcc budget, and once moved, it stays — CLOCK eviction never
triggers because the HWcc budget isn't exceeded.

Motor's performance is bandwidth-limited. Their testbed NICs are 25
Gbps; Motor's own OSDI'24 paper reports ~100K txn/s peak at 100 Gbps.
Tigon is ~500K txn/s on the same workload.""",

37: """YCSB. Four read/write mixes × varying multi-partition percent.

At 0% multi-partition, all systems within 3.3% — nobody pays cross-
host cost.

Where Tigon wins is when multi-partition goes up. I'll explain on the
next slide; let the audience eyeball the curves first. The pattern:
Tigon holds roughly flat, baselines drop.""",

38: """YCSB findings in detail.

Read-only at 100% multi-partition: Tigon is 2.0-2.3x over Sundial+.
At 50/50 reads/writes, 100% multi-partition, Zipf 0.99: Tigon is 2.7x
over Sundial+ and 3.5x over DS2PL+. Even bigger wins on writes — each
write is an atomic op rather than a message chain.

Motor: 5.4 to 14.3x slower.

Memory footprint: Tigon moves all 2.4M tuples to CXL. 112 MB HWcc,
2.6 GB total — fits comfortably in the 200 MB HWcc budget. No
evictions. Software cache coherence is doing the heavy lifting here
because everyone's reading the same big table.""",

39: """Scalability. 1, 2, 4, 6, 8 hosts. Workload: TPC-C 60/90 and
YCSB 95R/5W with 100% multi-partition.

Tigon: 5.7x on TPC-C, 3.5x on YCSB going from 1 to 8 hosts. Sundial+
and DS2PL+: 2.4x and 2.1x on TPC-C. The slopes: Tigon 55K txn/s per
host, Sundial+ 30K, DS2PL+ 22K.

Limits: the paper can't tell you the scalability ceiling — they're
emulating on one machine. Plausible bottlenecks: atomic-op
scalability over CXL, HW coherence bandwidth, fixed HWcc region size.
Open research question.""",

40: """Sensitivity to HWcc memory budget. Vary from 200 MB down to
10 MB.

Key result: even 50 MB of HWcc memory is essentially enough for TPC-C
— only 5.8% slower than the default 200 MB. That's a very frugal
design.

At 10 MB, you see real thrashing — Tigon moves 16K TPC-C tuples or
110K YCSB tuples per second between local DRAM and CXL. But that only
consumes 1.1% / 9.1% of CXL bandwidth — the cost is latency, not
bandwidth. This is why minimizing HWcc footprint mattered in the
design — so we can survive in tiny HWcc regions.

The implication: even quite small real CXL HWcc regions should be
workable.""",

41: """An important 'what if' experiment. Their emulation uses
intra-socket cache coherence, which is faster than what real inter-
host CXL back-invalidations will be. They can't directly measure
back-invalidations (no HW counter) but they can count software
invalidations: 14.5% of accesses need them.

Conservative assumption: real back-invalidations are 4x slower than
intra-socket. That would cost Tigon 41.4% of throughput.

Even with that pessimistic discount: Tigon is still 2.8% faster than
Sundial+, 45% faster than DS2PL+, 9.6x faster than Motor. Tigon's
'minimize HWcc usage' design principle directly pays off here — the
less traffic on HW-coherent lines, the less it hurts when back-
invalidations are expensive.""",

42: """Software cache coherence ablation. Four configurations:
NoSWcc (HWcc-only — everything must fit in HWcc), NonTemporal (no
caching; every SWcc read is non-temporal), NoSharedReader (only one
host at a time can read-cache any given SWcc row), and full Tigon.

TPC-C: NoSWcc is 19% slower at 60/90 (thrashing due to small HWcc
region). NonTemporal is only 4.5-5.1% slower — because tuples get
cached in local DRAM already, so the cacheable-read win on CXL is
small.

YCSB has more cross-host sharing, so the gaps widen. NoSWcc is 4.3x
slower at 100% multi-partition. NonTemporal is 11-20% slower.
NoSharedReader is 15% slower — it forces SWcc-bitmap flips and
cacheline flushes that Tigon avoids.

Takeaway: SWcc is essential for read-heavy workloads with cross-host
sharing. Just using HWcc memory is not enough once the CAT is large.""",

43: """Logging latency tradeoff. Epoch duration vs throughput and
tail latency.

Tiny epoch (1 ms): throughput drops ~5%, but p99 explodes — 345 ms —
because you're doing many small writes and SSD queue grows under
contention.

10 ms epoch: 2.8% slower than 50 ms but 48% lower p50 latency (17.7
ms). Good tradeoff. That's what Tigon uses by default.

Without logging, Tigon is only ~6% faster than with 10 ms logging —
so logging is cheap.

Motor uses 3-way in-memory replication for durability, no group
commit. Their p50 is in hundreds of microseconds, but throughput is
10x lower. Tigon trades a few ms of tail latency for throughput.""",

44: """Three optimization ablations.

CLOCK vs LRU: LRU needs a linked list in HWcc, which bloats HWcc
memory by 33% and adds lock contention. Under tight HWcc budget, LRU
is 2.4x slower. Under unlimited HWcc, still 17% slower. CLOCK wins
clearly.

Shortcut pointer: lets the owner skip the CXL index. +16% TPC-C,
+8-24% YCSB 95R/5W. Substantial.

is-dirty tracking: lets the owner read clean tuples from local DRAM
rather than CXL. +60% on YCSB read-only at 10% multi-partition, +27%
at 100%. Huge for read-heavy.

Together these are what make Tigon frugal with CXL bandwidth.""",

45: """Let me wrap up with discussion and takeaways.""",

46: """Questions worth debating in Q&A.

Is the CAT assumption robust outside OLTP — e.g., for long-running
analytical queries that touch huge scans? Probably not; the design
is specifically for OLTP.

What happens at 32 or 64 hosts? The SWcc-bitmap is 16 bits today.
HWcc region is finite. Scaling out the CXL pod is an open question.

SWcc requires database-side code changes. Is it reusable by other
applications? Probably not directly — you'd redo the co-design per
system.

No real HW today — the eval is on emulated CXL. Numbers are indicative
but not final.

2PL + NO_WAIT: abort rate under very high contention could be an
issue.

Failure model: a CXL pod shares a memory device. That device's
failure takes down the pod. Is that acceptable? Pasha (the architecture
paper) and CXL-SHM study partial-failure models — worth reading
alongside.

OCC/MVCC on CXL pods: left as future work.""",

47: """Takeaways.

One: CXL pods are a genuinely new architectural point. Treating them
like a faster network misses the opportunity.

Two: the right abstraction for distributed OLTP on a CXL pod is
shared memory with atomic ops, not message passing.

Three: convert messages into atomic ops. That's what kills 2PC.

Four: be stingy with hardware cache coherence — it's fundamentally
limited by silicon area. Co-design a software coherence protocol for
the bulk of your data.

Five: the results — 2.5x over the best optimized shared-nothing
baseline, 18.5x over RDMA — suggest the approach is worth chasing
further.

This opens a research agenda: OCC and MVCC variants, handling larger
pods, real hardware experiments once CXL 3.x with inter-host coherence
ships, and better failure models.""",

48: """Thank you. Happy to take questions.

The paper is up at the OSDI 25 proceedings page, code is open source
on GitHub. Related reading I found useful: Pasha at CIDR'25 (the
architecture precursor from the same group), SiloR at SOSP'13 (the
logging protocol Tigon adapts), Octopus (2025) on CXL memory pooling
at scale, and Motor at OSDI'24 (the RDMA baseline)."""
}

def main():
    prs = Presentation(str(PPTX))
    for i, sl in enumerate(prs.slides, start=1):
        if i in NOTES:
            sl.notes_slide.notes_text_frame.text = NOTES[i]
    prs.save(str(PPTX))
    total = sum(len(v) for v in NOTES.values())
    print(f"Wrote notes on {len(NOTES)} slides, {total} chars total.")


if __name__ == "__main__":
    main()
