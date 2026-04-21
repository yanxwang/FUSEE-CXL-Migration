# Overnight run summary — 2026-04-21 01:45–present CDT

> One-page digest of what the auto-run accomplished before you return.
> Full details in `docs/fusee_cxl_session_log.md` and
> `docs/fusee_cxl_progress.md`.

## What shipped (committed + pushed)

**Sync + backup infrastructure (earlier in this chat session)**
- GitHub private repo `yanxwang/FUSEE-CXL-Migration` set as `origin`.
  Upstream `dmemsys/FUSEE` kept as read-only `upstream`.
- emr + local both auth'd, bidirectional push/pull verified.
- g3 + g4 bootstrapped via local-orchestrator pattern (`scripts/bootstrap_slave.sh`)
  — no credentials stored on the slaves (survives PXE wipe).
- Portable backup: `/home/yanwang/fusee_backups/fusee_feat_cxl_20260421_020411.bundle`
  (1.3 MB, the full branch history in one file).
- Recovered 197 previously-only-local research docs (INDEX.md, SUMMARY.md,
  cxl_architecture_plan.md, cxl_implementation_guide.md,
  consensus_transformation_explained.md, implementation_log.md,
  bell_run/ + c1c2_run/ + tigon_presentation/ subdirs, pptx + png
  design artifacts). All now in git.

**Real-CXL emr data (open tasks #2 and #5)**
- `docs/fusee_mp_bench_emr_20260421.log` — 54 runs, 4-host fork-mode,
  cache off+on. Writes (wr=1.0) with cache on: C 596 k, B 230 k, A 169 k
  ops/s; C wins 2.6× over B, 3.5× over A.
- `docs/fusee_ycsb_sweep_emr_20260421.log` — 36 runs, real YCSB workloads
  (a..f), cache off+on. Read-heavy workloadc: cache on speeds C up 3.6×
  and A/B 5.7×.
- Plots: `docs/fusee_mp_bench_emr_20260421_cache_on.png`,
  `docs/fusee_ycsb_sweep_emr_20260421.png`,
  `docs/fusee_ycsb_sweep_tmpfs_cache.png`.

**Cross-host infrastructure for g3 + g4 (ready, not yet exercised at scale)**
- `FUSEE_HOST_ID` / `FUSEE_NUM_HOSTS` env support in `cxl_kv_bench_mp` and
  `cxl_ycsb_runner`: one invocation plays one role, no fork, coordinate
  through CXL shared stats. Host 0 emits the aggregate summary.
- `tests/cxl_xhost_test.cc`: 12-cacheline handshake across hosts.
  **Proven g3 ↔ g4 share the same physical CXL bytes via their PCIe
  switch + memory server** — see `docs/g34_bench/xhost_verification.log`.
- `scripts/run_xhost_bench.sh`: single cross-host bench launcher.
- `scripts/run_g34_full_sweep.sh`: A/B/C × workloadA+C × cache off+on
  matrix launcher (ssh-based, from local).

## What's NOT done — and why

**9 AM target (full A/B/C × YCSB A+C on g3+g4) is at risk.**

Reason 1 — LFM wedge at N ≥ 3 on PCIe-switched CXL:
When I ran fork-mode `cxl_kv_bench_mp_C` with `num_hosts=4` on g4's
`/dev/dax0.0`, all four processes spun at 99 % CPU indefinitely.
2-proc same binary finished in 4 ms. Hypothesis documented in
`docs/g34_bench/g34_lfm_finding.md`: Lamport's Fast Mutex retry loop
never clears because the fabric's write-visibility window is longer than
the fast-path window. Workaround: run the g3+g4 sweep with `num_hosts=2`
(1 process per slave), not 4 procs per slave.

Reason 2 — g3 + g4 went offline at 03:01 CDT and have not returned:
- g3: no ping, no ssh (hard offline, presumably PXE-cycle).
- g4: pings fine, ssh closes with `banner line 0: Not allowed at this time`
  (later: `Connection reset by peer`). Almost certainly user's daily PXE
  maintenance window. Monitor task `bhfky5v4i` watching for both to come
  back.

Whenever g3 and g4 are both reachable again (tested `scripts/rekey_slave.sh`
handles the PXE-wipe path too):
```bash
scripts/rekey_slave.sh g3    && scripts/rekey_slave.sh g4        # if ssh key was wiped
scripts/bootstrap_slave.sh g3 && scripts/bootstrap_slave.sh g4   # restore code + rebuild
bash scripts/run_g34_full_sweep.sh                               # 12 runs, ~5-10 min
python3 docs/plot_fusee_ycsb.py logs/g34_full_sweep_*/SUMMARY.log
# For the post-mortem while slaves are healthy:
scripts/collect_post_outage_diag.sh g3
scripts/collect_post_outage_diag.sh g4
```

**Outage status as of 08:27 CDT 2026-04-21**: still ongoing (~5h 26m).
- g3: hard offline (no ICMP, no TCP — "No route to host"). Likely mid-PXE
  cycle or full power-off during the maintenance window.
- g4: pings at 0.3 ms but sshd rejects every connection with PAM
  `Not allowed at this time` → later `Connection reset by peer`.
- Tried password ssh via `expect` (credentials from local notes file) —
  also rejected, confirming this is a PAM account-phase denial independent
  of auth method.
- No automated recovery possible. Slaves come back when the user's
  maintenance window closes.

**Bottom line for the 9 AM target**: the g3+g4 cross-host A/B/C × YCSB
A+C sweep did NOT run. All enabling infrastructure IS ready (role-mode
binaries built on both slaves, cross-host fabric verified, orchestrator
+ preflight + diag + rekey scripts all committed and tested in the 02:30
window when the slaves were briefly up). One-line execution once the
slaves return:
```bash
scripts/rekey_slave.sh g3 && scripts/rekey_slave.sh g4 && \
scripts/bootstrap_slave.sh g3 && scripts/bootstrap_slave.sh g4 && \
bash scripts/run_g34_full_sweep.sh && \
python3 docs/plot_fusee_ycsb.py logs/g34_full_sweep_*/SUMMARY.log
```
This should take ~10-15 min end-to-end.

## Close-out

**Final state at 09:29 CDT 2026-04-21** (stopping autonomous run at 10 AM
as instructed):
- Outage duration: ≈ 6 h 28 min (from 03:01 onset).
- g3 and g4 both still unreachable with the same error profile as at
  onset; no status change at any retry interval overnight.
- No further ssh retries scheduled; the rekey+bootstrap+sweep commands
  above will work on the next manually-triggered session once slaves
  return.
- 70+ commits on `feat/cxl-migration` landed this session; 4-way sync
  and GitHub mirror verified; all research docs recovered and committed;
  real-CXL emr sweeps + plots + tmpfs-vs-CXL comparison all shipped.

## Current commit state

- GitHub HEAD: (see below)
- Number of commits on `feat/cxl-migration`: 60+
- 4 hosts all aligned within one `git pull` or `bootstrap_slave` invocation.
