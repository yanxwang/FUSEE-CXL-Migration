# FUSEE-CXL migration — project rules for Claude

## North-star goal (read `docs/design_goals.md` first)

The CXL migration is **not complete** until both of these hold on
the g3 + g4 testbed:

- YCSB-C (100 % read)     sustains **≥ 20 Mops/s aggregate**
- YCSB-A (R50 U50 Zipf)   sustains **≥ 20 Mops/s aggregate**

A 3–5× improvement over the previous sweep is a checkpoint, not a
stopping condition. Every performance result must be compared to
the 20 Mops/s bar, and the remaining gap must be analyzed (latency
decomposition, identification of dominant stage) before declaring
a phase done. See `docs/design_goals.md` §"Analysis discipline".

## Canonical benchmark procedure

For any `scaling_ycsb` experiment, read `docs/scaling_ycsb_spec.md`
first. Scope, output layout, plot set, and reproducibility
requirements are spec-bound. Don't silently shrink scope when a
test times out — ask the user or raise timeout, then record the
choice in the summary doc.

## Phase plan reference

Overall throughput-improvement plan is in
`docs/ABC_throughput_improvement_plan.md`. Progress doc is
`docs/fusee_cxl_progress.md`.

## Code organization

- Three CXL protocols live side-by-side: `src/cxl_kv_ops_{A,B,C}.cc`,
  all implementing the `CxlKvStore` public surface. Selection is
  compile-time via `-DCONSENSUS_OPT=FUSEE_OPT_{A,B,C}`.
- Shared primitives: `src/cxl_hashtable.h`, `src/cxl_bucket_lock.h`
  (LFM wrapper), `src/cxl_pending_ring.h`, `src/cxl_same_host_queue.h`,
  `src/cxl_oplog.h`, `src/cxl_mm.{h,cc}`.
- LFM mutex primitives come from the sibling repo
  `~/cxl_shm_profiling/` (included via CMake).
- Tests: `tests/cxl_ycsb_runner.cc` (YCSB), `tests/cxl_kv_bench*.cc`
  (micro-bench), `crash-recover-test/` (recovery).

## Hosts

- `g3`, `g4`: dual 86-core Intel Xeon nodes sharing a CXL Type-3
  memory expander via PCIe switch. Device `/dev/dax0.0`, 512 GiB
  devdax. Kernel 6.15.0 after the PXE rebuild.
- `~/FUSEE_CXL/` on each host is the sync target; source lives on
  the workstation under `/home/yanwang/FUSEE/` and is rsync'd over.
- Rebuild after sync: `cd ~/FUSEE_CXL/build-cxl && make -j16 <targets>`.

## Commit hygiene

- Single branch: `feat/cxl-migration`.
- Commit prefixes: `[phase]`, `[2a]`, `[2b]`, etc. matching the
  plan item.
- Do not push to remote without explicit request.
