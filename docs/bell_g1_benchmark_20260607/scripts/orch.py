#!/usr/bin/env python3
"""
Paper-method 2-CN orchestrator for pristine RDMA FUSEE on bell.

Runs *_multi_client on node0(b2)+node1(b4) simultaneously via `ssh -tt`,
releases the per-node getchar() sync points by writing a newline to each
node's stdin when BOTH nodes have reached that sync point, then parses and
sums the per-node `tpt:` lines.

Mechanism reference (verified in source):
- ycsb_test_multi_client: only node0(server_id==memory_num) thread0 loads the
  full table, then getchar() (marker: last "time spent:"); node1 thread0 prints
  "press to start!" then getchar(). 1 sync point. Trans = 10s time-bounded.
  Each node prints "tpt: N ops/s" (= node-local total/10). Aggregate = sum.
- micro_test_multi_client: each node thread0 prints "press to sync start <OP>"
  + getchar() before each of 4 phases (INSERT, READ, UPDATE, DELETE).
  4 sync points. Each node prints "<op> tpt: N ops/s". Aggregate = sum per op.

Usage:
  orch.py ycsb  <workload> <threads_per_node> <outdir_prefix>
  orch.py micro <_unused_>  <threads_per_node> <outdir_prefix>
"""
import sys, subprocess, threading, time, re, os

MODE = sys.argv[1]
WORKLOAD = sys.argv[2]
T = int(sys.argv[3])
OUT = sys.argv[4]            # e.g. raw/ycsb_paper_workloada_T2_rep1

NODES = [
    # name, host, workdir, config
    ("node0", "b2", "~/FUSEE/build/ycsb-test" if MODE=="ycsb" else "~/FUSEE/build/micro-test",
     "client_config.json" if MODE=="ycsb" else "client_config_2cn.json"),
    ("node1", "b4", "~/FUSEE/build/ycsb-test" if MODE=="ycsb" else "~/FUSEE/build/micro-test",
     "client_config_2cn.json"),
]

if MODE == "ycsb":
    BIN = "./ycsb_test_multi_client"
    REMOTE = lambda cfg: f"{BIN} {cfg} {WORKLOAD} {T}"
else:
    BIN = "./micro_test_multi_client"
    REMOTE = lambda cfg: f"{BIN} {cfg} {T}"

procs, logs, lines, locks = {}, {}, {}, {}
done_launch_ts = None

def reader(name, p):
    for raw in iter(p.stdout.readline, ''):
        line = raw.rstrip("\n")
        with locks[name]:
            lines[name].append(line)
        logs[name].write(line + "\n"); logs[name].flush()
    p.stdout.close()

def count_marker(name, pat):
    rx = re.compile(pat)
    with locks[name]:
        return sum(1 for l in lines[name] if rx.search(l))

def has_marker(name, pat):
    return count_marker(name, pat) >= 1

def release_all(tag):
    for name in procs:
        try:
            procs[name].stdin.write("\n"); procs[name].stdin.flush()
        except Exception as e:
            print(f"[orch] release {tag} write to {name} failed: {e}")
    print(f"[orch] RELEASED ({tag}) at +{time.time()-done_launch_ts:.1f}s")

def wait_until(pred, timeout, what):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if pred():
            return True
        time.sleep(0.2)
    print(f"[orch] TIMEOUT waiting for {what} ({timeout}s)")
    return False

# launch
os.makedirs(os.path.dirname(OUT), exist_ok=True)
for name, host, wd, cfg in NODES:
    cmd = ["ssh", "-tt", host, f"cd {wd} && stdbuf -oL {REMOTE(cfg)}"]
    lf = open(f"{OUT}.{name}.log", "w")
    logs[name] = lf; lines[name] = []; locks[name] = threading.Lock()
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, bufsize=1)
    procs[name] = p
    threading.Thread(target=reader, args=(name, p), daemon=True).start()
    print(f"[orch] launched {name}={host}: {REMOTE(cfg)}")
done_launch_ts = time.time()

if MODE == "ycsb":
    # node0 must finish load ("time spent:"); node1 at "press to start!"
    ok = wait_until(lambda: has_marker("node0", r"time spent:") and
                            has_marker("node1", r"press to start"),
                    timeout=120, what="node0 load done + node1 ready")
    if ok:
        time.sleep(0.5)
        release_all("trans-start")
else:
    PHASES = ["INSERT", "READ", "UPDATE", "DELETE"]
    for k, op in enumerate(PHASES, start=1):
        ok = wait_until(lambda k=k: count_marker("node0", r"press to sync start") >= k and
                                    count_marker("node1", r"press to sync start") >= k,
                        timeout=120, what=f"both at sync point {k} ({op})")
        if not ok:
            break
        time.sleep(0.3)
        release_all(f"phase{k}-{op}")

# wait for completion
for name, p in procs.items():
    try:
        p.wait(timeout=120)
    except subprocess.TimeoutExpired:
        print(f"[orch] {name} did not exit; killing"); p.kill()
    logs[name].flush()

# parse + sum
def parse_tpt(name, pat):
    rx = re.compile(pat)
    vals = [int(m.group(1)) for l in lines[name] for m in [rx.search(l)] if m]
    return vals[-1] if vals else None

print("\n========== RESULT %s %s T=%d (2 nodes, total clients=%d) ==========" %
      (MODE, WORKLOAD if MODE=="ycsb" else "micro", T, 2*T))
if MODE == "ycsb":
    agg = 0; detail = []
    for name, *_ in NODES:
        v = parse_tpt(name, r"^tpt:\s*(\d+)\s*ops/s")
        detail.append((name, v)); agg += (v or 0)
    for name, v in detail:
        print(f"  {name}: tpt={v} ops/s")
    print(f"  AGGREGATE: {agg} ops/s = {agg/1e6:.4f} Mops/s")
    print(f"CSV,{MODE},{WORKLOAD},{T},{2*T}," + ",".join(str(v) for _,v in detail) + f",{agg}")
else:
    # Robust per-thread reconstruction. The pristine main "X tpt:" aggregation
    # is racy (observed: node0 search collapsed to 3896 while its per-thread
    # READ lines were healthy 214k/217k). Per-thread lines are ground truth:
    #   "thread: <tid> <P> ops/s"  with P = ops_cnt/10  (hardcoded /10)
    #   "<F> failed"
    # node tpt = sum over threads of (ops_cnt - F)*1000/phase_ms
    #          = sum (10*P - F)*1000/phase_ms
    # phase order in run_client: INSERT, READ(=search), UPDATE, DELETE
    PHASE_SEQ = [("insert", 500), ("search", 5000), ("update", 5000), ("delete", 500)]
    rx_thr = re.compile(r"thread:\s*\d+\s+(\d+)\s*ops/s")
    rx_fail = re.compile(r"^(\d+)\s+failed")
    rx_mark = re.compile(r"press to sync start")

    def node_phase_tpt(name):
        # segment lines into phases by the sync markers; collect (P, F) pairs
        with locks[name]:
            ls = list(lines[name])
        seg = -1; pend_P = None; out = {p: 0.0 for p, _ in PHASE_SEQ}
        for l in ls:
            if rx_mark.search(l):
                seg += 1; pend_P = None; continue
            if seg < 0 or seg >= len(PHASE_SEQ):
                continue
            mt = rx_thr.search(l)
            if mt:
                pend_P = int(mt.group(1)); continue
            mf = rx_fail.search(l)
            if mf and pend_P is not None:
                opname, ms = PHASE_SEQ[seg]
                ops_cnt = 10 * pend_P
                out[opname] += (ops_cnt - int(mf.group(1))) * 1000.0 / ms
                pend_P = None
        return out

    pernode = {name: node_phase_tpt(name) for name, *_ in NODES}
    aggs = {}
    for opn, _ in PHASE_SEQ:
        agg = sum(pernode[name][opn] for name, *_ in NODES)
        aggs[opn] = agg
        detail = " ".join(f"{name}={pernode[name][opn]:.0f}" for name, *_ in NODES)
        print(f"  {opn}: {detail}  AGG={agg:.0f} ({agg/1e6:.4f} Mops/s)")
    print("CSV,%s,micro,%d,%d,%.0f,%.0f,%.0f,%.0f" % (MODE, T, 2*T,
          aggs["insert"], aggs["search"], aggs["update"], aggs["delete"]))
