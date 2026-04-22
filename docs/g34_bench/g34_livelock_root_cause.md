# g3/g4 "LFM N≥3 livelock" — actual root cause (2026-04-22)

Previous doc `g34_lfm_finding.md` blamed Lamport's Fast Mutex for the
infinite hang observed at `num_hosts=3` (and N=4 on the older kernel).
After a fresh kernel 6.15.0 and careful instrumentation, **the real
cause is not in LFM at all**: it is a fork-mode initialization race
that happened to make the bench's first LFM acquire sit in an
impossible state.

## What we actually saw

With `FUSEE_TRACE=1` in `cxl_kv_bench_mp`:

```
[h1 t=12839.049s] region mapped size=16777216
[h1 t=12839.049s] non-primary waiting init_done
[h1 t=12839.049s] non-primary saw init_done; attaching        ← WRONG
[h1 t=12839.049s] non-primary attach done
[h1 t=12839.049s] set attached=1
[h1 t=12839.049s] all attached                                 ← WRONG
[h0 t=12839.049s] region mapped size=16777216                  ← just starting
...
[h0 t=12839.093s] primary attach done + init_done=1            ← init_done set HERE
[h0 t=12839.093s] set attached=1
```

h1 "saw init_done" and "all attached" within the same microsecond as
its region map, *44 ms before the primary (h0) even set init_done=1*.

## Why

In the old code, the fork loop ran *before* `cxl_region_init`. Each
process (parent + children) opened `/dev/dax0.0` and mmap'd
independently. Only the primary then `memset`'d the stats page and
published `init_done=1`; non-primary children were supposed to spin
on `init_done` until that happened.

This works *iff* `mmap(/dev/dax0.0, ...)` returns a zero-filled page.
On emr's direct-attach CXL it apparently did — prior runs never
triggered this bug there. On g3/g4's PCIe-switched CXL memory server
under kernel 6.15.0 it **does not**. The mmap returns whatever bits
the memory server left there, which includes leftover `init_done==1`,
`attached==1`, `ready==1`, `go==1` bytes from the previous bench run.

Non-primary children therefore:
- see a stale `init_done==1` → skip their wait and attach immediately,
  racing the primary's in-flight `memset` of the stats page;
- see a stale `attached[h]==1` for every h → skip the attached barrier;
- populate + set `ready=1` while the primary is still in the middle of
  zeroing the region, which the primary then *overwrites* back to 0;
- the next barrier (`ready == 1 for all h`) is therefore unsatisfiable
  because h1's freshly-written `ready=1` was clobbered.

Result: all four procs pin 99 % CPU spinning on flags that will never
converge. Looks exactly like an LFM livelock from the outside.

## The fix (committed)

`tests/cxl_kv_bench_mp.cc`: in fork-mode, open the region and zero the
stats page **in the parent before forking**. Children inherit the
parent's mmap via fork's address-space copy; they never see stale
residuals because the parent zeroed the page before the children existed.

Role-mode (each host is a separate invocation on a separate machine)
is unchanged — each invocation opens its own mmap, and the primary
still memsets on its side; non-primaries still spin on `init_done`.
Role-mode was never affected because separate machines don't race
through the same fork boundary.

## Verification

After the fix:

| opt | wr | N=4 agg (kops/s) |
|-----|----|-----|
| A   | 0.0 | 1450 |
| A   | 0.5 |  207 |
| A   | 1.0 |  118 |
| B   | 0.0 | 1176 |
| B   | 0.5 |  294 |
| B   | 1.0 |  141 |
| C   | 0.0 | 1197 |
| C   | 0.5 |  599 |
| C   | 1.0 |  403 |

Five consecutive runs at `opt=C wr=0.5 N=4`: all clean, wall 3 ms,
591-603 k ops/s agg. Also verified on g4.

## LFM backoff patch in `cxl_shm_profiling/locks/lfm_lock.c`

While investigating, I added an exponential backoff (`attempts > 8`
→ `nanosleep` 10 μs doubling up to 10 ms) plus a `FUSEE_LFM_DEBUG=1`
state dump. These stay in because:

- backoff is a cheap precaution: the fast path (attempts ≤ 8)
  is unchanged, so normal lock cost is unaffected.
- it gives us a safety net for real LFM contention once `threads_per_proc`
  pushes N above 4 in later scaling tests.
- the debug dump prints `[x, y, b[0..3]]` at every 1024 attempts when
  the env var is set — useful for any future retry-loop bug.

Neither is load-bearing for the current fix.

## Takeaway

Any code that assumes `mmap(/dev/dax*)` returns zero-filled memory is
broken on g3/g4's kernel 6.15.0 + PCIe-switched memory server. Review
the rest of the codebase for similar assumptions (e.g., the oplog
replay path probably handles stale bytes correctly because it validates
magic words, but worth double-checking).
