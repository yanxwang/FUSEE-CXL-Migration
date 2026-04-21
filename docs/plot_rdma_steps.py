#!/usr/bin/env python3
"""
RDMA consensus: one PNG per step.
Compute nodes top, memory nodes bottom, vertically aligned.
"""

import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch
import matplotlib.patches as mpatches

plt.rcParams.update({
    "font.family": "sans-serif",
    "font.sans-serif": ["DejaVu Sans"],
    "font.size": 9,
    "figure.facecolor": "white",
})

C = {
    "comp": "#1565C0", "comp_l": "#BBDEFB",
    "compb": "#C62828", "compb_l": "#FFCDD2",
    "mem": "#2E7D32", "mem_l": "#C8E6C9",
    "idx": "#FFF9C4", "data": "#E3F2FD", "log": "#F3E5F5",
    "gray": "#9E9E9E", "dg": "#333333", "white": "#FFFFFF",
    "commit": "#FF6F00", "ok": "#1B5E20", "fail": "#B71C1C",
    "hi": "#FF8F00",
}

# ── Layout constants ──
# 3 columns, each holding one compute node (top) + one memory node (bottom)
COL_X = [0.5, 5.5, 10.5]  # left edge of each column
NW, NH_C, NH_M = 4.0, 1.8, 2.8  # node width, compute height, memory height
CY = 5.5   # compute node Y
MY = 0.5   # memory node Y
GAP_MID = (CY + NH_C + MY) / 2  # midpoint for arrows / labels


def _box(ax, x, y, w, h, fc, ec, lw=2):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.1",
                 facecolor=fc, edgecolor=ec, lw=lw, zorder=3))

def _region(ax, x, y, w, h, fc, title, sub="", hi=False):
    ec = C["hi"] if hi else C["gray"]
    lw = 2.5 if hi else 0.7
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.04",
                 facecolor=fc, edgecolor=ec, lw=lw, zorder=4))
    if sub:
        ax.text(x+w/2, y+h/2+0.1, title, ha="center", va="center",
                fontsize=7.5, color=C["dg"], weight="bold", zorder=5)
        ax.text(x+w/2, y+h/2-0.12, sub, ha="center", va="center",
                fontsize=7, color=C["ok"] if hi else C["gray"],
                family="monospace", weight="bold" if hi else "normal", zorder=5)
    else:
        ax.text(x+w/2, y+h/2, title, ha="center", va="center",
                fontsize=7.5, color=C["dg"], weight="bold", zorder=5)

def _lbl(ax, x, y, t, fs=9, c="black", bold=True, ha="center", va="center"):
    ax.text(x, y, t, ha=ha, va=va, fontsize=fs, color=c,
            weight="bold" if bold else "normal", zorder=8)

def _arr(ax, x0, y0, x1, y1, c, lw=2, rad=0, dash=False):
    ax.add_patch(FancyArrowPatch(
        (x0, y0), (x1, y1), arrowstyle="->,head_length=0.35,head_width=0.2",
        connectionstyle=f"arc3,rad={rad}", color=c, lw=lw,
        linestyle="--" if dash else "-", mutation_scale=12, zorder=7))

def _step_badge(ax, n, bg=None):
    bg = bg or C["comp"]
    ax.add_patch(plt.Circle((0.6, 8.6), 0.35, fc=bg, ec="white", lw=2, zorder=9))
    ax.text(0.6, 8.6, str(n), ha="center", va="center",
            fontsize=14, color="white", weight="bold", zorder=10)


def _draw_base(ax, step_num, title, title_color=C["comp"],
               # per-MN state
               mn0_idx="Slot X = 0x0", mn1_idx="Slot X = 0x0", mn2_idx="Slot X = 0x0",
               mn0_kv="(empty)", mn1_kv="(empty)", mn2_kv="(empty)",
               mn0_log="old_value = 0", mn1_log="old_value = 0", mn2_log="old_value = 0",
               hi_idx=(False, False, False),
               hi_kv=(False, False, False),
               hi_log=(False, False, False),
               show_b=True):
    """Draw the static scene with given state. Returns ax."""
    ax.set_xlim(-0.3, 15.3)
    ax.set_ylim(-0.3, 9.2)
    ax.set_aspect("equal")
    ax.axis("off")

    # title
    _lbl(ax, 7.5, 8.6, f"Step {step_num}: {title}", 12, title_color)
    _step_badge(ax, step_num, title_color)

    # ── Compute nodes (top row) ──
    # Node A above MN0
    _box(ax, COL_X[0], CY, NW, NH_C, C["comp_l"], C["comp"])
    _lbl(ax, COL_X[0]+NW/2, CY+NH_C-0.2, "Compute Node A", 10, C["comp"])
    _region(ax, COL_X[0]+0.1, CY+0.1, NW-0.2, 0.5, C["white"],
            "KV Buf", "key=hello val=world")
    _region(ax, COL_X[0]+0.1, CY+0.7, NW-0.2, 0.5, C["idx"],
            "Root Cache")

    # Node B above MN1
    if show_b:
        _box(ax, COL_X[1], CY, NW, NH_C, C["compb_l"], C["compb"])
        _lbl(ax, COL_X[1]+NW/2, CY+NH_C-0.2, "Compute Node B", 10, C["compb"])
        _region(ax, COL_X[1]+0.1, CY+0.1, NW-0.2, 0.5, C["white"],
                "KV Buf", "key=hello val=xyz")

    # ── Memory nodes (bottom row) ──
    mn_vals = [
        (COL_X[0], "MN 0 (Primary)", mn0_idx, mn0_kv, mn0_log, hi_idx[0], hi_kv[0], hi_log[0]),
        (COL_X[1], "MN 1 (Backup)",  mn1_idx, mn1_kv, mn1_log, hi_idx[1], hi_kv[1], hi_log[1]),
        (COL_X[2], "MN 2 (Backup)",  mn2_idx, mn2_kv, mn2_log, hi_idx[2], hi_kv[2], hi_log[2]),
    ]
    for mx, t, iv, kv, lv, h_i, h_k, h_l in mn_vals:
        _box(ax, mx, MY, NW, NH_M, C["mem_l"], C["mem"])
        _lbl(ax, mx+NW/2, MY+NH_M-0.2, t, 10, C["mem"])
        _region(ax, mx+0.1, MY+1.6, 1.8, 0.7, C["idx"], "Index", iv, hi=h_i)
        _region(ax, mx+2.0, MY+1.6, 1.8, 0.7, C["data"], "KV Data", kv, hi=h_k)
        _region(ax, mx+0.1, MY+0.1, 3.7, 0.55, C["log"], "Log", lv, hi=h_l)

    # legend bar
    ax.legend(handles=[
        mpatches.Patch(fc=C["comp_l"], ec=C["comp"], lw=1.5, label="Compute"),
        mpatches.Patch(fc=C["mem_l"], ec=C["mem"], lw=1.5, label="Memory"),
        mpatches.Patch(fc=C["idx"], ec=C["gray"], lw=0.8, label="Index"),
        mpatches.Patch(fc=C["data"], ec=C["gray"], lw=0.8, label="KV Data"),
        mpatches.Patch(fc=C["log"], ec=C["gray"], lw=0.8, label="Log"),
        mpatches.Patch(fc="white", ec=C["hi"], lw=2, label="Modified"),
    ], loc="lower center", fontsize=7.5, framealpha=0.9, ncol=6,
       bbox_to_anchor=(0.5, -0.02))


def save(fig, name):
    fig.tight_layout()
    fig.savefig(f"docs/{name}.png", dpi=200, bbox_inches="tight")
    fig.savefig(f"docs/{name}.pdf", bbox_inches="tight")
    print(f"  Saved {name}")
    plt.close(fig)


# Helpers for arrow endpoints
def _comp_bot(col):
    return COL_X[col] + NW/2, CY

def _mn_top(col):
    return COL_X[col] + NW/2, MY + NH_M

def _mn_top_idx(col):
    return COL_X[col] + 1.0, MY + NH_M

def _mn_top_kv(col):
    return COL_X[col] + 2.9, MY + NH_M

def _mn_top_log(col):
    return COL_X[col] + NW/2, MY + NH_M


# ================================================================
def main():
    print("Generating RDMA consensus step-by-step PNGs...")

    # ── Step 1 ──
    fig, ax = plt.subplots(figsize=(15, 9))
    _draw_base(ax, 1, "RDMA WRITE KV data to all replicas",
               mn0_kv="hello:world", mn1_kv="hello:world", mn2_kv="hello:world",
               hi_kv=(True, True, True))
    for col in [0, 1, 2]:
        _arr(ax, *_comp_bot(0), *_mn_top_kv(col), C["comp"], 2.2)
    _lbl(ax, 7.5, 4.6, "RDMA WRITE (KV data)", 10, C["comp"])
    save(fig, "rdma_step1")

    # ── Step 2 ──
    fig, ax = plt.subplots(figsize=(15, 9))
    _draw_base(ax, 2, "RDMA READ hash bucket from Primary Index",
               mn0_kv="hello:world", mn1_kv="hello:world", mn2_kv="hello:world")
    _arr(ax, *_comp_bot(0), *_mn_top_idx(0), C["comp"], 2.2)
    _arr(ax, _mn_top_idx(0)[0]+0.3, _mn_top_idx(0)[1],
         _comp_bot(0)[0]+0.3, _comp_bot(0)[1], C["comp"], 1.5, 0, dash=True)
    _lbl(ax, 3.5, 4.6, "RDMA READ", 10, C["comp"])
    _lbl(ax, 3.5, 4.2, "A learns: Slot X = 0x0", 9, C["ok"])
    save(fig, "rdma_step2")

    # ── Step 3 ──
    fig, ax = plt.subplots(figsize=(15, 9))
    _draw_base(ax, 3, "RDMA CAS Backup Index  (A wins, B loses)",
               mn0_kv="hello:world", mn1_kv="hello:world", mn2_kv="hello:world",
               mn1_idx="Slot X = A", mn2_idx="Slot X = A",
               hi_idx=(False, True, True))
    # A -> backup 1, 2
    _arr(ax, _comp_bot(0)[0]+0.5, CY, _mn_top_idx(1)[0], MY+NH_M, C["comp"], 2.5)
    _arr(ax, _comp_bot(0)[0]+0.8, CY, _mn_top_idx(2)[0], MY+NH_M, C["comp"], 2.5)
    _lbl(ax, 5.0, 4.8, "A: CAS(Slot, 0x0->A)", 9, C["comp"])
    # B -> backup 1, 2
    _arr(ax, _comp_bot(1)[0]-0.5, CY, _mn_top_idx(1)[0]+0.5, MY+NH_M, C["compb"], 1.8)
    _arr(ax, _comp_bot(1)[0]+0.5, CY, _mn_top_idx(2)[0]+0.5, MY+NH_M, C["compb"], 1.8)
    _lbl(ax, 10.0, 4.8, "B: CAS(Slot, 0x0->B)", 9, C["compb"])
    # results
    _lbl(ax, COL_X[1]+NW/2, MY+1.25, "A: ret=0x0 (win)", 8.5, C["ok"])
    _lbl(ax, COL_X[1]+NW/2, MY+0.95, "B: ret=A (lose)", 8.5, C["fail"])
    save(fig, "rdma_step3")

    # ── Step 4 ──
    fig, ax = plt.subplots(figsize=(15, 9))
    _draw_base(ax, 4, "Check CAS results (local computation)", title_color=C["dg"],
               mn0_kv="hello:world", mn1_kv="hello:world", mn2_kv="hello:world",
               mn1_idx="Slot X = A", mn2_idx="Slot X = A")
    # A result box
    ax.add_patch(FancyBboxPatch((COL_X[0]+0.3, CY+NH_C+0.15), 3.4, 0.6,
                 boxstyle="round,pad=0.1", fc="#E8F5E9", ec=C["ok"], lw=2, zorder=6))
    _lbl(ax, COL_X[0]+2.0, CY+NH_C+0.45, "A: all backups agreed -> WIN_ALL", 9, C["ok"])
    # B result box
    ax.add_patch(FancyBboxPatch((COL_X[1]+0.3, CY+NH_C+0.15), 3.4, 0.6,
                 boxstyle="round,pad=0.1", fc="#FFEBEE", ec=C["fail"], lw=2, zorder=6))
    _lbl(ax, COL_X[1]+2.0, CY+NH_C+0.45, "B: CAS returned A -> FAIL (abort)", 9, C["fail"])
    save(fig, "rdma_step4")

    # ── Step 5 ──
    fig, ax = plt.subplots(figsize=(15, 9))
    _draw_base(ax, 5, "RDMA WRITE commit log to all replicas",
               mn0_kv="hello:world", mn1_kv="hello:world", mn2_kv="hello:world",
               mn1_idx="Slot X = A", mn2_idx="Slot X = A",
               mn0_log="old_value = orig", mn1_log="old_value = orig", mn2_log="old_value = orig",
               hi_log=(True, True, True), show_b=False)
    for col in [0, 1, 2]:
        _arr(ax, *_comp_bot(0), *_mn_top_log(col), C["comp"], 2.2)
    _lbl(ax, 7.5, 4.6, "RDMA WRITE (commit mark to Log)", 10, C["comp"])
    _lbl(ax, 7.5, 4.2, "KVLogTail.old_value := original slot value", 8, C["comp"], bold=False)
    save(fig, "rdma_step5")

    # ── Step 6 ──
    fig, ax = plt.subplots(figsize=(15, 9))
    _draw_base(ax, 6, "RDMA CAS Primary Index  ==  COMMIT POINT", title_color=C["commit"],
               mn0_kv="hello:world", mn1_kv="hello:world", mn2_kv="hello:world",
               mn0_idx="Slot X = A", mn1_idx="Slot X = A", mn2_idx="Slot X = A",
               mn0_log="old_value = orig", mn1_log="old_value = orig", mn2_log="old_value = orig",
               hi_idx=(True, False, False), show_b=False)
    _arr(ax, *_comp_bot(0), *_mn_top_idx(0), C["commit"], 3)
    _arr(ax, _mn_top_idx(0)[0]+0.3, MY+NH_M,
         _comp_bot(0)[0]+0.3, CY, C["commit"], 1.5, 0, dash=True)
    _lbl(ax, 3.5, 4.6, "CAS(Slot, 0x0 -> A)", 10, C["commit"])
    _lbl(ax, 3.5, 4.2, "ret = 0x0 (success)", 9, C["ok"])
    # commit line
    ax.axhline(y=4.9, xmin=0.03, xmax=0.97, ls="-", lw=3.5,
               color=C["commit"], alpha=0.35, zorder=1)
    _lbl(ax, 12.0, 5.15, "COMMIT POINT", 11, C["commit"])
    # final state
    ax.add_patch(FancyBboxPatch((3.5, MY-0.15), 11.5, 0.5,
                 boxstyle="round,pad=0.1", fc="#E8F5E9", ec=C["ok"], lw=1.5, zorder=8))
    _lbl(ax, 9.2, MY+0.1,
         "Final: Primary Idx = A  |  Backup Idx = A  |  KV on all MNs  |  Log committed",
         9, C["ok"])
    save(fig, "rdma_step6")

    print("Done. 6 PNGs saved to docs/rdma_step[1-6].png")


if __name__ == "__main__":
    main()
