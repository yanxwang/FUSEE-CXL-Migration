# P2 Decision — ROLLBACK

**Fix ID**: F1
**Date**: 2026-05-19
**Plan**: [task_plan_iter14A.md](../iters/task_plan_iter14A.md) Phase 2

---

## Result: rollback (target regressed, no measurable win)

| Cell role | Cell | w1 median Mops/s | p2 median | delta% | CI 95% (delta%) | Verdict |
|---|---|---:|---:|---:|---|---|
| **target** | workloada T=4 cache=off kv=1024 | 1.530 | 1.328 | **-13.18%** | [-13.33, -4.58] | **REGRESS** (CI excludes 0) |
| **target** | workloada T=32 cache=off kv=1024 | 10.238 | 10.251 | +0.13% | [-4.05, +2.86] | no measurable change |
| **target** | workloada T=64 cache=off kv=1024 | 10.801 | 10.606 | -1.80% | [-5.67, +3.28] | no measurable change |
| guardrail | workloadc T=64 cache=on kv=1024 | 18.116 | 18.624 | +2.80% | [-4.02, +10.85] | OK (no regress) |
| guardrail | workloadb T=64 cache=on kv=256 | 17.986 | 18.354 | +2.05% | [-7.64, +14.19] | OK |

Per universal fix policy:
- Target threshold = median +5% AND CI lower bound > +1%
- All 3 target cells FAIL (T=4 regress, T=32/64 flat-within-noise)
- → **rollback**

## Why predicted +5-15% didn't materialize

The RAP assumed: each cross-host write that involves "A was a sharer"
saves ~5 µs invalidate roundtrip. Reality on workload-a Zipf cache=off:

1. **sharer_bitmap reset by every write** (existing baseline behavior).
   After execute_write_local commits, `sharer_bitmap = (1 << owner)`.
   So most cross-host writes find bitmap=={owner} → broadcast skipped
   ALREADY at baseline → no roundtrip to save → P2 self-inval is pure
   added cost.
2. **At T=4 (low T)**, self-inval overhead (~200 ns: 2 atomic stores +
   bucket epoch bump) dominates throughput per worker. With 4 workers
   each issuing ~375K cross-host ops/s, ~75 ns is ~30% of effective
   write critical path → -13% throughput hit.
3. **At T=32/64**, contention dominates throughput; self-inval cost is
   amortized but the savings (≈ rare invalidate roundtrips) is too
   small to register above noise.

## What this teaches

The RAP underestimated how often `sharer_bitmap` is actually
NON-EMPTY (containing the writer) at the moment of a cross-host write.
In iter-9A+ §I9 behavior, sharer_bitmap is reset to {owner} after
every write commit → most writes have nothing to invalidate. The
"writer was a sharer" case requires the writer to have done a
register-then-fill READ since the last write — which is rare on
write-heavy hot Zipf keys.

To make the optimization pay, we'd need either:
- A different sharer_bitmap policy (don't reset to {owner}; keep
  prior sharers and rely on individual stale flags)
- Workloads with low write/read ratio on hot keys (workload-d
  insert-only?)

## Status

- F1 fix flag `FUSEE_XHOST_WRITE_SELF_INVAL` default OFF in CMake.
- Code retained in tree (no git revert) for future re-evaluation.
- `build-cxl-p2` directory may be removed in P3 cleanup OR kept for
  reference. Decision: keep until iter end.
- Forward to iter-15A backlog: "P2 self-inval revisit — sharer_bitmap
  retention design needed to make the saving real."
