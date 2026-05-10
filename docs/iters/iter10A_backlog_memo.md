# iter-10A backlog memo (priority order, post-iter-9A-redo)

**Source**: end-of-iter-9A-redo (2026-05-10)
**Branch**: `feat/cxl-migration`
**Predecessor**: `iter9A_redo_summary_20260510.md`
**Replaces**: prior iter10A_backlog_memo from end-of-iter-9A-original

---

## Major change vs prior memo

The prior iter-10A backlog memo (drafted at end-of-iter-9A-original)
listed Tier 2 items #3 (3-ring split), #4 (per-thread aggregator + 3
senders), #5 (ForwardStaging arena), #10 (C4 spec revision) as
"deferred" — but those were Phase 2 in-scope items that iter-9A
original silently descoped. iter-9A redo DELIVERED all of them. This
memo removes them from the backlog.

This memo's Tier 2 contains only **genuinely-deferred optimizations**
identified by iter-9A redo's measurement (no relabeled in-scope work).

---

## Tier 1 — mandatory carve-outs from iter-9A redo

### #1: 5-rep re-sweep of 29 anomaly cells (gate 5 obligation)

iter-9A redo Phase 4 sweep flagged 29 cells under §13 gate 5
(`gap_to_target.md` last section, full list there). Per gate-5 option
(c), iter-9A redo explicitly carved out to iter-10A. iter-10A MUST
run multi-rep on each cell to convert "carved out" → "explained" or
"deterministic regression".

Pattern from iter-7A Phase 1: ~88% of similar carve-outs self-resolved
on simple retry (22/25). Expected: ~25/29 cells self-resolve.

**Cell list**: see
`docs/g34_scaling_ycsb_iter9A_redo_20260510_072111/gap_to_target.md` →
`## §13 gate 5 — anomaly scan` section (29 rows table).

**Effort estimate**: 29 × 5 reps × ~5s/cell ≈ 12 min focused sweep.

### #2: Re-validate G6 (rw race test) on current build

iter-9A original deferred G6. iter-9A redo also did not re-run G6.
iter-10A should run alongside any sender-thread work (which lands as
backlog #4) to confirm strict-A still holds with sender-batched plumbing.

---

## Tier 2 — performance optimizations (genuine deferrals)

### #3: Forwarder-pool-direct + cross-host pool generation (RECOVERS read-path regression)

iter-9A redo's read path regressed ~37% on workload-b/c/d (12.4 / 11.8
/ 11.6 Mops/s vs iter-9A original's 18.6 / 19.0 / 18.4) because the
C2-compliant ReadRing response carries only `(blk_off, value_len)` —
the reader does an additional `pool_->read(blk_off + 4, ...)` LD-CXL
after seeing the response, instead of the inline value bytes that
iter-9A original returned in the SAME ring entry.

This is fundamentally one extra cross-host LD-CXL per cross-host miss
(~600 ns at p50, ~5 µs p99). At T=64 with cache=on, cross-host
miss frequency is enough that the extra load eats ~33% of read throughput.

**Fix candidate**: forwarder-pool-direct (was iter-10A backlog #6 in
prior memo). Owner's ReadReceiver writes the response such that
reader's pool->read can be overlapped with the resp_op_id load (or
batched into the same cacheline that the receiver flushes anyway).

**Required design work** (Tier-2 first design task):
- ReadEntry response shape: include `value_first_cl` (8 bytes) so
  short values come back in the response itself; longer ones still go
  via pool->read but the first cacheline is already warm.
- Cross-host block lifetime (generation tag for free-after-ack)
- Reader failure mode (pool block freed before reader fetches)
- Pool capacity rebalancing across hosts

**Estimated win**: workload-b/c/d back to ~18 Mops/s; pushes the best
cell across the 18 Mops/s threshold for the first time.

### #4: Sender slot batching (Phase 2.C aggregator perf recovery)

iter-9A redo Phase 2.C delivered the aggregator + 3 sender threads,
satisfying spec §2.E (6 named/pinned system threads). But the unbatched
single-sender-per-ring becomes a bottleneck at high T (workload-A
KV=1024 T=64 cache=on dropped 9.8 Mops/s direct → 0.5 Mops/s
aggregator). Currently the aggregator is opt-in via
`FUSEE_USE_AGGREGATOR=1`.

**Fix**: senders reserve K consecutive ring slots in one fetch_add
(N=16 amortizes 22× the atomic cost). After batching, aggregator
should be net-positive at high T.

**Code changes**: ~150 LOC in cxl_kv_ops_A.cc sender loops + worker
ack-tracking changes (worker_op_id mapping to ring slot N..N+K-1).

### #5: Lock-free hashmap for `cache_pool`

iter-9A redo Phase 3 decomp confirmed W10 (dir update +
cache_pool_insert) at p50 3.97 µs vs Phase 0 expected ~1 µs (4× over
baseline). Same finding as iter-9A original. H/E < 5× anomaly threshold
so no Phase 3.1 in-iter fix triggers — iter-10A is the right place.

**Fix candidate**: lock-free hashmap (per task plan §"Out of scope")
should bring W10 to ~1-1.5 µs and recover ~3 µs / write-op throughput
at high T. Stack: Folly UnorderedMap or open-addressing CAS.

### #6: Hot-bucket sharding within owner

Long-standing item from iter-5C; not addressed in iter-6A through
iter-9A redo. workload-a Zipf distribution makes some buckets very hot;
sharding within owner spreads load across cores. Currently workload-a's
~37% of writes hit the top-1% bucket; serialization on
SlotDirectoryEntry's spinlock is the next ceiling.

### #7: Variable-length keys

Independent of value-length work (iter-9A delivered values).
Required for full FUSEE compatibility; orthogonal to perf.

---

## Tier 3 — spec / discipline items

### #8: Re-enable 2 hash-diff tests (xhost_read, xhost_write)

iter-7A backlog item, NOT done in iter-8A or iter-9A original or
iter-9A redo. Mechanical attach-signature update + thread `pool` +
`InvalRingMatrix` params + new `WriteRingMatrix` + `ReadRingMatrix` +
`ForwardStagingMatrix` params.

### #9: BucketLockTable removal (-2.5 GB CXL region)

Long-standing item from iter-7A backlog. v2 path doesn't use
BucketLockTable; ~2.5 GB CXL region wasted. iter-9A redo's design_goals.md
§II layout already removes the row from documentation (commit 30adf87)
but the code still allocates it. Drop the allocation.

### #10: Living-docs full §II.3-II.6 rewrite

iter-9A redo delivered the C7 sync as an "amendment" (terminology
mapping table at the top of Part II) rather than a full rewrite of
§II.3-II.6. The narrative text in §II.3-II.6 still uses pre-iter-9A
names ("ForwardResponder", "CacheDispatcher", "ForwardRing"). Full
rewrite of those sections to use the canonical iter-9A redo names is
deferred.

Recommended: do this together with iter-10A backlog #3
(forwarder-pool-direct) since #3 changes the read-path narrative anyway.

---

## How to prioritize iter-10A scope

If iter-10A deadline is < 2 days: Tier 1 only + #4 (sender batching)
+ #8 (re-enable tests). 2 days.

If iter-10A deadline is 5+ days: Tier 1 + #3 (forwarder-pool-direct)
+ #4 (sender batching) + #5 (lock-free cache_pool). The combination
is the most likely path to push best-cell over 20 Mops/s.

If iter-10A is meant to push absolute peak above 20 Mops/s on YCSB-A
AND YCSB-C: Tier 1 + #3 + #4 + #5 + #6 (hot-bucket sharding). The
read-path recovery (#3) gets b/c/d back to 18+; the write-path
optimizations (#4 + #5 + #6) push workload-A above 20.

---

## What's NOT in this backlog (vs prior memo)

The prior memo (drafted at end-of-iter-9A-original) had these Tier 2
items:

- ~~#3 Full 3-ring split~~ — DELIVERED in iter-9A redo Phase 2.A
- ~~#4 Per-thread aggregator + named Sender threads~~ — DELIVERED in
  Phase 2.C (with Phase 2.C #4 follow-up for batching)
- ~~#5 ForwardStaging[H] arena~~ — DELIVERED in Phase 2.B
- ~~#10 C4 spec revision~~ — DELIVERED via assert_n_to_n_active() in
  Phase 2.G
- ~~#11 Re-enable 2 hash-diff tests~~ — still pending (renumbered #8
  here)
- ~~#12 BucketLockTable removal~~ — still pending (renumbered #9)
- ~~#13 Living-docs C7 update~~ — partially delivered as amendment;
  full rewrite renumbered #10

This is the cleanup that CLAUDE.md precedent #3 mandated: don't
relabel un-done in-scope work as "next-iter backlog". iter-9A redo
delivered the in-scope work; iter-10A backlog now lists only
genuine deferrals.
