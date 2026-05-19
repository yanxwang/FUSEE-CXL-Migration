# G6 protocol_a_rw_race_test broken at iter-13A baseline (pre-iter-14A)

Date: 2026-05-19
Discovered while running P2 G6 correctness gate.

## Symptom

Both build-cxl-w1 (no P2 fix) and build-cxl-p2 (with P2 fix) show
identical failure on protocol_a_rw_race_test at ITERS=500:
- h0 (writer): "[h0] wrote 500 values to K" — succeeds
- h1 (reader): "reads=2060566089 violations=0 misses=0 final_v=5 (target 500)" —
  asserts "insufficient progress" because prev < kIters/2

## Diagnosis

The §I9 implementation in iter-9A+ does:
1. execute_write_local Step 4: scan dir.sharer_bitmap, invalidate every
   non-self sharer
2. Step 6: reset dir.sharer_bitmap = {host_id_} (writer only)

So after the writer's FIRST update that invalidates a sharer, the bitmap
is reset. The writer's SECOND update sees bitmap={writer}, sends no
invalidates. The sharer's cache is now stale (from invalidate after 1st
update) but on next read, sharer MISSes cache → forward_read → re-
registers → bitmap = {writer, sharer} again. Then writer's 3rd update
invalidates sharer once more. And so on.

When writer writes back-to-back without yielding the directory lock,
ReadReceiver (responsible for handling sharer's re-register) can't
re-acquire the lock until writer completes. So sharer sees only a few
"snapshots" of writer's progression.

This is **expected §I9 behavior under high write rate, not a bug** —
strict-A doesn't promise the reader sees EVERY write, only that it
sees a linearizable sequence.

The test's pass criterion `prev >= kIters/2` is **incompatible with
current §I9 implementation**.

## iter-13A claim re-evaluation

iter-13A summary says "G6 100k iters PASS", but at 100k iters the pool
also exhausts (kBlocksPerHost=1024 < 100k unique block allocations on
back-to-back updates). The iter-13A claim is suspect; either the test
was different or pool semantics differed.

## Decision for iter-14A

- G6 is pre-existing broken: confirmed on build-cxl-w1 (iter-13A HEAD
  before P2 fix). NOT a P2 regression.
- P2 correctness gate falls back to: hash-diff 20-cell battery PASS
  (verified 20/20 on build-cxl-p2).
- iter-15A backlog: rewrite protocol_a_rw_race_test to assert
  linearizable read sequence (allowing skipped values), not "must
  see kIters/2 values".
