# iter-15A Layer A — Path counter verification + trace gen bug fix

**Date**: 2026-05-20
**Status**: ✅ Layer A complete; ready to resume for higher-scale microbench

## Backstory

iter-14A P6 microbench had 4 scenarios (`local_read`, `xhost_read`, `local_write`, `xhost_write`) intended to give clean separation of code paths. Throughout iter-14A we observed "local ≈ xhost" — within 1-2% throughput — and attributed it to cache_pool warmup behavior (R3 first-touch → R2hit amortization).

Layer A introduces 10 per-thread path counters in `cxl_kv_ops_A.cc` to directly verify which code path each op took. Result: trace gen had a **hash mismatch bug** that contaminated all 144 P6 cells.

## The bug

| Component | Hash computation |
|---|---|
| **Python trace gen** (`iter14A_gen_microbench_traces.py`) | `owner_host(int_key)` = `(fnv1a_u64(int_key) >> 63) & 1` |
| **C++ runtime** (`tests/protocol_a_ycsb.cc` + `cxl_sharding.h`) | `host_of(hash_str("user" + int_key))` = two-stage: `hash_str` (FNV on string bytes) then `sharding_hash_u64` (FNV on u64) |

Python and C++ produce **different owner partitions** for the same key.

Result: trace files labeled "local_read" actually contained ~50/50 mix of local + peer keys (from runtime's POV). Same for all 4 scenarios.

## Evidence (Round 1 — broken traces)

| Scenario | r3 fires (should be) | r3 fires (measured) |
|---|---|---:|
| local_read uniform | ≈ 0 | **224,203** (45% of reads) |
| xhost_read uniform | ≈ all reads | **224,278** (45%) — same as local_read! |
| local_write fwd_w | ≈ 0 | 249,164 (25% of writes) |
| xhost_write fwd_w | ≈ all writes | 249,532 — same as local_write |

→ Iter-14A's "local ≈ xhost" conclusion is **invalid** — the 4 scenarios ran essentially the same mixed workload.

## Fix

Updated `iter14A_gen_microbench_traces.py:32-65` to use exact runtime hash chain:
```python
def runtime_key_for_int(int_k):
    return hash_str_runtime(f"user{int_k}")        # match C++ hash_str

def owner_host(int_k, num_hosts=2):
    K = runtime_key_for_int(int_k)
    return (fnv1a_u64(K) >> 63) & 1                # match C++ host_of
```

Regenerated 32 trace files in `/home/yanwang/FUSEE/setup/iter14A_microbench_traces/` (old files preserved at `.../iter14A_microbench_traces.OLD/`).

## Evidence (Round 2 — fixed traces, T=1)

Output dir: [iter15A_layerA_pathcount_20260520_002133/](../iter15A_layerA_pathcount_20260520_002133/)

| Scenario | thpt (Mops/s) | r3 | r2hit + r2miss_local + r0_tls | fwd_w | local_w_blk_fwd |
|---|---:|---:|---:|---:|---:|
| local_read uniform | 1.94 | **0** ✓ | 500,000 | 0 ✓ | 0 ✓ |
| local_read zipf | 2.23 | **0** ✓ | 500,000 | 0 | 0 |
| xhost_read uniform | 0.21 | **449,180** ✓ | 50,820 (after R3 warmup) | 0 | 0 |
| xhost_read zipf | 0.33 | **268,284** ✓ | 231,716 (TLS+R2 hits) | 0 | 0 |
| local_write uniform | 0.67 | 0 | 0 | **0** ✓ | 0 |
| local_write zipf | 0.68 | 0 | 0 | **0** ✓ | 0 |
| xhost_write uniform | 0.20 | 0 | 0 | **500,000** ✓ | 500,000 ✓ |
| xhost_write zipf | 0.19 | 0 | 0 | **500,000** ✓ | 500,000 ✓ |

All path invariants satisfied. Cross-host write invariant: `host_i.fwd_w == host_(1-i).local_w_blk_fwd` ✓ on all 4 write cells.

## Counter implementation

10 thread-local counters in [`src/cxl_kv_ops_A.cc`](../../src/cxl_kv_ops_A.cc):
- Read: `n_tls_hit`, `n_r2hit`, `n_r2miss_local`, `n_r3`, `n_cache_pool_insert_from_read`
- Write (worker): `n_local_write_worker`, `n_forward_write`
- Write (receiver): `n_local_write_with_blk_forwarded`, `n_local_write_staging_forwarded`, `n_cache_pool_insert_from_write`

Build flag: `-DFUSEE_PATH_COUNTERS=1`. Default OFF (no-op).
Dump format: `# PATH host=N tid=M ...` printed at end-of-test by `tests/protocol_a_ycsb.cc`.
Build dir on hosts: `/tmp/builds/build-cxl-w1-pathcount` (symlinked to `/root/FUSEE_CXL/build-cxl-w1-pathcount`).

## True local vs xhost performance (T=1)

| Cell | local thpt | xhost thpt | local/xhost ratio |
|---|---:|---:|---:|
| read uniform | 1.94 M | 0.21 M | **9.2×** |
| read zipf | 2.23 M | 0.33 M | **6.8×** |
| write uniform | 0.67 M | 0.20 M | **3.3×** |
| write zipf | 0.68 M | 0.19 M | **3.5×** |

The actual local vs xhost gap is large (3-9×). iter-14A's "local ≈ xhost" was an artifact of the trace bug, not a property of the design.

## Implications for iter-14A documents

These iter-14A docs need erratum / correction:
- `docs/iter14A_p6_microbench_20260519_030533/MICROBENCH_ANALYSIS.md`
- `docs/iter14A_p6_microbench_20260519_030533/P6_FINDING_local_vs_xhost.md`
- `docs/iter14A_p65_xhost_slowpath_20260519_054536/ANALYSIS.md`
- `docs/iters/iter14A_summary_20260519.md` (P6 section)

The story "cache_pool warmup absorbs local/xhost difference" is **partially wrong** — it was a downstream artifact of trace contamination, not architectural behavior.

## Things still TBD (resume points)

When resuming this microbench thread:

1. **Sweep at higher T** (T=4, 32, 64) on the fixed traces — verify whether high-T xhost still ≈ local (if so, then maybe cache_pool warmup IS real at high T; if not, the bug was the whole story).

2. **Higher-rep sample** — currently 1 rep. 5-rep with bootstrap CI on the fixed traces to confirm signal.

3. **Layer B** — per-op path tags (CSV of which path each op took) to validate first-N vs last-N R3 trend.

4. **Layer C** — probe-build full stage decomp (was BLOCKED in iter-14A by segfault; needs separate investigation).

5. **iter-14A P6 / P6.5 reanalysis** — re-run with new traces; correct the "local ≈ xhost" claim or refine it.

6. **Wider scenario menu** — currently 4 scenarios × 2 keydists × 1 T × 1 rep. Future sweeps want T ∈ {1,4,16,32,64}, key concentration sweep (cap = 1K, 10K, 100K, 1M, 2M), value size sweep.

## Files / artifacts

| Artifact | Path |
|---|---|
| Trace gen (fixed) | `scripts/iter14A_gen_microbench_traces.py` |
| Old broken traces | `setup/iter14A_microbench_traces.OLD/` |
| New fixed traces | `setup/iter14A_microbench_traces/` |
| Counter source | `src/cxl_kv_ops_A.cc` (under `#if FUSEE_PATH_COUNTERS`) |
| Round 1 raw data (broken) | `docs/iter15A_layerA_pathcount_20260520_000553/` |
| Round 2 raw data (fixed) | `docs/iter15A_layerA_pathcount_20260520_002133/` |
| This summary | `docs/iter15A_layerA_pathcount_summary/README.md` |

## Build commands to reproduce

```bash
# On host (g3/g4):
mkdir -p /tmp/builds/build-cxl-w1-pathcount
cd /tmp/builds/build-cxl-w1-pathcount
cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS="-DFUSEE_READ_GUARD=2 -DFUSEE_WRITE_ALLOC=1 -DFUSEE_PATH_COUNTERS=1" \
      /root/FUSEE_CXL
make -j16 protocol_a_ycsb
ln -sf /tmp/builds/build-cxl-w1-pathcount /root/FUSEE_CXL/build-cxl-w1-pathcount
```
