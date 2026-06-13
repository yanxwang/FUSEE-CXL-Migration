#!/usr/bin/env python3
"""
4-CN orchestrator for G4 (4CN+4MN co-located). CN pinned to NUMA0 (--cpunodebind=0
--membind=0); MN already on NUMA1 (interleaved mem). node0=b1 (server_id=memory_num=4)
loads the table; nodes 1-3 wait. Release getchar across all 4 when ready; sum tpt.
Usage: orch_g4.py ycsb|micro <workload> <T_per_node> <outprefix>   (total clients = 4*T)
"""
import sys, subprocess, threading, time, re, os
MODE, WORKLOAD, T, OUT = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
WD = "~/FUSEE/build/ycsb-test" if MODE=="ycsb" else "~/FUSEE/build/micro-test"
CFG = "client_config.json" if MODE=="ycsb" else "client_config_2cn.json"
NUMA = "numactl --cpunodebind=0 --membind=0"
REMOTE = (lambda: f"{NUMA} stdbuf -oL ./ycsb_test_multi_client {CFG} {WORKLOAD} {T}" if MODE=="ycsb"
          else f"{NUMA} stdbuf -oL ./micro_test_multi_client {CFG} {T}")()
NODES = ["b1","b2","b3","b4"]
os.makedirs(os.path.dirname(OUT), exist_ok=True)
procs={}; lines={}; locks={}; logs={}
def reader(n,p):
    for ln in iter(p.stdout.readline,''):
        with locks[n]: lines[n].append(ln.rstrip("\n"))
        logs[n].write(ln); logs[n].flush()
for n in NODES:
    logs[n]=open(f"{OUT}.{n}.log","w"); lines[n]=[]; locks[n]=threading.Lock()
    p=subprocess.Popen(["ssh","-tt",n,f"cd {WD} && {REMOTE}"],stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,bufsize=1)
    procs[n]=p; threading.Thread(target=reader,args=(n,p),daemon=True).start()
t0=time.time()
def cnt(n,pat):
    with locks[n]: return sum(1 for l in lines[n] if re.search(pat,l))
def waitfor(pred,to,what):
    s=time.time()
    while time.time()-s<to:
        if pred(): return True
        time.sleep(0.2)
    print(f"[g4] TIMEOUT {what}"); return False
def release():
    for n in NODES:
        try: procs[n].stdin.write("\n"); procs[n].stdin.flush()
        except Exception as e: print(f"[g4] rel {n} fail: {e}")
    print(f"[g4] RELEASED +{time.time()-t0:.1f}s")
if MODE=="ycsb":
    # node0 (b1) loads -> "time spent:"; others -> "press to start"
    ok=waitfor(lambda: cnt("b1",r"time spent:")>=1 and all(cnt(n,r"press to start")>=1 for n in NODES[1:]),
               300,"node0 load + others ready")
    if ok: time.sleep(0.5); release()
else:
    for k,op in enumerate(["INSERT","READ","UPDATE","DELETE"],1):
        if not waitfor(lambda k=k: all(cnt(n,r"press to sync start")>=k for n in NODES),300,f"sync{k}-{op}"): break
        time.sleep(0.3); release()
for n in NODES:
    try: procs[n].wait(timeout=300)
    except: procs[n].kill()
    logs[n].flush()
def last(n,pat):
    with locks[n]: v=[int(m.group(1)) for l in lines[n] for m in [re.search(pat,l)] if m]
    return v[-1] if v else None
print(f"\n===== G4 {MODE} {WORKLOAD if MODE=='ycsb' else 'micro'} T={T} (4CN, clients={4*T}) =====")
if MODE=="ycsb":
    agg=0; det=[]
    for n in NODES:
        v=last(n,r"^tpt:\s*(\d+)\s*ops/s"); det.append((n,v)); agg+=(v or 0)
    for n,v in det: print(f"  {n}: tpt={v}")
    print(f"  AGG={agg} ({agg/1e6:.4f} Mops/s)  per-node={'/'.join(str(v) for _,v in det)}")
    # CSV aligned to gen_results_doc (agg at field r[8] after the Ypaper,rep prefix)
    print("CSV,ycsb,%s,%d,%d,%d,0,%d"%(WORKLOAD,T,4*T,agg,agg))
else:
    PH=[("insert",500),("search",5000),("update",5000),("delete",500)]
    rx_t=re.compile(r"thread:\s*\d+\s+(\d+)\s*ops/s"); rx_f=re.compile(r"^(\d+)\s+failed"); rx_m=re.compile(r"press to sync start")
    tot={p_:0.0 for p_,_ in PH}
    for n in NODES:
        with locks[n]: L=list(lines[n])
        seg=-1; pend=None
        for l in L:
            if rx_m.search(l): seg+=1; pend=None; continue
            if seg<0 or seg>=len(PH): continue
            m=rx_t.search(l)
            if m: pend=int(m.group(1)); continue
            f=rx_f.search(l)
            if f and pend is not None:
                opn,ms=PH[seg]; tot[opn]+=(10*pend-int(f.group(1)))*1000.0/ms; pend=None
    print("CSV,micro,micro,%d,%d,%.0f,%.0f,%.0f,%.0f"%(T,4*T,tot["insert"],tot["search"],tot["update"],tot["delete"]))
    for opn,_ in PH: print(f"  {opn}={tot[opn]:.0f}")
