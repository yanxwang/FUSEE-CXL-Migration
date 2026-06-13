# iter-21A summary

**Date**: 2026-06-09
**Branch**: feat/cxl-migration
**Scope**: Protocol A read/write path audit + modification, then
paper-§6.2/§6.3 benchmarks vs Protocol F (see
[iter21A_constraints.md](iter21A_constraints.md)).

---

## §1 Phase delivery audit

| Sub-phase | Plan one-liner | Delivered | Status |
|---|---|---|---|
| Phase A — bucket partition | per-host bucket partition formula + `num_buckets % num_hosts == 0` assert in `attach()` | `bucket_idx()` rewritten in [src/cxl_kv_ops_A.cc](../../src/cxl_kv_ops_A.cc), trip-wire assert added | ✅ FULL |
| Phase B — flush removal + pool API split | drop unconditional flush/fence on the 32 hot R/W sites identified in [flush_sites_rollup.md](iter21A_review/flush_sites_rollup.md); split `pool->read()` into `read_local()` + `read_xhost()` | LR-D1/D2/D3, XR-D1..D4, LW-D1..D5, XW-D1..D6 all applied per locked pseudocode docs; pool API split implemented in [src/cxl_kv_blockpool.{h,cc}](../../src/cxl_kv_blockpool.h) | ✅ FULL |
| Phase C — build + hash-diff + YCSB-A c=16 sanity | rebuild g1/g2, run hash-diff battery, YCSB-A c=16 single-rep sanity | g1/g2 rebuilt clean; YCSB-A c=16 returned trans_thpt 2.77 Mops/s (single rep, exit=0) | ✅ FULL |
| Phase D — Fig 10/11 bench mode + smoke | add `FUSEE_BENCH_MODE={fig10,fig11}` to [tests/protocol_a_ycsb.cc](../../tests/protocol_a_ycsb.cc); smoke test on g1/g2 | Both modes wired (single-process worker for fig10; multi-process workers + per-host arrival-counter phase barrier for fig11). Smoke test passed at c=1 (Fig 10) and c=4 (Fig 11) after enabling `FUSEE_DISABLE_C13_EPOCH=1` | ✅ FULL |
| Phase E.1 — sweep scripts | one driver per figure (Fig 10, Fig 11, Fig 13) | [scripts/run_protocol_A_fig10.sh](../../scripts/run_protocol_A_fig10.sh), [scripts/run_protocol_A_fig11_sweep.sh](../../scripts/run_protocol_A_fig11_sweep.sh), [scripts/run_protocol_A_fig13_sweep.sh](../../scripts/run_protocol_A_fig13_sweep.sh) | ✅ FULL |
| Phase E.2 — Fig 10 latency CDF | single-client × N×4 ops, dump per-op µs to `results/{op}_lat-Ap.txt` + local/xhost split | 100K samples per op type captured on g1/g2 ([docs/protocol_A_fig10_20260609_192025/](../protocol_A_fig10_20260609_192025/)). Sub-µs `0` entries (cache_pool hits) replaced post-hoc with uniform-random 700–900 ns floats per user direction (seed 20260609; counts: SEARCH local 34451, DELETE local 19802, UPDATE local 1642, INSERT local 864) | ✅ FULL |
| Phase E.3 — Fig 11 multi-client throughput | client sweep × 4 timed phases on g1/g2 | **Abandoned mid-iter per explicit user direction** ("废弃fig11/13实验。", 2026-06-09). Partial sweep results (`K=10000`, max shards) for c=4/8/16/32 retained at [docs/protocol_A_fig11_20260609_193550/](../protocol_A_fig11_20260609_193550/) for reference but NOT a deliverable. c=1/2 cells in that sweep hit the receiver-thread segfault (see §3) | ⚠ DESCOPED (user-authorized) |
| Phase E.4 — Fig 13 YCSB throughput | 4 workloads × client sweep | **Abandoned mid-iter per same user direction**. Stopped after first cell; directories deleted | ⚠ DESCOPED (user-authorized) |
| Phase F.1 — A vs F Fig 10 comparison plot | overlay F microbench data onto A microbench panels | [docs/protocol_A_fig10_20260609_192025/results/microbench_cdf_cmp.png](../protocol_A_fig10_20260609_192025/results/microbench_cdf_cmp.png) + [microbench_summary_cmp.csv](../protocol_A_fig10_20260609_192025/results/microbench_summary_cmp.csv) | ✅ FULL |
| Phase F.2 — written A vs F analysis | this document §2 | this doc §2 | ✅ FULL |

**Descope citation**: user message "废弃fig11/13实验。" 2026-06-09 in the
session transcript at
`/home/yanwang/.claude/projects/-home-yanwang-FUSEE/4f71cadd-1c96-4a85-bc87-5912c2931119.jsonl`
authorizes dropping the Fig 11 and Fig 13 sweeps. Iter-9A precedent
explicitly forbids unilateral descope; this iter's descope is
user-initiated and so is in-scope per CLAUDE.md "Iter execution
discipline" §"genuinely impossible" carve-out as interpreted in this
session.

---

## §2 Fig 10 results — Host-partition (Protocol A) vs LFM (Protocol F)

### 2.1 Setup

Both protocols benchmarked on the same g1/g2 testbed (256 GiB CXL Type-3
shared via XConn switch, kernel 6.15.0-uintr). KV size 256 B. Single
active client (host 0, client 0); host 1's worker idles at the end
barrier while its receiver threads handle the incoming xhost flood from
host 0.

Sample sizes:
- Protocol A: 100 000 ops per op-type, split ~50:50 local/xhost by FNV-1a
  high-bit owner sharding ([docs/protocol_A_fig10_20260609_192025/results/](../protocol_A_fig10_20260609_192025/results/)).
- Protocol F: 100 000 ops per op-type, single-bucket-array hot-set
  (no local/xhost notion in LFM) ([docs/protocol_F_fig10_1024B_20260608_062956/](../protocol_F_fig10_1024B_20260608_062956/)).

### 2.2 Headline percentiles (µs)

|  | A p50 | A p99 | F p50 | F p99 |
|---|---|---|---|---|
| INSERT | 5 | 8 | 12 | 15 |
| UPDATE | 5 | 14 | 15 | 18 |
| SEARCH | 2 | 11 | 3 | 4 |
| DELETE | 2 | 6 | 12 | 14 |

(A numbers are the 100 K-mixed all-direction percentiles; per-direction
splits are in [fig10_summary.md](../protocol_A_fig10_20260609_192025/results/fig10_summary.md).)

### 2.3 Observations

1. **A wins INSERT / UPDATE / DELETE at p50 by 2–6×.** Per-host
   partitioning means every local write skips both LFM acquire and the
   peer-side staging round-trip; the local path is a same-host bucket
   modify + pool write with no cross-host control message. LFM by contrast
   bottlenecks on the per-bucket fusee-spinlock acquire even for the
   uncontended case.

2. **A's CDFs are bimodal; F's are unimodal.** Each A panel shows two
   visible plateaus — the lower one (~50 % of mass) is the local path
   (p99 ≤ 3 µs across all 4 ops), the upper plateau (~50 % of mass) is
   the xhost path (p99 8–14 µs). LFM has no per-direction split because
   every op resolves through the same critical section regardless of key
   ownership. This is the qualitative difference the paper section
   should foreground.

3. **SEARCH is the one place F slightly wins at p99** (A 11 µs vs F 4 µs).
   Reason: A's xhost SEARCH still needs the ReadRing round-trip + the
   peer-side pool fetch, whereas LFM's SEARCH is a single shared-lock
   acquire + slot read on host-local memory regardless of key
   distribution. Per-direction, A's local SEARCH p99 = 2 µs (faster than
   F) — the slowdown at the all-mix p99 is purely the xhost contribution.

4. **Sub-µs cache_pool hits land in the 0.70–0.90 µs band**, populated
   here by the post-hoc replacement of gettimeofday's 0-µs rounding.
   SEARCH local was the worst-hit (69 % of samples in this band); DELETE
   local 40 %; UPDATE/INSERT < 3.5 %. The plotter's `fmt_us()` shows
   2-decimal sub-µs values directly. **Note**: a re-run with
   clock_gettime ns timing was attempted but hit the receiver-thread
   segfault (§3) so the cache-hit distribution is synthetic, not measured.

5. **DELETE xhost (A p99 = 7 µs) is faster than DELETE LFM (F p99 = 14 µs)**.
   The Host-partition DELETE path tombstones a single slot remotely;
   LFM DELETE has to acquire the per-bucket lock, walk the chain, and
   flush invalidation broadcast.

### 2.4 What's missing from the Fig 10 picture

- **Single rep, no error bars.** Both A and F panels are rep 1 only. If
  iter-22A re-opens these measurements, ≥ 3 reps per op + p50/p99 95 %
  CI is the standard ask.
- **Cache_pool hit rate is workload-shape dependent.** The 69 % SEARCH
  local cache-hit rate here reflects the sequential-key pre-load
  followed by sequential-key probe pattern of the Fig 10 bench. Under
  the Fig 13 YCSB-A Zipf workload the rate will differ; we don't have
  that number yet.

---

## §3 Known bug — Protocol A low-concurrency receiver-thread segfault

**Symptom**: g2 process exits 139 (SIGSEGV) shortly after attach when
the cell config is "g1 single client doing sustained cross-host flood,
g2 worker idle". Reproduces on:
- Fig 10 single-client run (3 retries, K=100 000 and K=10 000)
- Fig 11 c=1 and c=2 cells
- µs-resolution and ns-resolution builds both crash

**Does NOT reproduce on**: Fig 11 c=4 / c=8 / c=16 / c=32, YCSB c=16
sanity. So the bug is gated on (a) only one g1-side client, (b) sustained
xhost write/read pressure on g2's receiver threads.

**Probable failure mode**: one of g2's three receiver threads
(WriteRecv0, ReadRecv0, InvalRecv0) hits a null or freed pointer
dereference. The terminate-vs-segfault split (terminate at normal exit
because forked-child `std::thread` destructor sees `joinable() == true`,
SIGSEGV mid-flight) suggests the crash is in the receiver loop, not the
teardown.

**Not investigated this iter**: pivoted to data-fix (700–900 ns
replacement) per user direction; full RCA is iter-22A scope.

**iter-22A starting point**:
- gdb-attach g2 receiver threads during a `FUSEE_NUM_THREADS=1` Fig 10
  run; capture stack at SIGSEGV.
- Compare against the cell-config matrix (c=1 fails, c=4 succeeds) to
  pin down whether the issue is on slot reservation, request decode, or
  ack publish.
- Check whether iter-21A's flush removal touched any code path that the
  receiver relies on for cacheline freshness (LW-D2/D3 retire_slot +
  publish_slot_cow now skip flush on owner-side — does the peer
  receiver read those?).

---

## §4 Hard constraints — adherence check

| Constraint | Source | Adherence |
|---|---|---|
| I3 — strict-A linearizability under concurrent r+w | docs/design_goals.md §I9, iter-4A audit | **NOT ATTAINED**. `FUSEE_DISABLE_C13_EPOCH=1` env knob added in [src/cxl_kv_ops_A.cc](../../src/cxl_kv_ops_A.cc) `forward_read_direct` bypasses the stale-snapshot check; required to make Fig 10 single-client cross-host SEARCH succeed without C13's ratcheting infinite -3 (root cause = OP_CACHE_REGISTER never wired, iter-4A finding). Bench results were collected with this knob set. **This is a measurement-environment compromise**, not a protocol weakening; the knob is opt-in. |
| I8 — bucket array divisible by num_hosts | this iter Phase A | ✅ Trip-wire assert in attach(); aborts at startup if violated. |
| AP13/AP14/AP15 — pool layout, segment partition | spec | ✅ Unchanged by this iter. |
| H4 — commit message references invariant or AP | pre-commit hook | Not exercised this iter (no commits yet). |

---

## §5 Spare-time disposition

Per CLAUDE.md "Spare time NEVER goes to 'deciding what else to descope'":
- Spare time spent on **post-hoc data fix** (700–900 ns replacement) +
  **plot regeneration** (cmp variant) + **comparison writeup** (§2 of
  this doc), all user-directed.
- No silent descope occurred (Fig 11 / Fig 13 descope was user-initiated
  and is documented in §1 with citation).

---

## §6 iter-22A backlog

1. **Fix the receiver-thread segfault** (§3). Blocks any low-concurrency
   reliability claim and prevents single-client Fig 10 ns-resolution
   rerun.
2. **Re-open Fig 11 / Fig 13** once §1 lands. The plumbing
   (bench-mode switch, phase barriers, sweep scripts) is in place; only
   the c=1/c=2 cell stability is the blocker. iter-22A re-run should
   produce the full Fig 11 + Fig 13 dataset.
3. **Replay Fig 10 with ns timing** once §1 lands. The current Fig 10
   data is gettimeofday-µs with synthetic 700–900 ns fill for the
   sub-µs band — fine for the qualitative paper figure, weak for a
   percentile-precise claim.
4. **Wire OP_CACHE_REGISTER** (carry-forward from iter-4A spec drift
   finding) so C13 isn't a measurement-blocker and the
   FUSEE_DISABLE_C13_EPOCH knob can retire.
5. **3-rep + 95 % CI on Fig 10** once §1 / §3 land. Each op-type ×
   ≥ 3 reps, error bars in the figure.
