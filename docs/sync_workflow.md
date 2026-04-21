# Four-way sync workflow

Single-branch Git repo hosted at **github.com:yanxwang/FUSEE-CXL-Migration**,
mirrored across four working copies:

| Host   | Path                       | Role                          | GitHub auth |
|--------|----------------------------|-------------------------------|-------------|
| emr    | `~/FUSEE`                  | Primary dev + benchmark       | SSH (yanxwang) ✓ |
| local  | `/home/yanwang/FUSEE`      | Primary dev + editing         | SSH (yanxwang) ✓ |
| g3     | `~/FUSEE_CXL`              | Benchmark slave               | **TBD** — account is `heatheart3`, read-only to this private repo until collaborator invite or deploy key |
| g4     | `~/FUSEE_CXL`              | Benchmark slave               | **TBD** — same as g3 |

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

g3 and g4 are currently **read-only** copies. Until auth is solved:

```bash
# on emr after a relevant commit
git bundle create /tmp/fusee.bundle feat/cxl-migration --all
rsync /tmp/fusee.bundle g3:/tmp/ g4:/tmp/

# on g3 or g4
cd ~/FUSEE_CXL
git pull /tmp/fusee.bundle feat/cxl-migration
```

Once g3/g4 have GitHub auth, the bundle step disappears and they run the
same `git pull` as emr/local.

## Getting g3 and g4 GitHub-authed

Pick one:

1. **Invite `heatheart3` as collaborator** on the private repo (settings →
   collaborators). After accepting, g3 can push/pull over SSH with its
   existing key. g4 needs its own key added to whichever account you use
   there.
2. **Deploy key per host**: generate `ssh-keygen` on g3 (and g4), add the
   public key under *repo settings → deploy keys* on GitHub. This grants
   the specific machine (not the user) access. Safer for shared machines.
3. **HTTPS + PAT**: clone via `https://github.com/...`, use a fine-grained
   personal access token in `~/.git-credentials`. Works but rotate the
   token regularly.

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

- **g3 dax0.1 is still system-ram** (2026-04-21 02:08 CDT). Reconfigure
  to devdax before running any CXL benchmark on g3. Sudo required.
- **Cross-host shared CXL region unconfirmed**. g3/g4/emr each expose
  `/dev/dax0.*`, but whether those devices map the *same physical bytes*
  via the CXL switch fabric has not been verified yet. First step of the
  multi-machine benchmark push should be a cross-host magic-word mmap
  test (one host writes, the others read back the same offset).
