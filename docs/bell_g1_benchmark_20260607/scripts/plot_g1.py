import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
import csv, statistics as st
from collections import defaultdict

GROUP="G1 (2CN+2MN, data=2, idx=1)"
OPS=[("search","lat_search_rep1.txt"),("insert","lat_insert_rep1.txt"),
     ("update","lat_update_rep1.txt"),("delete","lat_delete_FIXED_rep1.txt")]
COL={"search":"#1f77b4","insert":"#2ca02c","update":"#ff7f0e","delete":"#d62728"}

# ---------- Fig 1: latency CDF (Fig-10 style) ----------
plt.figure(figsize=(6,4.2))
for op,fn in OPS:
    xs=sorted(int(x) for x in open(f"raw/{fn}"))
    n=len(xs); ys=[(i+1)/n for i in range(n)]
    plt.plot(xs,ys,label=f"{op} (p50={xs[n//2]}µs)",color=COL[op],lw=1.8)
plt.xscale("log"); plt.xlabel("Latency (µs)"); plt.ylabel("CDF")
plt.title(f"Fig 1. KV op latency CDF — {GROUP}")
plt.grid(True,which="both",ls=":",alpha=0.5); plt.ylim(0,1.005); plt.legend(loc="lower right",fontsize=8)
plt.tight_layout(); plt.savefig("fig1_latency_cdf.png",dpi=130); plt.close()

# ---------- parse results CSV ----------
rows=[r for r in csv.reader(open("results_g1.csv"))][1:]
def mean(x): return st.mean(x) if x else 0
m2=defaultdict(lambda:defaultdict(list)); yp=defaultdict(lambda:defaultdict(list))
for r in rows:
    if len(r)<6: continue
    if r[0]=="M2":
        cl=int(r[5])
        for op,v in zip(["insert","search","update","delete"],map(int,r[6:10])): m2[op][cl].append(v)
    elif r[0]=="Ypaper" and len(r)>=9:
        wl=r[3].replace("workload",""); cl=int(r[5]); yp[wl][cl].append(int(r[8]) if r[6]!='None' else 0)

# ---------- Fig 2: micro throughput vs clients (4 ops) ----------
cls=[2,4,8,16,28]
plt.figure(figsize=(6,4.2))
for op in ["search","insert","delete","update"]:
    ys=[mean(m2[op].get(c,[]))/1e6 for c in cls]
    plt.plot(cls,ys,"-o",label=op,color=COL[op],lw=1.8,ms=5)
plt.xlabel("# clients"); plt.ylabel("Throughput (Mops/s)")
plt.title(f"Fig 2. Micro throughput vs clients — {GROUP}")
plt.grid(True,ls=":",alpha=0.5); plt.xticks(cls); plt.legend(fontsize=9)
plt.tight_layout(); plt.savefig("fig2_micro_thpt.png",dpi=130); plt.close()

# ---------- Fig 3: YCSB throughput vs clients (4 workloads, paper) ----------
WC={"a":"#d62728","b":"#ff7f0e","c":"#1f77b4","d":"#2ca02c"}
NM={"a":"A (50r/50u)","b":"B (95r/5u)","c":"C (100r)","d":"D (95r/5i)"}
plt.figure(figsize=(6,4.2))
for wl in "cdba":
    pts=[(c,mean(yp[wl].get(c,[]))/1e6) for c in cls if yp[wl].get(c) and mean(yp[wl][c])>0]
    if pts: plt.plot([p[0] for p in pts],[p[1] for p in pts],"-o",label=NM[wl],color=WC[wl],lw=1.8,ms=5)
plt.xlabel("# clients"); plt.ylabel("Throughput (Mops/s)")
plt.title(f"Fig 3. YCSB throughput vs clients (paper method) — {GROUP}")
plt.grid(True,ls=":",alpha=0.5); plt.xticks(cls); plt.legend(fontsize=9)
plt.tight_layout(); plt.savefig("fig3_ycsb_thpt.png",dpi=130); plt.close()
print("wrote fig1_latency_cdf.png, fig2_micro_thpt.png, fig3_ycsb_thpt.png")
