# G4 — ABANDONED (2026-06-08, per user)

Co-located 4CN+4MN (scheme①: MN CPU NUMA1 + interleaved mem, CN NUMA0) was launched
but abandoned mid-sweep. Valid data only for ≤8 clients (T≤2/node); T≥4 (≥16cl) FAILs.

**Root cause of the ≥16cl failures (diagnosed):** b2 is hugepage-starved — node0 free was
only 256 hugepages (512MB), node1 1536. Co-located on b2: MN (interleaved, ~2GB from node0)
+ CN (each client-thread needs client_local_size=1GB on NUMA0). At T≥4 the CN's threads
can't get their 1GB local caches from b2's depleted node0 hugepages → mmap fails → b2 CN
SIGSEGVs (b1 loads fine, b3/b4 wait, b2 crashes). So 28cl (7 threads/node × 1GB = 7GB on
node0) is infeasible without either (a) much smaller client_local_size, (b) rebalancing/adding
b2 hugepages, or (c) fewer threads/node. The single-CN probe (b1×7threads) worked because
only b1 ran a CN; the full 4-CN run exposes b2's hugepage shortage.

Partial data kept in results_g4.csv (micro + ypaper ≤8cl). FEASIBILITY.md has the setup.
Not part of the G1-G3 deliverable.
