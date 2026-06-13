import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt, csv, statistics as st, sys, os
from collections import defaultdict
DIR,CSVN,LABEL=sys.argv[1],sys.argv[2],sys.argv[3]; os.chdir(DIR)
rows=[r for r in csv.reader(open(CSVN))][1:]
def mean(x): return st.mean(x) if x else 0
yp=defaultdict(lambda:defaultdict(list)); yl=defaultdict(lambda:defaultdict(list))
for r in rows:
    if len(r)<6: continue
    if r[0]=="Ypaper" and len(r)>=9:
        wl=r[3].replace("workload",""); cl=int(r[5]); yp[wl][cl].append(int(r[8]) if r[6]!='None' else 0)
    elif r[0]=="Ylw" and len(r)>=8:
        wl=r[3].replace("workload",""); N=int(r[4]); yl[wl][N].append(int(r[7]))
cls=sorted({c for wl in yp for c in yp[wl]})
WC={"a":"#d62728","b":"#ff7f0e","c":"#1f77b4","d":"#2ca02c"}
NM={"a":"A (50r/50u)","b":"B (95r/5u)","c":"C (100r)","d":"D (95r/5i)"}
def val(wl,c):
    v=mean(yp[wl].get(c,[]))
    if v>0: return v/1e6
    if yl[wl].get(c): return mean(yl[wl][c])/1e6
    return None
fig,(axL,axR)=plt.subplots(1,2,figsize=(13,4.8))
fig.suptitle(f"FUSEE YCSB Throughput  ({LABEL})",fontsize=14,y=0.98,fontweight="bold")
# left: line
for wl in "cdba":
    pts=[(c,val(wl,c)) for c in cls if val(wl,c) is not None]
    axL.plot([p[0] for p in pts],[p[1] for p in pts],"-o",label=NM[wl],color=WC[wl],lw=1.8,ms=5)
axL.set_xlabel("# clients"); axL.set_ylabel("Throughput (Mops/s)"); axL.set_xticks(cls)
axL.legend(fontsize=9); axL.grid(True,ls=":",alpha=0.5); axL.set_title("(a) Throughput vs. #clients",fontsize=12)
# right: peak bar (A,B,C,D order)
order=["a","b","c","d"]
peak={wl:max((v for c in cls if (v:=val(wl,c)) is not None),default=0) for wl in order}
bars=axR.bar(range(len(order)),[peak[w] for w in order],color=[WC[w] for w in order],width=0.6)
for b,w in zip(bars,order): axR.text(b.get_x()+b.get_width()/2,b.get_height()+max(peak.values())*0.015,f"{peak[w]:.2f}",ha="center",fontsize=10,fontweight="bold")
axR.set_xticks(range(len(order))); axR.set_xticklabels([NM[w] for w in order],fontsize=9)
axR.set_ylabel("Peak throughput (Mops/s)"); axR.set_ylim(0,max(peak.values())*1.18)
axR.grid(True,axis="y",ls=":",alpha=0.5); axR.set_title("(b) Peak throughput",fontsize=12)
fig.subplots_adjust(top=0.86,bottom=0.13,left=0.07,right=0.97,wspace=0.22)
fig.savefig("fig3_ycsb_thpt.png",dpi=130); print(f"{DIR}: fig3 combined (line+bar)")
