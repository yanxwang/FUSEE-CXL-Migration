#!/usr/bin/env python3
# Single-CN (b2) paper-method orchestrator for G3. 1 node loads + runs; feed getchar.
import sys, subprocess, threading, time, re, os
MODE, WORKLOAD, T, OUT = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
WD = "~/FUSEE/build/ycsb-test" if MODE=="ycsb" else "~/FUSEE/build/micro-test"
CFG = "client_config.json" if MODE=="ycsb" else "client_config_2cn.json"
REMOTE = (f"./ycsb_test_multi_client {CFG} {WORKLOAD} {T}" if MODE=="ycsb"
          else f"./micro_test_multi_client {CFG} {T}")
os.makedirs(os.path.dirname(OUT), exist_ok=True)
lines=[]; lock=threading.Lock()
lf=open(f"{OUT}.node0.log","w")
p=subprocess.Popen(["ssh","-tt","b2",f"cd {WD} && stdbuf -oL {REMOTE}"],
    stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,bufsize=1)
def rd():
    for ln in iter(p.stdout.readline,''):
        with lock: lines.append(ln.rstrip("\n"))
        lf.write(ln); lf.flush()
threading.Thread(target=rd,daemon=True).start()
t0=time.time()
def cnt(pat): 
    with lock: return sum(1 for l in lines if re.search(pat,l))
def waitfor(pred,to,what):
    s=time.time()
    while time.time()-s<to:
        if pred(): return True
        time.sleep(0.2)
    print(f"[orch1] TIMEOUT {what}"); return False
def rel(tag):
    try: p.stdin.write("\n"); p.stdin.flush(); print(f"[orch1] REL {tag} +{time.time()-t0:.1f}s")
    except Exception as e: print("rel fail",e)
if MODE=="ycsb":
    if waitfor(lambda: cnt(r"time spent:")>=1, 180, "load"): time.sleep(0.3); rel("trans")
else:
    for k,op in enumerate(["INSERT","READ","UPDATE","DELETE"],1):
        if not waitfor(lambda k=k: cnt(r"press to sync start")>=k,180,f"sync{k}"): break
        time.sleep(0.3); rel(op)
try: p.wait(timeout=180)
except: p.kill()
lf.flush()
with lock: L=list(lines)
def last(pat):
    v=[int(m.group(1)) for l in L for m in [re.search(pat,l)] if m]; return v[-1] if v else None
print(f"\n===== {MODE} {WORKLOAD if MODE=='ycsb' else 'micro'} T={T} (1CN, clients={T}) =====")
if MODE=="ycsb":
    v=last(r"^tpt:\s*(\d+)\s*ops/s"); print(f"  tpt={v}")
    print(f"CSV,ycsb,{WORKLOAD},{T},{T},{v},0,{v}")
else:
    PH=[("insert",500),("search",5000),("update",5000),("delete",500)]
    rx_t=re.compile(r"thread:\s*\d+\s+(\d+)\s*ops/s"); rx_f=re.compile(r"^(\d+)\s+failed"); rx_m=re.compile(r"press to sync start")
    seg=-1; pend=None; out={p_:0.0 for p_,_ in PH}
    for l in L:
        if rx_m.search(l): seg+=1; pend=None; continue
        if seg<0 or seg>=len(PH): continue
        m=rx_t.search(l)
        if m: pend=int(m.group(1)); continue
        f=rx_f.search(l)
        if f and pend is not None:
            opn,ms=PH[seg]; out[opn]+=(10*pend-int(f.group(1)))*1000.0/ms; pend=None
    print("CSV,micro,micro,%d,%d,%.0f,%.0f,%.0f,%.0f"%(T,T,out["insert"],out["search"],out["update"],out["delete"]))
    for opn,_ in PH: print(f"  {opn}={out[opn]:.0f}")
