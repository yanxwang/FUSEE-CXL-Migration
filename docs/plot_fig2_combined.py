import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt, csv, statistics as st, sys, os
from collections import defaultdict
DIR,CSVN,LABEL=sys.argv[1],sys.argv[2],sys.argv[3]; os.chdir(DIR)
COL={"search":"#1f77b4","insert":"#2ca02c","update":"#ff7f0e","delete":"#d62728"}
rows=[r for r in csv.reader(open(CSVN))][1:]
def mean(x): return st.mean(x) if x else 0
m2=defaultdict(lambda:defaultdict(list))
for r in rows:
    if len(r)>=10 and r[0]=="M2":
        cl=int(r[5])
        for op,v in zip(["insert","search","update","delete"],map(int,r[6:10])): m2[op][cl].append(v)
mcls=sorted({c for op in m2 for c in m2[op]})
order=["search","insert","update","delete"]
peak={op:(max((mean(v) for v in m2[op].values()),default=0)/1e6) for op in order}

fig,(axL,axR)=plt.subplots(1,2,figsize=(13,4.8))
fig.suptitle(f"FUSEE Microbench Throughput  ({LABEL})",fontsize=14,y=0.98,fontweight="bold")
# left: throughput vs clients
for op in order:
    ys=[mean(m2[op].get(c,[]))/1e6 for c in mcls]
    axL.plot(mcls,ys,"-o",label=op,color=COL[op],lw=1.8,ms=5)
axL.set_xlabel("# clients"); axL.set_ylabel("Throughput (Mops/s)"); axL.set_xticks(mcls)
axL.legend(fontsize=9); axL.grid(True,ls=":",alpha=0.5); axL.set_title("(a) Throughput vs. #clients",fontsize=12)
# right: peak bar
bars=axR.bar(range(len(order)),[peak[o] for o in order],color=[COL[o] for o in order],width=0.6)
for b,o in zip(bars,order): axR.text(b.get_x()+b.get_width()/2,b.get_height()+max(peak.values())*0.015,f"{peak[o]:.2f}",ha="center",fontsize=10,fontweight="bold")
axR.set_xticks(range(len(order))); axR.set_xticklabels(order); axR.set_ylabel("Peak throughput (Mops/s)")
axR.set_ylim(0,max(peak.values())*1.18); axR.grid(True,axis="y",ls=":",alpha=0.5); axR.set_title("(b) Peak throughput",fontsize=12)
fig.subplots_adjust(top=0.86,bottom=0.12,left=0.07,right=0.97,wspace=0.22)
fig.savefig("fig2_micro_thpt.png",dpi=130); print(f"{DIR}: fig2 combined (line+bar)")
