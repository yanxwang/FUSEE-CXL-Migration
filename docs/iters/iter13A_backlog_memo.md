# iter-13A backlog memo (from iter-12A)

**Date**: 2026-05-16 (drafted alongside iter-12A summary)
**Source**: `docs/iters/iter12A_summary_20260516.md`

iter-12A delivered the bimodal fix (gate-12 PASS with margin: 0 / 13 BIMODAL). With that data-reliability blocker removed, iter-13A's optimization aims become data-driven and pivotable.

---

## Mandatory carry-over

### 1. Phase 3 2nd-round path_decomp on iter-12A new-best cells

iter-11A and iter-12A both deferred this. Rationale for iter-12A deferral: Phase 1.7 gate-12 PASS + Phase 2 R3 stable evidence made the original motivation (validate fix at decomp level) optional. iter-13A picks up: with Phase 4 sweep complete, pick new-best cells per workload (5 cells × healthy + worst), build with `FUSEE_PROBE=1`, run path_decomp, compare against `docs/path_decomp_iter11A_20260511_023247/` to identify the new dominant stage post-bimodal-fix. Output target: `docs/path_decomp_iter12A_<ts>/`.

Implementation note: use `scripts/iter11A_5wl_pathdecomp.sh` template, adapt to iter-12A's new-best cells from Phase 4 sweep.

### 2. Parallel inval drain redesign (iter-11A backlog #8 carry-over)

iter-11A Phase 2 shipped + reverted parallel inval (w_p99 26× regression). Canonical redesign: bucket-affinity sharded queues (each InvalWorker drains a fixed set of buckets, no cross-shard contention). Still a relevant optimization for write-heavy workloads (a, b, d, f).

### 3. Hot-key replication (iter-11A backlog #6 carry-over)

For Zipf workloads (a, d, f), the most-accessed keys dominate cross-host invalidate traffic. Replicating the hot top-K keys at each host (with TTL-based eventual consistency or §I9-compatible epoch ordering) would remove the cross-host bottleneck for hot keys without breaking strict-A semantics.

---

## Open from iter-12A

### 4. Parent-CPU-0 jitter mitigation (escalation past QR2 L1)

The Phase 1.6 fix at the receiver level reduced bimodal collapse rate from ~40% to ~5% (1/20 reps still has occasional hard-runaway in reference cell). The residual is driven by parent worker on CPU 0 occasionally needing > 5 ms to publish (held by current `kBudgetUs=5000`).

Two iter-13A options (per QR2 escalation hierarchy):
- **L2 systemd cpuset shielding** — `AllowedCPUs` in `/etc/systemd/system.conf` carves a shielded core set for protocol_a_ycsb. No reboot needed; needs root systemctl daemon-reexec.
- **L3 kernel-cmdline isolcpus + nohz_full** — most thorough; requires `/etc/default/grub` edit + `update-grub` + reboot. Risk: bootkill possible (PXE rescue available).

Both need user explicit go-ahead (per QR2 "L2/L3 在本 iter 之后向我确认").

### 5. kBudgetUs / receiver gap-budget Pareto sweep

Phase 1.6 picked `kBudgetUs=5000` (5 ms) + 4096-iter receiver budget (~2.8 ms) empirically. A small parameter sweep along (timeout, budget) might find a Pareto-better point. Low priority — gate-12 already passes.

---

## Held items from iter-11A backlog (still applicable)

### 6. RCU cache_pool (iter-11A backlog #7 carry-over)

W10 stage (write to local cache pool) wasn't dominant in iter-11A Phase 5 path_decomp; iter-12A Phase 3 (deferred) would re-check whether that's still true post-bimodal-fix. Conditional on data from item 1.

### 7. Probe-overhead attribution (iter-11A backlog #6 carry-over)

Quantify how much of the FUSEE_PROBE=1 vs FUSEE_PROBE=0 throughput delta is "actually measured per-stage cost" vs "instrumentation overhead". Useful for sweet-spot configuration.

---

## Constraints carry-over

- **C16 observe-first** + **C17 narrow-targeted fix** + **C18 commit-level bisect** are now CLAUDE.md cautionary precedent #4 + iter-12A spec entries. Apply to all subsequent iters.
- §I9 strict-A linearizability — non-negotiable; every protocol change requires G1 hash-diff PASS.
- 20 Mops/s target — still binding on workload-a AND workload-c per `docs/design_goals.md`.

---

## What iter-13A should NOT do

- **No new architecture absent data** — Phase 1.7 + Phase 2 evidence from iter-12A removed the easy "hot fix" surface area. New optimization must be measured-then-justified, not hypothesized.
- **No "fix N bugs at once"** — C17 narrow-targeted is now permanent. Each iter ships ≤ 3 file × function changes per problem.
- **No "verification cell" measurements without per-cell 5-rep reproduction** — iter-11A Phase 1 lesson (1.4 µs claim unreproducible). Headline numbers must be 5-rep medians.

---

## Suggested iter-13A first task

**Phase 3 path_decomp on iter-12A new-best cells, then decide between (2) parallel inval or (3) hot-key replication based on which stage dominates the post-fix bottleneck**. Both are big architectural changes; let the data pick.
