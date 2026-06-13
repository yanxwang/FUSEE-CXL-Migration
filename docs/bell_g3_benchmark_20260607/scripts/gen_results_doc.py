#!/usr/bin/env python3
# Generate the STANDARD 3-table RESULTS.md from a group's results CSV.
# Usage: gen_results_doc.py <results_csv> <group_label> <out_md>
import csv, statistics as st, sys
from collections import defaultdict
csvf, label, outf = sys.argv[1], sys.argv[2], sys.argv[3]
rows=list(csv.reader(open(csvf)))[1:]
def mean(x): return st.mean(x) if x else 0
m1=defaultdict(lambda:defaultdict(list)); m2=defaultdict(lambda:defaultdict(list))
yp=defaultdict(list); yl=defaultdict(list)
clset=set()
for r in rows:
    if len(r)<6: continue
    if r[0]=="M1":
        op=r[3]; p=r[5].split()
        if p[0]!="NA": m1[op]["mean"].append(int(p[0])); m1[op]["p50"].append(int(p[1])); m1[op]["p99"].append(int(p[2]))
    elif r[0]=="M2":
        cl=int(r[5]); clset.add(cl)
        for op,v in zip(["insert","search","update","delete"],map(int,r[6:10])): m2[cl][op].append(v)
    elif r[0]=="Ypaper":
        if len(r)<9: continue
        wl=r[3].replace("workload",""); cl=int(r[5]); yp[(wl,cl)].append(int(r[8]) if r[6]!='None' else 0)
    elif r[0]=="Ylw":
        if len(r)<8: continue
        wl=r[3].replace("workload",""); N=int(r[4]); yl[(wl,N)].append(int(r[7]))
cls=sorted(clset) or [2,4,8,16,28]
ycls=sorted({c for _,c in list(yp)+list(yl)}) or cls
o=[]
o.append(f"# {label} — Standard Results\n")
o.append("Pristine RDMA FUSEE on bell. value=1024B. 3-rep mean. Both YCSB methods (paper / lw).\n")
o.append("## Part 1 — Micro latency (1 client, 1 coroutine, sequential sync, µs)\n")
o.append("| op | mean | p50 | p99 |\n|---|---:|---:|---:|")
for op in ["search","insert","update","delete"]:
    d=m1.get(op);
    if d: o.append(f"| {op} | {mean(d['mean']):.0f} | {mean(d['p50']):.0f} | {mean(d['p99']):.0f} |")
    else: o.append(f"| {op} | - | - | - |")
o.append("\n## Part 2 — Micro throughput (Mops/s)\n")
o.append("| clients | insert | search | update | delete |\n|---:|---:|---:|---:|---:|")
for cl in cls:
    d=m2.get(cl,{})
    o.append(f"| {cl} | {mean(d.get('insert',[]))/1e6:.3f} | {mean(d.get('search',[]))/1e6:.3f} | {mean(d.get('update',[]))/1e6:.3f} | {mean(d.get('delete',[]))/1e6:.3f} |")
o.append("\n## Part 3 — YCSB throughput (Mops/s, paper / lw)\n")
o.append("| workload | "+" | ".join(f"{c}cl" for c in ycls)+" |")
o.append("|---|"+"|".join("---:" for _ in ycls)+"|")
names={"a":"a (50r/50u)","b":"b (95r/5u)","c":"c (100r)","d":"d (95r/5i)"}
for wl in "abcd":
    cells=[]
    for c in ycls:
        p=mean(yp.get((wl,c),[])); l=mean(yl.get((wl,c),[]))
        ps="CRASH" if (wl,c) in yp and p==0 else (f"{p/1e6:.2f}" if yp.get((wl,c)) else "-")
        ls=f"{l/1e6:.2f}" if yl.get((wl,c)) else "-"
        cells.append(f"{ps}/{ls}")
    o.append(f"| {names[wl]} | "+" | ".join(cells)+" |")
open(outf,"w").write("\n".join(o)+"\n")
print("wrote",outf)
