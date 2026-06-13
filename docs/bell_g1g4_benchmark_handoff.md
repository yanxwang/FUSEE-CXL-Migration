# Bell G1-G4 Benchmark — Handoff / Resume Doc

(Written 2026-06-07. Read this + CLAUDE.md + memory to resume.)

## Goal
Benchmark pristine RDMA FUSEE on bell (b1-b4) within hardware limits. Verify
runnable + clean baselines. NOT matching paper's 128-client scale.

## 4 groups (data_rep = idx_rep = r)
- G1: 2CN+2MN, data=2, idx=1 (SNAPSHOT off, paper headline)
- G2: 2CN+2MN, data=2, idx=2 (Rule 1)
- G3: 1CN+3MN, data=3, idx=3 (Rule 1+3, no Rule 2)
- G4: 4CN+4MN co-loc, data=4, idx=4 (Rule 1+2+3)
data_rep>=2 avoids num_replication=1 DELETE-hang bug. MN >= max(data,idx) (code not enforced).

## Per group: 2 sweeps
1. YCSB a/b/c/d thpt+lat. 2. 4-types micro (search/insert/update/delete) thpt+lat.
KV value=1024B, subblock=512, rep=3. "N clients" = N threads (each = 1 client w/ 8 coroutines, per paper).

## Environment (set up on bell)
- Access: memory reference_bell_cluster.md. ssh b1-b4 (user wang). sudo password = 123456 (memory wrongly says no-sudo; CORRECT: sudo works).
- b2 ptrace_scope=0; b4 hugepages set.
- bell source = pristine RDMA FUSEE at b*:~/FUSEE (git d1e9932). Do NOT rsync local CXL fork over it.
- 1024B value patch applied + built on ALL 4 hosts (client.cc pads value to 1024B, value_buf[1100], fixed [256] stack overflow). md5 cbf6632dad6963e5f00ad985bb830582.
- server_data_len=8GB (8589934592) in all configs (MN num_blocks 58->122). REQUIRED for 1024B at high N else "Cannot have allocated" (client_mm.cc:194) crash.
- Configs: MN b1/b3:~/fusee_dbg/server_config.json (sid via argv); CN b2/b4:~/FUSEE/build/ycsb-test/client_config.json.
- Start MN (ssh-safe detach): ssh bX "cd ~/fusee_dbg && (setsid stdbuf -oL ~/FUSEE/build/ycsb-test/ycsb_test_server <sid> >server.log 2>&1 </dev/null &)". Run CN from ~/FUSEE/build/ycsb-test (needs workloads/). Never pkill -f ycsb_test_server in the same shell that contains that string (self-kill).

## CORRECT G1=2CN+2MN mechanism (KEY, not yet run)
Single-CN load + multi-CN trans summed, via loader/worker binaries:
1. Start 2 MN (b1 sid0, b3 sid1).
2. ycsb_wl_loader <config> <workload> on b2 -> load table once (no race).
3. Launch N worker procs concurrently: ycsb_wl_worker <client_id> <config> <workload>, each=1 client, N/2 per CN.
   For N=28: b2 client_id 3-16, b4 client_id 17-30.
   - client_id MUST be globally unique (=server_id=CID for MN memory ownership).
   - Core map (conf_reassign_cores, ycsb_test.cc:857): core_off=client_id-num_replication-1; main=cfg.main_core_id+2*core_off; poll=cfg.poll_core_id+2*core_off.
     b2 cfg main=0/poll=1 -> client_id 3-16 -> cores 0-27.
     b4 cfg main_core_id=-28/poll_core_id=-27 -> client_id 17-30 -> b4 local cores 0-27 (unique CID + local cores).
   - Worker runs load_test_cnt_time (ycsb_test.cc:785): loads TRANS op file (same for all = shared Zipf keyspace = real contention); table already loaded -> searches hit -> failed~0.
4. Sum each worker ops/s = aggregate N-client thpt. Sweep N={2,4,8,16,28}.
   OPEN: confirm worker tpt output. load_test_cnt_time prints "Test phase ends (Failed N)" + "time spent: X ms", maybe no clean "tpt:". May need compute ops/s from op count (client_ops_fb_cnt_time) or add a tpt print + rebuild.

## G1 STATUS: COMPLETE (2-CN, both methods) 2026-06-07
Full results + tables + cited root causes: `docs/bell_g1_benchmark_20260607/SUMMARY.md`
(147 cells, micro throughput+latency + YCSB paper + YCSB loader/worker). Headlines:
- Read ceiling ≈ 5.0–5.5 Mops/s aggregate (2× ConnectX-4): micro-search 28cl=5.48M,
  YCSB-C 16cl=5.50M agree. Write/update bottleneck: update 28cl 1.94M.
- Two methods agree ≤8cl; lw ≥ paper at high N (no getchar/barrier overhead).
- 2 cited-root-cause anomalies (both = ClientMM block-exhaustion, gdb-located):
  workloada-paper-28cl SIGSEGV (mm_alloc empty deque); delete-latency HANG
  (nm_poll_completion_sync spins). See SUMMARY §"Cited root causes".

### Validated 2-CN mechanics (supersedes the "OPEN" notes below)
- MN MUST launch in parallel (lone server aborts in inter-server init). `restart_mn.sh`.
- ycsb_test_server `getchar()` is commented out (`sleep(1e8)`), so `</dev/null` is fine.
- Two methods: paper (`ycsb_test_multi_client`+`split-workload`, getchar-sync, T/node →
  2T clients, 10s `tpt:` line) vs loader/worker (load once + N workers same trans, fixed
  ~100k ops, ops/s=ops*1000/ms). lw N ↔ paper T=N/2. Orchestrator: `scripts/orch.py`.
- loader/worker `should_stop` SIGSEGV FIXED + `ycsb_wl_worker` rebuilt on b2/b4.
- cache.dump scp'd b2→b4; b4 worker config main=-28/poll=-27 (cid 17.. → local cores).
- See memory [[project_bell_g1_benchmark_mechanics]].

### Old single-CN reference numbers (pre-2-CN, kept for cross-check)
1 CN b2, 2 MN, data=2 idx=1, 1024B, Mops/s 3-rep avg (per-client ~0.17, matches 2-CN):
- A: N1 0.175, N2 0.338, N4 0.668, N8 1.260, N14 2.03
- C: 0.358, 0.712, 1.419, 2.610, 2.74 (saturates ~N8 = NIC bw)

## Phase 2 (4-types micro), after YCSB
- Latency: latency_test_client <config> (1 client, 4 ops x100k, prints "lat test X / Failed: N", writes results/). = Fig 10.
- Throughput: micro_test_multi_client <config> <op?> <num_clients> (check main args). = Fig 11.
- Both rebuilt (1024B+overflow fix). G1 rep=2 -> DELETE not hang.

## Key findings
- "num_replication=1 silent insert-drop" does NOT reproduce on bell single-client; real rep=1 bug = DELETE hangs (rep=2 clean). All groups data_rep>=2 so moot.
- bell harness single-CN by design; multi-CN needs loader/worker model above.
- Old bell 1.89/4.04 Mops used ~18-32B values; NOT comparable to paper 1024B.

## Driver
/tmp/g1_sweep.sh = SINGLE-CN version (rewrite for loader/worker 2-CN). /tmp volatile.

## G2/G3 DONE + G4 FEASIBLE (2026-06-07 autonomous run)
Each group dir has RESULTS.md (standard 3-table format via scripts/gen_results_doc.py):
- **G2** (2CN+2MN data=2 idx=2): `docs/bell_g2_benchmark_20260607/` — ypaper 60/60 clean
  (headline); idx=2 halves writes (paper Fig 18); **lw broke for update workloads** (lockstep
  trace + replicated-index CAS storm). a-28cl=2.20 (no crash, unlike G1 idx=1).
- **G3** (1CN+3MN data=3 idx=3): `docs/bell_g3_benchmark_20260607/` — ypaper 16/16 clean;
  single-CN read ceiling ~2.7M (1 NIC); **lw non-functional at idx=3** (38/48 FAIL).
  Suppressed nm.cc:819 flush-err printf (memory_num≥3 floods; not a bug, downstream of
  FAIL_REDO; doesn't skew tpt).
- **G4** (4CN+4MN co-located): `docs/bell_g4_benchmark_20260607/FEASIBILITY.md` — FEASIBLE.
  Blocker: per-NUMA-node hugepages=3584 (7GB) < MN 8GB → can't put MN mem on node1 only.
  Workaround validated: MN `numactl --cpunodebind=1 --interleave=0,1` (CPU node1, mem both),
  CN `--cpunodebind=0 --membind=0`. Probe: 1 CN×7thr vs 4 co-located MN = 2.17 Mops c, 0 fail.
  Full G4 needs: 4-node orchestrator + user decision on mem config (interleave vs reduce
  data_len vs rebalance hugepages). DEFERRED to user discussion.
Key cross-group: ypaper is the method of record (lw degrades with idx≥2, dead at idx=3).
Replication cost: write tput/latency degrade idx 1→2→3 (expected). Reads NIC-bound
(~5.5M @ 2CN, ~2.7M @ 1CN). Bugs fixed: should_stop, delete reclaim, flush-printf suppressed.
