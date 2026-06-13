import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
import csv, statistics as st, sys, os
from collections import defaultdict
DIR,CSVN,LABEL=sys.argv[1],sys.argv[2],sys.argv[3]
os.chdir(DIR)
rows=[r for r in csv.reader(open(CSVN))][1:]
def mean(x): return st.mean(x) if x else 0
m1=defaultdict(lambda:defaultdict(list)); m2=defaultdict(lambda:defaultdict(list))
yp=defaultdict(lambda:defaultdict(list)); yl=defaultdict(lambda:defaultdict(list))
for r in rows:
    if len(r)<6: continue
    if r[0]=="M1":
        op=r[3]; p=r[5].split()
        if p[0]!="NA": m1[op]["mean"].append(int(p[0])); m1[op]["p50"].append(int(p[1])); m1[op]["p99"].append(int(p[2]))
    elif r[0]=="M2" and len(r)>=10:
        cl=int(r[5])
        for op,v in zip(["insert","search","update","delete"],map(int,r[6:10])): m2[op][cl].append(v)
    elif r[0]=="Ypaper" and len(r)>=9:
        wl=r[3].replace("workload",""); cl=int(r[5]); yp[wl][cl].append(int(r[8]) if r[6]!='None' else 0)
    elif r[0]=="Ylw" and len(r)>=8:
        wl=r[3].replace("workload",""); N=int(r[4]); yl[wl][N].append(int(r[7]))
mcls=sorted({c for op in m2 for c in m2[op]}); ycls=sorted({c for wl in yp for c in yp[wl]})

fig=plt.figure(figsize=(11,9)); fig.suptitle(f"FUSEE Benchmark Results  ({LABEL})",fontsize=14,y=0.985,fontweight="bold")
def addtbl(pos,title,colh,data,h):
    ax=fig.add_axes(pos); ax.axis("off")
    ax.set_title(title,fontsize=11,fontweight="bold",pad=4,loc="left")
    t=ax.table(cellText=data,colLabels=colh,loc="center",cellLoc="center")
    t.auto_set_font_size(False); t.set_fontsize(9.5); t.scale(1,h)
    for (rr,cc),cell in t.get_celld().items():
        if rr==0: cell.set_facecolor("#33548c"); cell.set_text_props(color="w",fontweight="bold")
        elif cc==0: cell.set_facecolor("#e8edf5"); cell.set_text_props(fontweight="bold")

# Part 1 latency
d1=[[op,f"{mean(m1[op]['mean']):.0f}",f"{mean(m1[op]['p50']):.0f}",f"{mean(m1[op]['p99']):.0f}"] for op in ["search","insert","update","delete"]]
addtbl([0.06,0.66,0.40,0.24],"1. KV Request Latency (µs)  [1 client]",["op","mean","p50","p99"],d1,1.5)
# Part 2 micro thpt
d2=[[op]+[f"{mean(m2[op].get(c,[]))/1e6:.2f}" for c in mcls] for op in ["search","insert","update","delete"]]
addtbl([0.54,0.66,0.42,0.24],"2. Microbench Throughput (Mops/s)",["op"]+[f"{c}cl" for c in mcls],d2,1.5)
# Part 3 YCSB (paper / lw)
NM={"a":"A 50r/50u","b":"B 95r/5u","c":"C 100r","d":"D 95r/5i"}
def cell(wl,c):
    p=mean(yp[wl].get(c,[])); l=mean(yl[wl].get(c,[]))
    ps="CRASH" if (c in yp[wl] and p==0) else (f"{p/1e6:.2f}" if yp[wl].get(c) else "-")
    ls=f"{l/1e6:.2f}" if yl[wl].get(c) else "-"
    return f"{ps}/{ls}"
d3=[[NM[wl]]+[cell(wl,c) for c in ycls] for wl in "abcd"]
addtbl([0.06,0.06,0.90,0.46],"3. YCSB Throughput (Mops/s, paper / lw)",["workload"]+[f"{c}cl" for c in ycls],d3,2.2)
fig.savefig("fig4_tables.png",dpi=130,bbox_inches="tight"); print(f"{DIR}: fig4_tables.png written")
