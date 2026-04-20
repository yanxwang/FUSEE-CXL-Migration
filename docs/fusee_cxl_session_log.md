# FUSEE → CXL Migration Session Log

> Chronological log of each work session. Append-only.

---

## Session 2026-04-20 ~01:40 CDT — Planning / setup

**Context**: user re-scoped the work. We actually need to modify FUSEE src/. Dev host = emr.

**Done**:
- SSH'd to emr; confirmed Xeon Gold 6530 (clflushopt supported), 128 GB DRAM + 256 GB CXL
- Identified `/dev/dax0.0` currently in system-ram mode (blocker for CXL testing)
- Synced `/home/yanwang/cxl_shm_profiling/` → `emr:~/cxl_shm_profiling/`
- Built `bench/measure_latency` on emr, validated DRAM latencies (332 ns STORE, 535 ns LOAD)
- Could not test CXL latency (dax0.0 not devdax yet; sudo reconfigure blocked)
- FUSEE on emr is at `~/FUSEE`, main branch, 2 commits (initial commit + extended FAST paper)
- Untracked user files: localhost_configs/, run_simple_test.sh, etc. — not touching these
- Created `docs/fusee_cxl_progress.md` (this session started it)
- Set up session log file (this one)

**Not done / deferred**:
- CXL devdax mode switch (user sudo needed)
- Actual FUSEE modifications (haven't started Phase 0)
- Option A deep-dive analysis (will do before Phase 0 per user request)

**Ends with**: investigation plan for Option A perf bottleneck (in progress)

---
