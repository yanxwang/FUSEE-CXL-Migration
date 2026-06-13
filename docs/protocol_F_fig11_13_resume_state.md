# Protocol F Fig 11 + Fig 13 — Resume State (paused 2026-06-07 ~11:50 CDT)

## Why paused
g1 + g2 entering another user's shift; pausing to resume tonight.

## Current state at pause

### Code
- ✅ `tests/cxl_kv_ops_F_micro_throughput.cc` written + builds clean on g1 + g2.
- ✅ `tests/cxl_kv_ops_F_ycsb.cc` written + builds clean on g1 + g2.
- ✅ `tests/CMakeLists.txt` updated to include both binaries.
- ✅ `scripts/run_protocol_F_fig11_sweep.sh` written.
- ✅ `scripts/run_protocol_F_fig13_sweep.sh` written.

### Documentation
- ✅ [docs/protocol_F_vs_FUSEE_alignment.md](protocol_F_vs_FUSEE_alignment.md) §1 (4-op pseudocode), §3 (Fig 10/11/13 experiment alignment), §7.1 (Fig 10 baseline) all in place.
- ⏳ §7.2 (Fig 11 data) — placeholder; needs data.
- ⏳ §7.3 (Fig 13 data) — placeholder; needs data.

### Workload files (FUSEE-standard YCSB)
- ✅ `setup/workloads/workload{a,b,c,d}.spec_{load,trans}` rsync'd to `g1:/tmp/workloads/` and `g2:/tmp/workloads/`. (Lives in tmpfs — fits, g1 root was 98% full.)

### Builds on testbed
- `g1:/root/FUSEE_CXL/build-cxl/tests/cxl_kv_ops_F_micro_throughput` — built
- `g1:/root/FUSEE_CXL/build-cxl/tests/cxl_kv_ops_F_ycsb` — built
- Same on g2.

## Blocking issue
Fig 11 binary hangs at cross-host barrier 0.

### Symptoms (last run with diag prints)
```
g1:
  host 0/2 clients=1 cookie=... store_bytes=17712391104 map=17712545792
  host 0 attach OK
  [bar0] host 0 publishing cookie=...152
  [bar0] host 0 waiting on peer 1
  exit_g1=124   ← timeout

g2:
  host 1/2 clients=1 cookie=... store_bytes=17712391104 map=17712545792
  host 1 attach rc=-1     ← attach FAILED
  exit_g2=1
```

g2's attach returned -1; the cross-host barrier never had a peer to wait for.

### Most likely root cause
g1 init_region path memset's a **17.7 GB** lock table (LFM per-slot at
38.6 KB × 65 536 buckets × 7 slots).  Over CXL with clflushopt+sfence,
that memset takes much longer than the 5-second sleep between g1 launch
and g2 launch in the sweep script.  g2 attaches before g1's init is
visible → magic check fails → rc=-1.

The simpler `cxl_kv_ops_F_2host_hashdiff` (num_buckets=4096, store=1.1 GB)
**does** work — same barrier code path, just a much smaller init time.

### Resume plan (in priority order)

**1. Reduce Fig 11 store size to <2 GB** so init finishes inside the 5s
sleep.  Two options:

   (a) Smaller `kNumBuckets` (e.g. 16384 → LFM ≈ 4.3 GB).  Still too
       slow for 5s sleep on CXL.

   (b) Switch to a fixed `kKeysPerClient` that bounds total keys low
       enough to fit num_buckets=4096 (the hashdiff config).  At
       num_buckets=4096 → 28 672 slot capacity.  For max-C=64 × 2 = 128
       threads at LF ≤ 0.5, total keys ≤ 14 336.  → kKeysPerClient ≤ 50
       and per-thread INSERT cap ≤ 50.  INSERT cap of 50 in 500 ms means
       only the slowest threads will be timer-bound; fast threads
       finish their cap in <50 ms which under-reports throughput.

   (c) **Best: extend host-0 init wait → host-1 doesn't poll-fail.**
       Modify the sweep script to:
       - launch g1
       - wait until g1's log contains "attach OK" (means init done +
         init_done_bit set)
       - then launch g2

       This keeps num_buckets=65536 and the LFM table size, but makes
       the launch race-free.

   Pick (c).  It's a script change only.

**2. Defensive — increase attach's init_done_bit wait timeout** if the
attach is silently giving up after some retry budget.  Read [src/cxl_kv_ops_F.cc:140-150](../src/cxl_kv_ops_F.cc#L140) to confirm — it's an infinite spin (no timeout), so this isn't the actual cause; rc=-1 must come from a later check (magic mismatch?).  Worth a 5-min audit.

**3. Re-run Fig 11 smoke cell c=1** with the fixed launch ordering.
   - Expected output: `SUMMARY F_micro_tpt ...` from both hosts.

**4. If c=1 passes, run full Fig 11 sweep** (`run_protocol_F_fig11_sweep.sh`).

**5. Run full Fig 13 sweep** (`run_protocol_F_fig13_sweep.sh`).

**6. Parse + plot results, fill in §7.2 and §7.3 of alignment doc.**

## Files to touch on resume
- `scripts/run_protocol_F_fig11_sweep.sh` — replace `sleep 5` with
  `while ! ssh root@g1 "grep -q 'attach OK' ${G1_LOG}"; do sleep 1; done`
  pattern; mirror in Fig 13 sweep.
- `tests/cxl_kv_ops_F_micro_throughput.cc` — remove debug prints in
  `barrier_arrive_and_wait` once it's confirmed working.
- `tests/cxl_kv_ops_F_ycsb.cc` — copy the same sync fix into its launch
  script.

## Quick verify on resume
```bash
ssh root@g1 'ls /dev/dax0.0 && daxctl list -d dax0.0' 2>&1
ssh root@g2 'ls /dev/dax0.0 && daxctl list -d dax0.0' 2>&1
# hashdiff sanity (proves barrier mechanism still works)
RUN_ID=$(date +%s)
ssh root@g1 'daxctl reconfigure-device --mode=devdax --force dax0.0 >/dev/null 2>&1; chmod 666 /dev/dax0.0'
ssh root@g2 'daxctl reconfigure-device --mode=devdax --force dax0.0 >/dev/null 2>&1; chmod 666 /dev/dax0.0'
ssh root@g1 "timeout 30 /root/FUSEE_CXL/build-cxl/tests/cxl_kv_ops_F_2host_hashdiff /dev/dax0.0 0 2 $RUN_ID" &
sleep 3
ssh root@g2 "timeout 30 /root/FUSEE_CXL/build-cxl/tests/cxl_kv_ops_F_2host_hashdiff /dev/dax0.0 1 2 $RUN_ID" &
wait
# Then start with the launch-ordering fix in run_protocol_F_fig11_sweep.sh.
```
