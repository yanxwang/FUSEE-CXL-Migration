# Four-way sync workflow

Single-branch Git repo hosted at **github.com:yanxwang/FUSEE-CXL-Migration**,
mirrored across four working copies:

| Host   | Path                       | Role                          | GitHub auth |
|--------|----------------------------|-------------------------------|-------------|
| emr    | `~/FUSEE`                  | Primary dev + benchmark       | SSH (yanxwang) ✓ |
| local  | `/home/yanwang/FUSEE`      | Primary dev + editing         | SSH (yanxwang) ✓ |
| g3     | `~/FUSEE_CXL`              | Benchmark slave (PXE-wiped daily) | **N/A by design** — re-bootstrapped from local via `scripts/bootstrap_slave.sh g3` |
| g4     | `~/FUSEE_CXL`              | Benchmark slave (PXE-wiped daily) | **N/A by design** — same as g3 |

**`upstream`** (= `git@github.com:dmemsys/FUSEE.git`) is set on emr and local
for occasional rebase / diff against the original FAST'23 FUSEE code.
`origin` everywhere points to the private fork above.

## Day-to-day loop

Most work happens on **emr** or **local**, which are both write-authorized:

```bash
# on the machine you just edited
git add <files>
git commit -m "..."
git push origin feat/cxl-migration

# on the other write-authorized machine, pull the changes
git pull --ff-only origin feat/cxl-migration
```

g3 and g4 are **ephemeral** — homedirs live on local NVMe and PXE-reboot
daily, wiping `~/`, `~/.ssh/`, and any installed SSH key. They never
hold credentials for anything. Instead, **local is the orchestrator**:

```bash
# after a PXE reboot, or whenever code needs refreshing on a slave:
scripts/bootstrap_slave.sh g3    # or g4
```

This script runs on local, bundles the current HEAD of local's FUSEE,
rsyncs the bundle + cxl_shm_profiling to the slave, clones/updates
`~/FUSEE_CXL` there from the bundle, and rebuilds the CXL-only target.
Slaves never push; their output (benchmark logs) is rsynced back to
local by whoever launched the bench.

Why this instead of GitHub deploy keys / HTTPS-PAT / collaborator invite:
all three require credentials to persist on the slave, which the daily
PXE reboot prevents.

## Backups

- `/home/yanwang/fusee_backups/fusee_feat_cxl_*.bundle` — Git bundle,
  portable offline restore of the full history.
- `/home/yanwang/fusee_backups/local_working_tree_*.tar.gz` — snapshot of
  the local working tree at the moment we re-synced to GitHub (contains
  any stray files that were not yet committed).
- GitHub itself — authoritative remote, three-datacenter replicated.

Rebuild bundle after significant batches of commits:

```bash
ssh emr 'cd ~/FUSEE && git bundle create ~/fusee_feat_cxl_$(date +%Y%m%d_%H%M%S).bundle --all'
rsync emr:~/fusee_feat_cxl_*.bundle /home/yanwang/fusee_backups/
```

## Dependency: cxl_shm_profiling

Lives under `~/cxl_shm_profiling/` on each host. Remote is
`ssh://git@gitlab.seircloud.dc:30222/dupeiran/cxl_shm_profiling.git`
(internal GitLab, not GitHub). Any Option A side-track edits to
`bench/ycsb_abc_bench*.c` + `locks/lfm_lock.c` etc. live there, not in
the FUSEE repo. Decision on whether to mirror cxl_shm_profiling to a
second GitHub repo is deferred; for now, rsync the directory across
hosts when the LFM or bench code changes.

## Known non-sync risks

- **emr, g3, g4 are TWO testbeds, not one**:
  - **emr** has a private direct-attach 256 GB CXL Type-3 expander
    (`/dev/dax0.0`). Single-machine multi-process benchmarks over emr
    measure the CXL load/store path without the PCIe-switch hop.
  - **g3 + g4** share a 4-card ~512 GB CXL memory server behind a PCIe
    switch; each host sees partitions `/dev/dax0.0` (256 GB),
    `/dev/dax0.1` (128 GB), `/dev/dax0.2` (128 GB). Cross-machine KV
    benchmarks run here.
  Data from the two testbeds is **complementary, not directly
  comparable**. Keep separate output logs (`..._emr.log`, `..._g34.log`).
- **g3 dax0.1 still system-ram** as of 2026-04-21 02:10 CDT (devdax for
  `dax0.0` and `dax0.2`). Reconfigure dax0.1 only if we need the full
  fabric; dax0.0 alone is more than sufficient for current bench sizes.
- **Cross-host (g3↔g4) shared-region byte-equality unverified**. First
  cut of the multi-machine bench should be a magic-word mmap test:
  g3 writes a pattern to dax0.0 offset 0, g4 reads back, bytes match.
