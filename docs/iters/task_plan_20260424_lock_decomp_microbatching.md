# Task plan — lock decomposition + micro-batching (iter 3)

**Context**: iter 1/2 for protocol C write-path
(`docs/task_plan_20260423_c_writepath.md`) brought workload A peak
from 1.08 → 3.27 Mops/s and B from 6.55 → 10.54 Mops/s, but still
2.0–6.1 × below the 20 Mops/s north-star
(`docs/design_goals.md`). Iter 1/2 iteration notes:
`docs/g34_scaling_ycsb_C_only_20260423_051200/iteration_note.md`,
`docs/g34_scaling_ycsb_C_only_20260423_054027/iteration_note.md`.
Fresh decomp
(`docs/latency_decomp_C_iter2_20260423_053919.md`) shows lock p50 is
roughly flat with T (6.3 → 9.0 µs from T=8 → T=64) while lock p99
scales ~27×. Strong circumstantial evidence that the remaining
ceiling is hot-slot **queueing**, not LFM acquire physics, but we
have not proven it at instruction level yet. Iter 3 does that
proof, then moves to the queueing-side optimizations.

## Ground rules

- **`docs/scaling_ycsb_spec.md` is binding** for every scaling sweep
  in this plan:
  - 240-run / 80-run layout for C-only (per §1–3 of the spec),
    bucket count 65 536, 200 k ops cap, 600 s per-run timeout, two
    hosts g3 + g4 with role mode.
  - Output directory pattern `docs/g34_scaling_ycsb_C_only_<ts>/`
    with `SUMMARY.log`, `plot_commit.txt`, 14 cache-on plots,
    14 cache-off plots, and `extra/` overlays (§6).
  - Plots use linear Mops/s, x-axis log-base-2 clients per host (§7).
  - Append one row per run to `docs/scaling_ycsb_runs_index.md` (§10).
- **Commit hygiene** (see `CLAUDE.md`): `feat/cxl-migration`, tag
  prefixes `[read-singleshot]`, `[flush-collapse]`,
  `[decomp-lfm-anatomy]`, `[light-lock]`, `[route-seq]`,
  `[micro-batch]`, `[C-sweep]`.
- **North-star comparison**: every latency / throughput number in
  each phase's output doc must be explicitly compared to the
  20 Mops/s bar, per `docs/design_goals.md` §Analysis discipline.

## Execution order

Risk-ascending, independent-testable. Each step has its own
verification gate; fail-gates trigger rollback, not "push through".

```
┌─ 2.4 remove retry loop ──┐
├─ 2.6 flush collapse ─────┤  all 3 parallelisable against Phase 1
├─ Phase 1 LFM anatomy ────┤
└─ 2.5 route_seq w/ gate ──┘
          │
          ▼
   [Phase-2 80-run sweep]
          │
          ▼
   Phase 3 micro-batching
          │
          ▼
   [Phase-3 80-run sweep + iter-3 iteration_note]
```

---

## Phase 1 — prove LFM acquire is T-independent

**Why**: if Phase 1 fails (acquire itself grows with T), optimisation
effort must first go into replacing LFM, not into queueing-side work.

### 1.1 FUSEE-local instrumented LFM

- New file `src/lfm_lock_fusee_instrumented.c` (symbol-override
  pattern, same as `src/ticket_lock_fusee_patched.c`). Not a
  replacement for the production LFM; built under
  `FUSEE_LFM_INSTRUMENT=ON` into a **third** static library
  `fusee_cxl_lfm_instr` so production binaries never see it.
- Inside `shm_mutex_lock`, add 4 `rdtscp` samples:
  - `t_a` = function entry
  - `t_b` = after `b[host_id] = 1` + fence
  - `t_c` = after the `for j != host_id` peer scan
  - `t_d` = after x/y negotiation, immediately before CS entry
- Publish these 4 deltas into an extended `DecompProbe` with new
  stage ids: `kDecompLfmLocalStore`, `kDecompLfmPeerScan`,
  `kDecompLfmContWait`, `kDecompLfmEnterCS`.

### 1.2 Uncontended baseline

- Extend `tests/cxl_latency_decomp.cc` to report LFM sub-stage
  breakdown at `num_contenders = 1`. This is the physical lower
  bound for the four LFM stages.
- Run once on g3 only (single-host): expected `t_a..t_d` sum
  ~3–5 µs.

### 1.3 Contended decomp at scaling T

- Extend `tests/cxl_latency_decomp_C.cc` to additionally report the
  four LFM sub-stages (piggy-back on the existing DECOMP_C line).
- Run at T ∈ {8, 16, 32, 64, 86}, workload A cache=on,
  2 hosts g3+g4.
- Product: per-T table of `local_store / peer_scan / cont_wait /
  enter_cs` **avg / p50 / p99**.

### 1.4 Verification gates

- **PASS** ⇒ `local_store + peer_scan` (acquire physics) grows
  < 20 % from T=8 to T=86; `cont_wait` grows super-linearly. Conclude
  "acquire is T-independent; queue is the bottleneck." Proceed to
  Phase 2's 2.5 + Phase 3.
- **FAIL** ⇒ acquire-intrinsic also grows with T. Add sub-step 1.5
  (light-lock replacement) before Phase 3.

### 1.5 (conditional) Light-lock replacement

- Triggered only on Phase 1 **FAIL**.
- Replace LFM in `SlotLockTable` with a minimal MCS-style spinlock:
  one cacheline per (bucket, slot), single atomic swap on acquire,
  single CAS on release. Cross-host correctness via clflushopt pair
  on the owner pointer.
- Rebuild, re-run **Phase 1 decomp** on the new lock. Accept only if
  `acquire physics` now grows < 10 % across T.
- **End-of-1.5 sweep (mandatory)**: full 80-run C-only sweep per
  `scaling_ycsb_spec.md`, output
  `docs/g34_scaling_ycsb_C_only_lightlock_<ts>/`, compare vs iter-2
  baseline in `extra/`.

### Phase-1 artefact

- `docs/latency_decomp_C_iter3_lock_anatomy_<ts>.md` with
  uncontended baseline, contended scaling table, and the
  PASS/FAIL conclusion explicitly written.

---

## Phase 2 — low-risk write/read path fixes

All three sub-steps commit independently so we can bisect if any one
regresses. **After 2.4 + 2.6 + 2.5 land (or are rolled back), run one
full 80-run C-only sweep to capture the combined Phase-2 impact.**

### 2.4 Remove `search()` retry loop  *[read-singleshot]*

- Change `src/cxl_kv_ops_C.cc::search` 8-attempt loop → single-shot
  read. x86 aligned u64 load is atomic, so no torn values on the
  slot pair.
- LRC impact: read semantics relax from "consistent snapshot across
  scan window" to "snapshot at some instant during scan". Writer
  visibility guarantees unchanged.
- **Verification**: re-run iter-2 sweep binary on workloads A/B/C/F
  at T=86 cache=on; `r_p99` must drop from current ~ms level to
  < 100 µs on A/B/F, stay ≤ 30 µs on C. Writer thpt must not
  regress more than 2 %.
- **Rollback gate**: any regression > 5 % on peak writer thpt on
  any of A/B/C/D/F → revert commit, open an issue.

### 2.6 Batch flush_line to 2 cachelines  *[flush-collapse]*

- `CxlKvBucket` is 128 B = 2 cachelines. 7 × (key, value) flushes
  currently issue 14 `clflushopt` but target only 2 unique lines
  (slot 0-3 on line 0, slot 4-6 on line 1).
- Change `src/cxl_kv_ops_C.cc` scan/publish/search flush loops
  from 14 clflushopt → 2 clflushopt (one at `&b->slots[0]`,
  one at `&b->slots[4]`).
- **Verification**: decomp `scan` and `search` stages p50 drop by
  50–150 ns; writer thpt +3–5 % on A/B/F.
- **Rollback gate**: any regression or decomp p99 divergence.

### 2.5 `route_seq` skip-second-scan *[route-seq]*

- `SlotLockEntry` gains a new `cacheline_u64 route_seq` field;
  INSERT and DELETE bump it, UPDATE does not.
- Writer flow: read `route_seq` before unlocked pre-scan; after
  `lock_slot`, re-read `route_seq`. If unchanged, skip the under-lock
  `flush_line(&slot.key) + full_fence + verify`; proceed straight to
  publish.
- Applies only to UPDATE (INSERT / DELETE / search unaffected).
- **Verification**: workload A/B/F thpt ≥ +5 % vs the post-2.6
  baseline; workload D regression ≤ 5 % (INSERT-heavy → seq changes
  frequently, may lose).
- **Rollback gate** (strict): if the above does not hold on BOTH A
  and D, revert the commit. Do not merge a net regression.

### Phase-2 consolidation sweep

- After all three sub-steps settle (merged or reverted individually),
  run **one full 80-run C-only sweep** per `scaling_ycsb_spec.md`:
  OPTS="C", WORKLOADS="workloada workloadb workloadc workloadd
  workloadf", THREADS="1 2 4 8 16 32 64 86", CACHE_MODES="on off".
- Output: `docs/g34_scaling_ycsb_C_only_phase2_<ts>/` with the
  full 14 cache-on + 14 cache-off plots + `extra/` 9-plot overlays
  vs baseline + iter1 + iter2 + phase2. `iteration_note.md` per
  spec §6, `plot_commit.txt` per spec §5, append to
  `docs/scaling_ycsb_runs_index.md` per spec §10.
- **Gate to Phase 3**: document current peak on A/B/F, recompute
  gap to 20 Mops/s, mention which of the three sub-steps
  contributed what (quantified).

---

## Phase 3 — per-bucket-per-host micro-batching  *[micro-batch]*

**Why**: Phase 1 has proven (if PASS) that acquire-intrinsic is not
the bottleneck. Phase 2 has reclaimed the obvious engineering
slack. The remaining gap is the **per-op cross-host CXL epoch bump
(~3 µs/op)** funneling writers through one hot slot. Batching K
writes into one bump amortises this over K.

### 3.1 Design

**Per-host DRAM ring (NOT CXL-shared).** Key observation: only the
owner host's clients append to the ring and only its own flusher
drains it — peer host only ever reads materialised slots + write_epoch.
So the ring body does not need to be on CXL; keeping it on host-local
DRAM saves the ~3 µs CXL roundtrip per append that micro-batching
is supposed to eliminate.

- Allocation: `mmap(nullptr, bytes, PROT_READ|PROT_WRITE,
  MAP_SHARED|MAP_ANONYMOUS, -1, 0)` done by each host's primary
  client **before fork**, so all fork children inherit the same
  virtual address.
- Layout: row-major `RingEntry ring[num_buckets][K]`, 16 B per entry.
  K is **runtime** (from env `FUSEE_BATCH_K`, default 32). Memory at
  K=256 ≈ 16 B × 256 × 65 536 = 256 MiB per host — acceptable on
  86-core nodes with hundreds of GiB RAM.
- Ring entry (16 B): `{uint16_t slot_idx; uint16_t _flags;
   uint32_t seq; uint64_t new_value}`. Key is implied by slot_idx
  (UPDATE does not change key).
- Cursors (also host-local DRAM, in the same mmap region):
  `atomic<uint64_t> append_cursor[num_buckets]`,
  `uint64_t flush_cursor[num_buckets]`. Wrapped into a separate
  cacheline-padded struct to avoid false sharing.
- **SlotLockEntry / CxlKvBucket CXL layout stays unchanged.** The
  existing `staging_scratch` cacheline is free to be deprecated later
  (we do not need it for this scheme).
- Flusher: one thread per host, woken by a dirty-bucket queue (see
  §3.3 Flusher model) or a T-timer.

Rationale for choosing per-host DRAM ring over the originally
proposed staging_scratch inline variant (or a CXL-shared ring region):

| choice                                 | K runtime | per-append cost | Phase-3.7 sweep cost  | peer visibility | memory |
|----------------------------------------|-----------|-----------------|-----------------------|-----------------|--------|
| inline in SlotLockEntry (compile-time K) | no      | ~100 ns local   | recompile 7× builds   | owner only      | 4 KiB/bucket at K=256 = 256 MiB |
| separate CXL-shared ring region        | yes       | ~3 µs (CXL RT)  | one build             | both hosts can see | 256 MiB on CXL |
| **per-host DRAM ring (selected)**      | yes       | ~100 ns local   | one build             | owner only (fine — peer reads materialised slots) | 256 MiB/host DRAM |

(Phase-3.7 sweep cost column refers to batch-size sweep in §3.8
which iterates 7 K-values × 4 T-values; "recompile" in the inline
row means rebuilding the slave binary 7 times.)

### 3.2 Write protocol (UPDATE fast path, no slot-lock)

**Design choice**: UPDATE goes through the ring without taking
`slot-lock`. Rationale:

- Same-slot concurrent writers each `fetch_add` a unique position in
  the ring and write their own entry — no data race.
- The flusher replays ring entries **in cursor order** into
  `slot.value`; the final state is the last-committed writer's value,
  which matches "last writer wins" UPDATE semantics.
- The slot-lock was originally needed to serialise same-slot writers
  on INSERT (dup check) and DELETE (existence check). UPDATE's only
  serialisation need is "somebody wins" — which atomic cursor order
  already provides.

**INSERT / DELETE keep the iter-2 per-op path** (slot-lock +
under-lock dup/existence verify + publish + bump_epoch + unlock).
Rationale:

- INSERT requires dup check, which must see both materialised slots
  AND pending ring entries — scanning the ring under a lock is more
  expensive than just doing the per-op path.
- Workload A/B/F trans phase has no INSERT and no DELETE, only
  UPDATE + READ — so the fast path covers 100 % of the validation
  workload writes.
- Workload D (INSERT-heavy) tolerates the per-op path — it already
  hits 38 Mops/s without batching.

```
writer.update(K, v):
  idx = bucket_idx(K)
  slot_idx = locate_slot_for_key(idx, K)   # unlocked pre-scan
  if (slot_idx < 0) return -1               # key not found
  pos = __atomic_fetch_add(&append_cursor[idx], 1, ACQ_REL)
  while (pos + 1 - flush_cursor[idx] > K_capacity) {
      # ring full: wait for flusher; simple blocking (§3.2.1)
      cond_wait(ring_not_full[idx])
  }
  ring[idx][pos % K_capacity] = {slot_idx, seq=pos, value=v}
  cache_buckets_[idx].slots[slot_idx].value = v   # read-your-writes
  if ((pos + 1 - flush_cursor[idx]) >= K_trigger)
      dirty_queue.push(idx)
  return 0
```

Flusher drain (separate thread, see §3.3):

```
flusher.drain(idx):
  flush_lock(idx)              # per-bucket flush-lock, distinct from slot-locks
  applied_end   = __atomic_load(&append_cursor[idx], ACQUIRE)
  applied_start = flush_cursor[idx]
  # §3.7 same-key merge
  merged = collapse_by_slot(ring[idx], applied_start, applied_end)
  for (slot_idx, value) in merged:
      buckets[idx].slots[slot_idx].value = value
  flush_line(&buckets[idx].slots[0])    # cacheline 0 (slots 0-3)
  flush_line(&buckets[idx].slots[4])    # cacheline 1 (slots 4-6)
  store_fence()
  bump_epoch(entry(idx))                # single atomic fetch-add + clflushopt
  flush_cursor[idx] = applied_end
  cond_notify_all(ring_not_full[idx])
  flush_unlock(idx)
```

### 3.2.1 Backpressure on ring full

- Initial implementation: **blocking**. Writer waits on a condvar
  signalled by flusher when it advances `flush_cursor`. Simple and
  preserves correctness.
- If Phase 3.8 sweep observes backpressure dominating writer p99
  at common K values, add a fallback: when ring would overflow,
  the writer falls back to the iter-2 per-op path
  (`lock_slot + publish + bump_epoch + unlock_slot`) instead of
  blocking. Adds complexity — do it only if measured.

### 3.3 Flusher model — one thread per host + dirty-bucket queue

**Do NOT** scan all 65 k buckets on every K-trigger — that alone
would cost ~65 µs per scan.

- Per-host lock-free MPMC queue `dirty_queue` (Vyukov-style bounded
  ring, ~1024 capacity, local DRAM).
- On K-trigger: appending writer pushes `bucket_idx` onto the queue
  and does `cond.notify_one()`.
- Flusher loop:

```
flusher.run():
  while (!stop):
    idx = dirty_queue.pop_or_wait(T_flush_µs)
    if idx != NONE:
      drain(idx)                   # K-triggered, O(1) pop
    else:
      # timer woke us, do a T-trigger pass
      for idx in 0..num_buckets:
        if append_cursor[idx] > flush_cursor[idx]:
          drain(idx)
```

- **Start with 1 flusher thread per host.** Utilisation estimate at
  3 M writes/s, K=32, drain cost ~3 µs: ~94 k drains/s × 3 µs ≈
  28 % of a core. Enough headroom.
- If Phase 3.8 shows flusher saturating (append_cursor keeps
  growing, backpressure frequent), shard into a flusher pool:
  N threads, each owning `bucket_idx % N`. Separate per-flusher
  dirty queues. No redesign, just scale out.
- Per-bucket flusher threads (65 k threads) is **not** viable —
  kernel thread struct alone is multi-GB.

### 3.4 Read-your-writes

- Same-client search() must overlay pending ring on top of
  materialised slot. Either: (a) writer updates its `cache_buckets_`
  slot immediately on ring append (already the iter-2 behaviour),
  or (b) search() explicitly walks the ring first.
- Choose (a): cheaper, keeps search fast path unchanged.

### 3.5 Peer-host visibility

- Peer host's `search()` looks at materialised slots + seqlocks on
  write_epoch. Pending-but-unflushed writes on owner host are
  invisible to peer. Formal staleness bound:
  `peer_visibility_lag ≤ T_flush + cxl_epoch_latency (~3 µs)`.

### 3.6 Crash recovery

- **OpLog commits before ring append** (not after). This preserves
  the existing redo-on-restart invariant: every completed update
  call has a corresponding InProgress or Committed oplog entry;
  recovery either re-applies or discards.
- On restart, run normal `recover_from_oplog()`; then drain any
  residual ring into slots. Because oplog `InProgress` state
  indicates "may or may not be applied," recovery can safely
  re-apply the ring (idempotent by slot_idx + key).

### 3.7 Same-key merge within batch  *(ON by default)*

- `FUSEE_BATCH_MERGE_SAME_KEY=ON/OFF` compile flag, default ON.
- At flush time, scan the ring for (slot_idx, latest_value) per
  slot — only apply the **last** write to each slot. Older entries
  for the same slot become no-ops.
- For workload A, expected compression ~10–15× (hottest key gets
  ~0.2 K hits per batch; at K=32 → ~6 collapsed to 1).
- LRC impact: intermediate values never become peer-visible.
  Equivalent to strengthening the staleness bound's "you may see
  stale, but at least you see something that was committed" to
  "you may miss intermediate values entirely, only final ones
  guaranteed." Must be called out in the staleness doc update.
- Comparison point: both `OFF` and `ON` runs at the chosen (K, T)
  after the sweep, one full 80-run each.

### 3.8 Batch size sweep

**Goal**: find the (K, T) corner that maximises workload-A writer
throughput without exceeding a target peer-visibility bound.

- **Starting point**: `K=32, T=100 µs`.
- **OAT sweep phase A — K (fix T=100 µs)**:
  `K ∈ {4, 8, 16, 32, 64, 128, 256}` — 7 points, geometric ×2.
  Memory at K=256 ≈ 16 B × 256 × 65 536 = **256 MiB** per bucket
  table, acceptable.
- **OAT sweep phase B — T (fix K = best-from-A)**:
  `T ∈ {10 µs, 100 µs, 1 ms, 10 ms}` — 4 points, geometric ×10.
  Lower bound 10 µs ≈ 3× cxl_epoch_latency (meaningful batching
  only if flush cost amortises across ≥ 3 bumps worth of work).
- **OAT sweep phase C — fine-tune**: if best corner is mid-range,
  one additional pass of K near best (±2, geometric) and T near
  best (±3, geometric).
- **Each sweep cell**: workload A cache=on, 2 hosts, T=32 clients,
  200 k ops — ~1 s of wall-clock per run, so total sweep cost
  (7 + 4 + ~5) × 2 cells (MERGE_SAME_KEY ON / OFF) ≈ 32 cells,
  ~40 s wall-clock.
- **Metrics reported per cell**: writer thpt (ops/s), writer p99
  (µs, writer-side only — backpressure sensitive), peer visibility
  lag p99 (observed via an auxiliary synthetic client that reads a
  recently-written key and records the lag), memory occupied by
  rings.
- Artefact: `docs/latency_decomp_C_iter3_microbatch_sweep_<ts>.md`
  with the (K, T, merge) → metric table + heat plot.

### 3.9 Phase-3 final 80-run sweep

- Using best (K, T, MERGE) discovered in 3.7, run one full 80-run
  C-only sweep per `scaling_ycsb_spec.md`. Output
  `docs/g34_scaling_ycsb_C_only_microbatch_<ts>/` with all spec
  artefacts.
- `extra/` overlay: baseline → iter1 → iter2 → phase2 → micro-batch
  (5 lines per workload).

### Phase-3 verification gates

- **A, B, F must each hit ≥ 20 Mops/s at some T** with the chosen
  (K, T, merge) — per task north-star. If any of the three misses,
  record the remaining gap, hypothesise causes, do NOT silently
  accept.
- **C and D**: throughput may change (the micro-batching path is
  UPDATE-centric; INSERT path for D goes through the batching
  layer too). Acceptable regression threshold: ≤ 10 % on C/D vs
  iter-2. Beyond that → treat as rollback trigger for the merge
  flag, and rerun.

---

## Phase 4 — finalisation

- `docs/g34_scaling_ycsb_C_only_microbatch_<ts>/iteration_note.md`:
  structured per iter-1/iter-2 notes (optimisation landed, peak
  numbers vs baseline / iter1 / iter2 / phase2 / phase3, gap to
  20 Mops/s, decomp reconciliation).
- `docs/scaling_ycsb_runs_index.md`: append one row per sweep
  produced in this iter (Phase 2 consolidation, Phase 1.5 if
  triggered, Phase 3 batch-size sweep result headline, Phase 3
  final).
- `docs/fusee_cxl_progress.md`: append a "2026-04-24 iter 3"
  section with what landed, the 20 Mops/s achievement / gap, and
  any follow-on work.
- `docs/design_goals.md` (if Phase 3 lands): add a short
  "Protocol-C under micro-batching" section documenting the
  relaxed LRC bound formally.

---

## Time budget (indicative)

| Phase | est. wall | notes |
|---|---|---|
| 2.4 (read-singleshot) | 0.3 h | ~10 lines + decomp validation |
| 2.6 (flush-collapse)  | 0.3 h | ~15 lines |
| Phase 1 (LFM anatomy) | 2 h   | new instrumented library + microbench + contended decomp |
| 1.5 (light-lock)      | +2 h  | only if Phase 1 FAIL |
| 2.5 (route_seq)       | 1 h   | with gate |
| Phase-2 sweep + note  | 0.7 h | 80 runs ≈ 15 min + docs |
| Phase 3.1–3.7 impl    | 5 h   | ring + flusher + oplog + merge + tests |
| 3.8 batch sweep       | 0.5 h | ~40 s each × 32 cells + analysis |
| 3.9 final 80-run      | 0.5 h | sweep + plots |
| Phase 4 docs          | 1 h   | iteration_note + index + progress + goals |

Total 11–14 h including the conditional 1.5 branch.

## Ground rules restated

- Every intermediate decomp doc compares numbers against the
  20 Mops/s bar explicitly.
- Every scaling sweep follows `docs/scaling_ycsb_spec.md` exactly —
  directory layout, plot set, SUMMARY.log line format, runs-index
  append.
- Commits live on `feat/cxl-migration` with the prefix tags above.
- Per-phase rollback gates are strict — net regressions never merge.
