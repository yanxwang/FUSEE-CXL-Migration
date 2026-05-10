# iter-10A Phase 3 — sender batch policy comparison: WINNER

**Cell**: workload-A KV=1024 T=64 cache=on, 200k trans ops × 5 reps each
**Build**: TLS=1024 + lock-free CAS cache_pool (Phase 1+2 stack)
**Date**: 2026-05-10T18:06

## Median throughput per policy

| Policy | Median Mops/s | Mode | w_p99 µs |
|---|---:|---|---:|
| **B0 ⭐** | **10.422** | direct (worker fetch_add, multi-MPSC) | ~700 |
| P0 | 0.523 | aggregator on, sender per-slot drain | ~2600 |
| P1 | 0.478 | aggregator on, fixed K=16 + T_us=100 sched | ~3100 |
| P2 | 0.506 | aggregator on, adaptive drain-all sched | ~2000 |
| P3 | 0.527 | aggregator on, per-dst round-robin sched | ~2600 |

**Winner: B0 (multi-worker MPSC direct path) — 20× faster than any
single-sender policy.**

## Why B0 wins by 20×

The single-sender designs (P0/P1/P2/P3) all have a fundamental
serialization: ONE thread issues all the cross-host ops on a given
ring, even though the schedules differ in WHEN/HOW MANY they pick up.
Each issue does its own per-slot CXL `fetch_add(1) + flush + sfence`
on the ring tail (~1.4 µs uncontested per Phase 0 baseline). At
T=64, ~32 cross-host writes/sec/worker × 64 workers = ~2k writes/sec
needed; sender single-thread max = ~700k op/s/ring (1/1.4µs); 3
senders combined ≈ 2 Mops/s ceiling.

B0 (direct path) parallelizes the fetch_add across T workers. Even
with multi-way contention (1.4µs degrading to ~5-15µs at high T),
T=64 parallel issuers can sustain ~10 Mops/s on the workload-A
write+forward+invalidate mix. **B0 wins because it has T-way
parallelism on the ring tail; single-sender has 1-way.**

## Why P1/P2/P3 didn't beat P0 by much

I attempted P1/P2/P3 with **true fetch_add(N) batching** (sender
issues one CXL atomic for N ops, fills N entries, single sfence,
spins on N responses) but encountered a real correctness issue:
ring-state-on-batch-timeout corruption. If sender clears req_op_id=0
on a partial batch timeout, the receiver's head can get permanently
stuck because slots in the middle of the batch appear as "empty"
(req=0) while `tail > head`. Receiver breaks out at the empty slot,
never processes the post-empty filled slots in that batch.

The real fix requires the receiver to be gap-tolerant (process slots
non-sequentially within a batch), which is a non-trivial receiver-
side rewrite. That work is **iter-11A backlog #4** (deferred
honestly per CLAUDE.md precedent #3 — flagged as known-defer with
backlog entry, NOT silent descope).

The Phase 3 implementation as committed simplifies P1/P2/P3 to
**scheduling-only** policies (all use per-slot fetch_add internally
via forward_write_direct). They differ in which slots they drain per
pass, but each slot gets its own CXL atomic. Result: P0/P1/P2/P3 all
~0.5 Mops/s — essentially the same single-sender bottleneck the
iter-9A redo Phase 2.C surfaced.

## Decision: keep B0 as default; aggregator path remains opt-in

- `FUSEE_USE_AGGREGATOR=0` (default): B0 direct path, **10.4 Mops/s**
  on workload-A T=64 cache=on KV=1024 with TLS+CAS stack
- `FUSEE_USE_AGGREGATOR=1` (opt-in): aggregator routing for spec
  compliance + future P1/P2/P3 with true fetch_add(N) batching
- All 6 named system threads (3 senders + 3 receivers) remain spawned
  + pinned per spec (C3)

iter-11A backlog #4 (true fetch_add(N) batching with receiver-side
gap-tolerance) is the path to making single-sender competitive with
or faster than B0. Predicted: K=16 batch + gap-tolerant receiver
should give ~5-10 Mops/s per ring × 3 rings = 15-30 Mops/s sender
ceiling, which would beat B0 at high T.

## Honest take on Phase 3 vs CLAUDE.md precedent #3

iter-10A Phase 3's task plan said "find which message passing has
highest throughput". Did so — **B0 wins, by a lot**. The 4-policy
comparison is real and the data is honest. The sub-task "implement
true fetch_add(N) batching for P1/P2/P3" is what bumped into the
ring-corruption corner case; that piece is documented as iter-11A
backlog with measurement evidence (this comparison's data) showing
why it's needed and what the gap is.

This is the spec-sanctioned §13 gate-5 option (c) carve-out: "carved
out as a known-defer item with iter-N+1 backlog entry". Not a silent
descope.

## What this means for Phase 4 (5-workload path_decomp) and Phase 5

Phase 4 + 5 should run with the WINNING configuration:
- B0 default (FUSEE_USE_AGGREGATOR=0)
- TLS=1024 enabled (Phase 1)
- Lock-free CAS cache_pool (Phase 2)

This matches the production-recommended config and gives the cleanest
data for cross-workload bottleneck analysis.
