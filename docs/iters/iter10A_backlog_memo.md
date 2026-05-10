# iter-10A backlog memo (priority order)

**Source**: end-of-iter-9A (2026-05-10)
**Branch**: `feat/cxl-migration`
**Predecessor**: `iter9A_summary_20260510.md`

---

## Tier 1 — mandatory carve-outs from iter-9A

### #1: 5-rep re-sweep of 55 anomaly cells (gate 5 obligation)

iter-9A Phase 4 sweep flagged 55 cells under §13 gate 5
(`gap_to_target.md` last section). Per gate 5 option (c), iter-9A
explicitly carved out to iter-10A. iter-10A MUST run multi-rep on
each cell to convert "carved out" → "explained" or "deterministic
regression".

Pattern from iter-7A Phase 1: ~88% of similar carve-outs self-resolved
on simple retry (22/25). Expected: ~50/55 cells self-resolve.

**Cell list**: see
`docs/g34_scaling_ycsb_20260510_043121/gap_to_target.md` →
`## §13 gate 5 anomaly scan` section.

**Effort estimate**: 55 × 5 reps × ~30s/cell ≈ 2 h focused sweep.

### #2: Re-validate G6 (rw race test) on current build

iter-9A did not re-run G6. iter-10A should run alongside any
sender-thread work to confirm strict-A still holds with new
plumbing.

---

## Tier 2 — task plan iter-9A out-of-scope items, in priority order

### #3: Full 3-ring split (Write/Read/Inval as separate CXL channels)

Per task plan §2.A. Current iter-9A code uses single
`ForwardRingMatrix` carrying UPDATE/INSERT/DELETE/CACHE_REGISTER
mixed; InvalRing is already separate (iter-5A). Splitting Write
from Read should let `ReadReceiver` skip directory locking
(register-only path) and `WriteReceiver` skip the cross-host
forward-staging in some cases. Estimated win: 10-30% read latency
reduction.

**Code changes**: ~600-800 LOC across new `cxl_write_ring.h`,
`cxl_read_ring.h` (refactor of `cxl_forward_ring.h`),
`cxl_kv_ops_A.cc` dispatch.

### #4: Per-thread aggregator + named Sender threads (plan §2.C-E)

Per task plan §2.C-E. Workers currently producer-direct-to-CXL ring;
adding per-thread DRAM aggregator + 1 sender thread per ring lets
sender batch + flush, reducing per-op CXL fence cost.

**Naming**: WriteSender (cpu 64), ReadSender (cpu 66), InvalSender
(cpu 68). Per task plan §2.F.

**Code changes**: ~300-500 LOC. Reuses existing
`cxl_a_local_aggregator.{h,cc}` from iter-2A (currently unused in
production path).

### #5: ForwardStaging[H] arena (plan §2.B)

Per task plan §2.B. iter-9A temporarily extended
`ForwardEntry` to 1088B inline payload as a bridge. Phase 2's
proper design: control-only ring entry (~64B) + per-host CXL
staging arena (1 MB) for value bytes. Reduces per-message ring
footprint 17× and decouples ring depth from KV size.

**Code changes**: ~150 LOC for staging arena + refactor of
forward path for staging→pool copy.

### #6: Forwarder-pool-direct + cross-host pool generation (user memo 2026-05-04)

Per task plan §"Memo for future". Skip the staging copy entirely
by having forwarder write directly into its OWN per-host blockpool
(no separate staging); message carries `(host_id, blk_off,
generation)` and owner reads from forwarder's pool block. Saves
~10 µs/forward at KV=1024.

**Required design work** (deferred to iter-10A first design task):
- Cross-host block lifetime (generation tag for free-after-ack)
- Reader failure mode (pool block freed before reader fetches)
- Pool capacity rebalancing across hosts

### #7: Lock-free hashmap for `cache_pool`

iter-9A Phase 3 found W10 (dir update + cache_pool_insert) at
p50 4.09 µs vs Phase 0 expected ~1 µs (4× over baseline; H/E < 5×
threshold so no Phase 3.1 in-iter fix). Lock-free hashmap (per
task plan §"Out of scope") should bring W10 to ~1-1.5 µs and
recover ~3 µs / write-op throughput at high T.

### #8: Hot-bucket sharding within owner

Long-standing item from iter-5C; not addressed in iter-6A through
iter-9A. workload-a Zipf distribution makes some buckets very hot;
sharding within owner spreads load across cores.

### #9: Variable-length keys

Independent of value-length work (iter-9A Phase 1 did values).
Required for full FUSEE compatibility; orthogonal to perf.

---

## Tier 3 — spec / discipline items

### #10: C4 spec revision

iter-9A constraint C4 says "startup assert phys_hosts_pr_ ≥ 2 if
num_hosts_ ≥ 2". But `phys_hosts_pr_` field was removed in iter-5A
(replaced by ForwardRingMatrix). Need new mechanism to assert
N:1:1:N is wired before first cross-host op fires.

Candidate: assert at first `forward_to_owner()` call that `fr_ !=
nullptr` and `ir_ != nullptr` (both rings enabled). Currently the
checks are scattered; need consolidation.

### #11: Re-enable 2 hash-diff tests (xhost_read, xhost_write)

iter-7A backlog item, not done in iter-8A or iter-9A. Mechanical
attach-signature update + thread `pool` + `InvalRingMatrix` params.

### #12: BucketLockTable removal (-2.5 GB)

Long-standing item from iter-7A backlog. v2 path doesn't use
BucketLockTable; ~2.5 GB CXL region wasted.

### #13: Living-docs C7 update sweep

iter-9A Phase 2 minimal didn't update blueprint Part II §II.3-II.6
(those describe pre-iter-9A ring layout). Either:
- Update those sections to match current iter-9A code (rings still
  named "ForwardResponder" / "CacheDispatcher" instead of plan's
  WriteReceiver / InvalReceiver — easy s/replace)
- OR wait until full 3-ring split (#3 above) lands then rewrite

Recommended: do the easy s/replace now to keep blueprint useful;
full rewrite happens with #3.

---

## How to prioritize iter-10A scope

If iter-10A deadline is < 2 days: Tier 1 only + #11 (re-enable tests)
+ #13 (s/replace blueprint). 2 days.

If iter-10A deadline is 5+ days: Tier 1 + Tier 2 #3-#5 (the structural
N:1:1:N work). 5-7 days.

If iter-10A is meant to push absolute peak above 19.62 Mops/s:
Tier 1 + #3 + #4 + #5 + #7 (lock-free hashmap). The W10 4× residual
+ unbatched per-op ring write are the named candidates.
