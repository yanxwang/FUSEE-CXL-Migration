import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
import csv, statistics as st, sys, os
from collections import defaultdict

DIR, CSVN, LABEL = sys.argv[1], sys.argv[2], sys.argv[3]   # dir, csv filename, "2CN+2MN, data rep=2, index rep=1"
os.chdir(DIR)
COL={"search":"#1f77b4","insert":"#2ca02c","update":"#ff7f0e","delete":"#d62728"}
def style(ax,title):
    ax.set_title(title, fontsize=12, pad=14)
    ax.grid(True, ls=":", alpha=0.5)

def latfile(op):
    for c in (f"raw/lat_{op}_FIXED_rep1.txt", f"raw/lat_{op}_rep1.txt"):
        if os.path.exists(c) and os.path.getsize(c)>0: return c
    return None

# ---- Fig 1: latency CDF ----
fig,ax=plt.subplots(figsize=(7,4.6))
for op in ["search","insert","update","delete"]:
    f=latfile(op)
    if not f: continue
    xs=sorted(int(x) for x in open(f) if x.strip()); n=len(xs)
    if n==0: continue
    ax.plot(xs,[(i+1)/n for i in range(n)],label=f"{op} (p50={xs[n//2]}µs)",color=COL[op],lw=1.8)
ax.set_xscale("log"); ax.set_xlabel("Latency (µs)"); ax.set_ylabel("CDF"); ax.set_ylim(0,1.005)
ax.legend(loc="lower right",fontsize=9); style(ax,f"FUSEE KV Request Latency  ({LABEL})")
fig.subplots_adjust(top=0.88,bottom=0.13,left=0.11,right=0.97); fig.savefig("fig1_latency_cdf.png",dpi=130); plt.close(fig)

# ---- parse CSV ----
rows=[r for r in csv.reader(open(CSVN))][1:]
def mean(x): return st.mean(x) if x else 0
m2=defaultdict(lambda:defaultdict(list)); yp=defaultdict(lambda:defaultdict(list))
for r in rows:
    if len(r)<6: continue
    if r[0]=="M2" and len(r)>=10:
        cl=int(r[5])
        for op,v in zip(["insert","search","update","delete"],map(int,r[6:10])): m2[op][cl].append(v)
    elif r[0]=="Ypaper" and len(r)>=9:
        wl=r[3].replace("workload",""); cl=int(r[5]); yp[wl][cl].append(int(r[8]) if r[6]!='None' else 0)
mcls=sorted({c for op in m2 for c in m2[op]})
ycls=sorted({c for wl in yp for c in yp[wl]})

# ---- Fig 2: micro throughput vs clients ----
fig,ax=plt.subplots(figsize=(7,4.6))
for op in ["search","insert","update","delete"]:
    ys=[mean(m2[op].get(c,[]))/1e6 for c in mcls]
    ax.plot(mcls,ys,"-o",label=op,color=COL[op],lw=1.8,ms=5)
ax.set_xlabel("# clients"); ax.set_ylabel("Throughput (Mops/s)"); ax.set_xticks(mcls); ax.legend(fontsize=9)
style(ax,f"FUSEE Microbench Throughput  ({LABEL})")
fig.subplots_adjust(top=0.88,bottom=0.13,left=0.11,right=0.97); fig.savefig("fig2_micro_thpt.png",dpi=130); plt.close(fig)

# ---- Fig 2b: micro peak bar ----
order=["search","insert","update","delete"]
peak={op:(max((mean(v) for v in m2[op].values()),default=0)/1e6) for op in order}
fig,ax=plt.subplots(figsize=(7.5,4.6))
bars=ax.bar(range(len(order)),[peak[o] for o in order],color=[COL[o] for o in order],width=0.6)
for b,o in zip(bars,order): ax.text(b.get_x()+b.get_width()/2,b.get_height()+max(peak.values())*0.015,f"{peak[o]:.2f}",ha="center",fontsize=10,fontweight="bold")
ax.set_xticks(range(len(order))); ax.set_xticklabels(order); ax.set_ylabel("Peak throughput (Mops/s)")
ax.set_ylim(0,max(peak.values())*1.18); ax.grid(True,axis="y",ls=":",alpha=0.5)
ax.set_title(f"FUSEE Microbench Throughput  ({LABEL})",fontsize=12,pad=14)
fig.subplots_adjust(top=0.88,bottom=0.10,left=0.12,right=0.96); fig.savefig("fig2b_micro_peak_bar.png",dpi=130); plt.close(fig)

# ---- Fig 3: YCSB throughput vs clients ----
WC={"a":"#d62728","b":"#ff7f0e","c":"#1f77b4","d":"#2ca02c"}
NM={"a":"A (50r/50u)","b":"B (95r/5u)","c":"C (100r)","d":"D (95r/5i)"}
fig,ax=plt.subplots(figsize=(7,4.6))
for wl in "cdba":
    pts=[(c,mean(yp[wl].get(c,[]))/1e6) for c in ycls if yp[wl].get(c) and mean(yp[wl][c])>0]
    if pts: ax.plot([p[0] for p in pts],[p[1] for p in pts],"-o",label=NM[wl],color=WC[wl],lw=1.8,ms=5)
ax.set_xlabel("# clients"); ax.set_ylabel("Throughput (Mops/s)"); ax.set_xticks(ycls); ax.legend(fontsize=9)
style(ax,f"FUSEE YCSB Throughput  ({LABEL})")
fig.subplots_adjust(top=0.88,bottom=0.13,left=0.11,right=0.97); fig.savefig("fig3_ycsb_thpt.png",dpi=130); plt.close(fig)
print(f"{DIR}: 4 figs written ({LABEL})")
