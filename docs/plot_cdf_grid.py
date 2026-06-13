import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
import sys, os
DIR, LABEL = sys.argv[1], sys.argv[2]
os.chdir(DIR)
# reference order/colors: (a)INSERT red (b)UPDATE orange (c)SEARCH blue (d)DELETE green
SPEC=[("INSERT","insert","#d62728","a"),("UPDATE","update","#ff7f0e","b"),
      ("SEARCH","search","#1f77b4","c"),("DELETE","delete","#2ca02c","d")]
def loadlat(op):
    for c in (f"raw/lat_{op}_FIXED_rep1.txt", f"raw/lat_{op}_rep1.txt"):
        if os.path.exists(c) and os.path.getsize(c)>0:
            return sorted(int(x) for x in open(c) if x.strip())
    return None
fig,axes=plt.subplots(2,2,figsize=(13,8.5))
fig.suptitle(f"FUSEE KV Request Latency — single-client CDFs  ({LABEL})",fontsize=14,y=0.98,fontweight="bold")
for (NAME,op,col,tag),ax in zip(SPEC,axes.flat):
    xs=loadlat(op)
    if not xs: ax.set_title(f"({tag}) {NAME} — no data"); continue
    n=len(xs); ys=[(i+1)/n for i in range(n)]
    p50=xs[int(n*.5)]; p99=xs[int(n*.99)]; p999=xs[int(n*.999)]
    xmax=max(p99*1.5, p99+3)
    ax.plot(xs,ys,color=col,lw=2.2,label=f"FUSEE ({LABEL.split(',')[0]})")
    ax.axvline(p50,ls=":",color=col,alpha=0.6,lw=1.5)
    ax.axvline(p99,ls="--",color=col,alpha=0.6,lw=1.5)
    ax.annotate(f"p50={p50}µs",xy=(p50,0.5),xytext=(xmax*0.6,0.42),fontsize=9,color=col,
                arrowprops=dict(arrowstyle="-",color=col,alpha=0.5,lw=0.8))
    ax.annotate(f"p99={p99}µs",xy=(p99,0.97),xytext=(xmax*0.6,0.90),fontsize=9,color=col,
                arrowprops=dict(arrowstyle="-",color=col,alpha=0.5,lw=0.8))
    # note the cut tail if p99.9 is far beyond axis
    if p999 > xmax*1.5:
        ax.text(0.97,0.06,f"(0.1% tail → ~{p999/1000:.1f}ms, off-axis)",transform=ax.transAxes,
                ha="right",fontsize=8,color="gray",style="italic")
    ax.set_xlim(0,xmax); ax.set_ylim(0,1.005)
    ax.set_xlabel("Latency (µs)"); ax.set_ylabel("CDF")
    ax.set_title(f"({tag}) {NAME} latency CDF",fontsize=12)
    ax.grid(True,ls=":",alpha=0.5); ax.legend(loc="lower right",fontsize=9)
fig.subplots_adjust(top=0.91,bottom=0.08,left=0.07,right=0.97,hspace=0.32,wspace=0.18)
fig.savefig("fig1_latency_cdf.png",dpi=130); print(f"{DIR}: fig1 grid written")
