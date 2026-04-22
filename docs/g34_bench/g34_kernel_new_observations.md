# g3 / g4 under new kernel 6.15.0 — running observations (2026-04-22 02:00 CDT+)

User rebooted g3 and g4 onto a different kernel (`uname -r` returns
6.15.0 now, was something else before) and changed root password to
`123456`. This doc tracks everything different about running FUSEE
benches on the new setup, so the next PXE cycle doesn't surprise us.

## What changed on the hosts

| | Before | After |
|-|--------|-------|
| Kernel   | unknown pre-6.15 | 6.15.0 |
| root pw  | `Haidilao666` | `123456` |
| dax0.0 size | 256 GiB | 512 GiB (!) |
| dax0.0 mode | had been flipping devdax / system-ram | devdax out of the box |
| SSH authorized_keys | wiped on PXE | still wiped on PXE (expected) |

## What still needs to happen after every PXE cycle

1. `scripts/rekey_slave.sh g3 && scripts/rekey_slave.sh g4` if local's
   pubkey isn't auto-re-installed (check `ssh g3 true`).
2. `chmod 666 /dev/dax0.0` on each host (root-only by default).
3. `scripts/bootstrap_slave.sh g3 && scripts/bootstrap_slave.sh g4` to
   reinstall FUSEE_CXL and rebuild.

If `daxctl list` shows system-ram mode, run
`daxctl reconfigure-device --mode=devdax --force dax0.0` on the host.

## Observed under new kernel that is NEW behaviour

### (+) Stability under bench load

The headline difference: **bench no longer takes g3 offline**. We ran
`cxl_kv_bench_mp_C` at N=4 fork-mode, the threads bench at 16 threads,
the latency decomp at N=4 contenders, the 12-run cross-host sweep, and
a 5-run N=4 repeatability test — g3 uptime went from fresh boot to
~4 hours without a single hard offline. Under the old kernel the same
series of tests took g3 down within minutes every time. So whatever
was driving the crashes (presumed CXL / PCIe driver path) is better in
6.15.0.

### (+) LFM wedge threshold lifted

Fork-mode `cxl_kv_bench_mp_C num_hosts=N` repeatability on g3:

| N | old kernel | 6.15.0 |
|---|------------|--------|
| 2 | ok 4 ms    | ok 4 ms |
| 3 | hang       | **ok 4 ms** |
| 4 | hang + crash | hang (software bug, not kernel) |

The N=3 now working without any code change is real progress. N=4
still hung — but that turned out to be a fork-mode init race in our
own bench (see g34_livelock_root_cause.md), not LFM, not kernel.
After fixing the init race, N=4 is clean too.

### (!) devdax mmap DOES NOT zero-fill on 6.15.0

The biggest correctness gotcha. Under 6.15.0 on g3/g4, newly mmap'd
`/dev/dax0.0` returns the exact bytes the previous test left there.
Every bench that uses CXL memory for cross-process or cross-host
flags MUST zero or version its own bookkeeping; relying on "fresh
mmap means fresh zero page" breaks.

We hit this in two places:

- `cxl_kv_bench_mp` fork-mode: non-primary children saw stale
  `init_done=1` from the previous run, skipped the wait, raced the
  primary's memset, and locked the whole bench in an unsatisfiable
  barrier. Fixed by moving the region open + memset to the parent
  *before* the fork.
- `cxl_ycsb_runner` role-mode: same bug between machines. Fixed by
  adding a unique `FUSEE_RUN_COOKIE` that the orchestrator rolls per
  run; non-primary waits for the cookie to match, not just for
  init_done.

Any other code that reads CXL flags before any primary has written
them this run should use the same pattern.

### (!) layout-skew footgun

Any compile-time constant that sizes CXL data (we hit `MAX_HOST_NUM`,
but the same applies to `kCxlKvSlotsPerBucket`, `kMaxHosts`, anything
in `bytes_for(...)`) MUST match on both hosts of a cross-host run.
If it doesn't, each host carves the region differently and they stop
talking.

Caught it this session when `CxlKvStore::bytes_for(65536)` returned
different values on g3 (MAX_HOST_NUM=8) vs g4 (32), putting the
shared-stats struct at different physical offsets — each host
cheerfully read and wrote its own flags and spun forever waiting for
the *other* host's flags that were landing somewhere else.

Proposed: add a one-time self-check at attach time — primary stamps
`sizeof(YcsbShared) + bytes_for(num_buckets)` at a known offset;
non-primary verifies a byte-for-byte match and refuses to proceed if
they diverge. Deferred as a nice-to-have.

### ( ) `daxctl reconfigure` unaffected

Switching between system-ram and devdax works the same as on the old
kernel. No need to reboot for the change to stick.

## Residual unknowns (none actively reproducing right now)

- Why the old kernel crashed g3 but not g4 under the same bench.
  Probably moot since 6.15.0 no longer crashes.
- Whether the N=4 LFM (not bench-race) livelock from the overnight
  analysis was ever real or was always the init-race masquerading as
  an LFM issue. The backoff patch in `cxl_shm_profiling/locks/lfm_lock.c`
  is kept as a safety net but is not load-bearing under 6.15.0.

## Log of things that have NOT been observed (under 6.15.0)

- No hard offline during any bench.
- No PAM "Not allowed at this time" since the reboot.
- No dmesg CXL / PCIe errors (checked via `dmesg -T | tail -200` after
  the bench loops).
- No sshd instability.

If any of these reappear, append a dated entry here.
