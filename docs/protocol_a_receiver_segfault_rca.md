# Protocol A receiver-thread terminate/SIGSEGV — RCA + fix

**Date**: 2026-06-13
**Branch**: feat/cxl-migration

iter-21A summary §3 documented a flaky g2 segfault under single-client
cross-host flood; investigation + fix completed as a standalone bugfix
(not under an iter*A label).

## §1 Symptom

g2 process exits 139 (SIGSEGV) or 134 (SIGABRT) shortly after attach
under "g1 single client doing sustained cross-host flood, g2 worker
idle" cell configs:
- Fig 10 single-client large N (10K and 100K)
- Fig 11 c=1 and c=2 cells

Did NOT reproduce at c >= 4.

## §2 Root cause

[tests/protocol_a_ycsb.cc](../tests/protocol_a_ycsb.cc) Fig 10 and Fig
11 branches both `return 0;` directly at the end, **without calling
`store.stop()`** first. The YCSB-path teardown at line 1154 already
had this call; the Fig 10/11 paths added in iter-21A missed it.

Mechanism: each forked worker process inherits 3 `std::thread` members
(write/read/inval receiver) that remain in joinable state. On scope
exit, `~CxlKvStoreA()` finds them joinable -> `std::terminate` ->
SIGABRT (the 134 case). In the worker-mid-INSERT case the terminate
fired while one of the workers was in the middle of a CXL operation,
which manifested as SIGSEGV in some races.

Confirmed by GDB on a 134-exit core (Thread 1 in `~CxlKvStoreA` ->
`std::terminate`; Threads 2-4 still in {write,read,inval}_receiver_loop).
The 139-exit cases are statistically the same bug with a different
exit path.

## §3 Fix

Two-line patch to [tests/protocol_a_ycsb.cc](../tests/protocol_a_ycsb.cc):

```cpp
// Fig 10 path (line ~836):
    store.stop();   // <-- ADDED
    return 0;
  }

// Fig 11 path (line ~990):
    store.stop();   // <-- ADDED
    return 0;
  }
```

## §4 Verification (11 / 11 PASS)

Stress reps at the previously-failing cell configs after the fix:

| Cell | Runs | Pass | Fail |
|---|---|---|---|
| Fig 11 c=1 K=200 | 5 | 5 | 0 |
| Fig 10 N=10 000 | 3 | 3 | 0 |
| Fig 10 N=100 000 | 3 | 3 | 0 |

All exit cleanly with `exit_g{1,2}=0`. Zero op failures. No more
SIGABRT teardown, no more SIGSEGV.

## §5 Unblocked

- Fig 10 ns-resolution re-run (was blocked by SIGSEGV at N=10K and 100K)
  - completed in next commit; new authoritative dataset at
    [docs/protocol_A_fig10_20260613_025510/](protocol_A_fig10_20260613_025510/)
- Fig 11 c=1 / c=2 cells can populate the full sweep that iter-21A had
  to skip (left for user to direct)
- Fig 13 YCSB sweep can include c=1/c=2 cells (was descoped per user
  direction in iter-21A)
