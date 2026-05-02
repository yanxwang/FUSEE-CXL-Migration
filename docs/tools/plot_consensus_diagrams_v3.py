#!/usr/bin/env python3
"""
Step-by-step animation-style diagrams for FUSEE RDMA consensus.
Each step is one subplot showing exactly what changes in that step.
"""

import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch

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
    "gray": "#9E9E9E", "lgray": "#F5F5F5", "dg": "#333333",
    "white": "#FFFFFF", "commit": "#FF6F00", "ok": "#1B5E20",
    "fail": "#B71C1C", "hi": "#FF8F00",  # highlight color
}

W, H = 15, 6.2  # canvas per panel
# positions
CA_X, CA_Y, CA_W, CA_H = 0.3, 4.0, 3.2, 1.8
CB_X, CB_Y, CB_W, CB_H = 0.3, 1.8, 3.2, 1.5
MN_Y, MN_W, MN_H = 0.3, 3.5, 3.5
MN0_X, MN1_X, MN2_X = 4.5, 8.2, 11.9


def box(ax, x, y, w, h, fc, ec="white", lw=1.5):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.08",
                 facecolor=fc, edgecolor=ec, lw=lw, zorder=3))

def region(ax, x, y, w, h, fc, title, sub="", highlight=False):
    ec = C["hi"] if highlight else C["gray"]
    lw = 2.0 if highlight else 0.7
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.04",
                 facecolor=fc, edgecolor=ec, lw=lw, zorder=4))
    if sub:
        ax.text(x+w/2, y+h/2+0.1, title, ha="center", va="center",
                fontsize=7, color=C["dg"], weight="bold", zorder=5)
        ax.text(x+w/2, y+h/2-0.1, sub, ha="center", va="center",
                fontsize=6.5, color=C["gray"] if not highlight else C["ok"],
                family="monospace", zorder=5,
                weight="bold" if highlight else "normal")
    else:
        ax.text(x+w/2, y+h/2, title, ha="center", va="center",
                fontsize=7, color=C["dg"], weight="bold", zorder=5)

def lbl(ax, x, y, t, fs=9, c="black", bold=True, ha="center"):
    ax.text(x, y, t, ha=ha, va="center", fontsize=fs, color=c,
            weight="bold" if bold else "normal", zorder=6)

def arr(ax, x0, y0, x1, y1, c="black", lw=1.8, rad=0, dash=False):
    ax.add_patch(FancyArrowPatch((x0, y0), (x1, y1), arrowstyle="->,head_length=0.3,head_width=0.18",
                 connectionstyle=f"arc3,rad={rad}", color=c, lw=lw,
                 linestyle="--" if dash else "-", mutation_scale=12, zorder=7))

def step_num(ax, x, y, n, bg=None):
    bg = bg or C["comp"]
    ax.add_patch(plt.Circle((x, y), 0.25, fc=bg, ec="white", lw=2, zorder=9))
    ax.text(x, y, str(n), ha="center", va="center", fontsize=10,
            color="white", weight="bold", zorder=10)


def draw_scene(ax, step,
               mn0_idx="Slot X = 0x0", mn1_idx="Slot X = 0x0", mn2_idx="Slot X = 0x0",
               mn0_kv="", mn1_kv="", mn2_kv="",
               mn0_log="old_value = 0", mn1_log="old_value = 0", mn2_log="old_value = 0",
               hi_mn0_idx=False, hi_mn1_idx=False, hi_mn2_idx=False,
               hi_mn0_kv=False, hi_mn1_kv=False, hi_mn2_kv=False,
               hi_mn0_log=False, hi_mn1_log=False, hi_mn2_log=False):
    """Draw the base scene (compute top, memory bottom) with given state."""
    ax.set_xlim(-0.3, W)
    ax.set_ylim(-0.3, H)
    ax.set_aspect("equal")
    ax.axis("off")

    # ── Compute Node A ──
    box(ax, CA_X, CA_Y, CA_W, CA_H, C["comp_l"], C["comp"], 2)
    lbl(ax, CA_X+CA_W/2, CA_Y+CA_H-0.2, "Compute Node A", 9, C["comp"])
    region(ax, CA_X+0.1, CA_Y+0.7, CA_W-0.2, 0.5, C["idx"], "Root Cache", "subtable_entry[]")
    region(ax, CA_X+0.1, CA_Y+0.1, CA_W-0.2, 0.45, C["white"], "KV Buf", "key=hello val=world")

    # ── Compute Node B ──
    box(ax, CB_X, CB_Y, CB_W, CB_H, C["compb_l"], C["compb"], 2)
    lbl(ax, CB_X+CB_W/2, CB_Y+CB_H-0.2, "Compute Node B", 9, C["compb"])
    region(ax, CB_X+0.1, CB_Y+0.1, CB_W-0.2, 0.45, C["white"], "KV Buf", "key=hello val=xyz")

    # ── Memory Nodes ──
    for mx, title, idx_v, kv_v, log_v, hi_i, hi_k, hi_l in [
        (MN0_X, "MN 0 (Primary)", mn0_idx, mn0_kv, mn0_log, hi_mn0_idx, hi_mn0_kv, hi_mn0_log),
        (MN1_X, "MN 1 (Backup)",  mn1_idx, mn1_kv, mn1_log, hi_mn1_idx, hi_mn1_kv, hi_mn1_log),
        (MN2_X, "MN 2 (Backup)",  mn2_idx, mn2_kv, mn2_log, hi_mn2_idx, hi_mn2_kv, hi_mn2_log),
    ]:
        box(ax, mx, MN_Y, MN_W, MN_H, C["mem_l"], C["mem"], 2)
        lbl(ax, mx+MN_W/2, MN_Y+MN_H-0.2, title, 9, C["mem"])
        region(ax, mx+0.1, MN_Y+2.1, 1.55, 0.7, C["idx"], "Index", idx_v, highlight=hi_i)
        region(ax, mx+1.8, MN_Y+2.1, 1.55, 0.7, C["data"],
               "KV Data", kv_v if kv_v else "(empty)", highlight=hi_k)
        region(ax, mx+0.1, MN_Y+0.1, 3.25, 0.6, C["log"], "Log", log_v, highlight=hi_l)

    # step number badge
    step_num(ax, 0.3, H-0.3, step)


# ================================================================
#  Build all 6 steps
# ================================================================
def draw_all():
    fig, axes = plt.subplots(3, 2, figsize=(16, 17))
    fig.suptitle("FUSEE: RDMA CAS-Based Consensus (INSERT, step by step)",
                 fontsize=15, fontweight="bold", y=0.995)

    # ── Step 1: WRITE KV data to all replicas ──
    ax = axes[0][0]
    draw_scene(ax, 1, hi_mn0_kv=True, hi_mn1_kv=True, hi_mn2_kv=True,
               mn0_kv="hello:world", mn1_kv="hello:world", mn2_kv="hello:world")
    ax.set_title("Step 1: RDMA WRITE KV data to all replicas", fontsize=10,
                 fontweight="bold", color=C["comp"], pad=4)
    arr(ax, CA_X+CA_W, CA_Y+0.3, MN0_X+2.6, MN_Y+MN_H, C["comp"], 2, 0.08)
    arr(ax, CA_X+CA_W, CA_Y+0.2, MN1_X+2.6, MN_Y+MN_H, C["comp"], 2, 0.03)
    arr(ax, CA_X+CA_W, CA_Y+0.1, MN2_X+2.6, MN_Y+MN_H, C["comp"], 2, 0)
    lbl(ax, 7.0, 4.7, "RDMA WRITE (KV data)", 8, C["comp"])

    # ── Step 2: READ bucket from Primary ──
    ax = axes[0][1]
    draw_scene(ax, 2, mn0_kv="hello:world", mn1_kv="hello:world", mn2_kv="hello:world")
    ax.set_title("Step 2: RDMA READ hash bucket from Primary Index", fontsize=10,
                 fontweight="bold", color=C["comp"], pad=4)
    arr(ax, CA_X+CA_W, CA_Y+1.0, MN0_X+0.9, MN_Y+MN_H, C["comp"], 2, 0.1)
    arr(ax, MN0_X+0.9, MN_Y+MN_H, CA_X+CA_W, CA_Y+0.8, C["comp"], 1.5, 0.1, dash=True)
    lbl(ax, 4.3, 4.7, "RDMA READ", 8, C["comp"])
    lbl(ax, CA_X+CA_W+0.3, CA_Y+0.9, "-> Slot X = 0x0", 7.5, C["ok"], ha="left")

    # ── Step 3: CAS Backup Index (A wins, B loses) ──
    ax = axes[1][0]
    draw_scene(ax, 3, mn0_kv="hello:world", mn1_kv="hello:world", mn2_kv="hello:world",
               mn1_idx="Slot X = A", mn2_idx="Slot X = A",
               hi_mn1_idx=True, hi_mn2_idx=True)
    ax.set_title("Step 3: RDMA CAS Backup Index  (A wins, B loses)", fontsize=10,
                 fontweight="bold", color=C["comp"], pad=4)
    # A's CAS
    arr(ax, CA_X+CA_W, CA_Y+0.8, MN1_X+0.9, MN_Y+MN_H, C["comp"], 2.2, 0.02)
    arr(ax, CA_X+CA_W, CA_Y+0.6, MN2_X+0.9, MN_Y+MN_H, C["comp"], 2.2, 0)
    lbl(ax, 5.5, 5.0, "A: CAS(Slot, 0x0->A)", 8, C["comp"])
    # B's CAS
    arr(ax, CB_X+CB_W, CB_Y+1.0, MN1_X+0.9, MN_Y+2.9, C["compb"], 1.5, -0.08)
    arr(ax, CB_X+CB_W, CB_Y+0.8, MN2_X+0.9, MN_Y+2.9, C["compb"], 1.5, -0.05)
    lbl(ax, 5.5, 2.3, "B: CAS(Slot, 0x0->B)", 8, C["compb"])
    # results
    lbl(ax, MN1_X+MN_W/2, MN_Y+1.6, "A: ret=0x0 (win)", 7.5, C["ok"])
    lbl(ax, MN1_X+MN_W/2, MN_Y+1.3, "B: ret=A  (lose)", 7.5, C["fail"])

    # ── Step 4: Check consensus (local) ──
    ax = axes[1][1]
    draw_scene(ax, 4, mn0_kv="hello:world", mn1_kv="hello:world", mn2_kv="hello:world",
               mn1_idx="Slot X = A", mn2_idx="Slot X = A")
    ax.set_title("Step 4: Check CAS results (local computation)", fontsize=10,
                 fontweight="bold", color=C["dg"], pad=4)
    # speech bubbles
    lbl(ax, CA_X+CA_W+0.3, CA_Y+CA_H/2, "All backups\nreturned 0x0\n-> WIN_ALL", 8.5, C["ok"], ha="left")
    ax.add_patch(FancyBboxPatch((CA_X+CA_W+0.1, CA_Y+0.2), 2.0, 1.2,
                 boxstyle="round,pad=0.1", fc="#E8F5E9", ec=C["ok"], lw=1.2, zorder=3))
    lbl(ax, CA_X+CA_W+1.1, CA_Y+0.8, "A: WIN_ALL", 9, C["ok"])

    ax.add_patch(FancyBboxPatch((CB_X+CB_W+0.1, CB_Y+0.1), 2.0, 0.8,
                 boxstyle="round,pad=0.1", fc="#FFEBEE", ec=C["fail"], lw=1.2, zorder=3))
    lbl(ax, CB_X+CB_W+1.1, CB_Y+0.5, "B: FAIL\n(aborts)", 9, C["fail"])

    # ── Step 5: WRITE commit log ──
    ax = axes[2][0]
    draw_scene(ax, 5, mn0_kv="hello:world", mn1_kv="hello:world", mn2_kv="hello:world",
               mn1_idx="Slot X = A", mn2_idx="Slot X = A",
               mn0_log="old_value = 0x..A", mn1_log="old_value = 0x..A", mn2_log="old_value = 0x..A",
               hi_mn0_log=True, hi_mn1_log=True, hi_mn2_log=True)
    ax.set_title("Step 5: RDMA WRITE commit log (all replicas)", fontsize=10,
                 fontweight="bold", color=C["comp"], pad=4)
    arr(ax, CA_X+CA_W, CA_Y+0.3, MN0_X+1.7, MN_Y+MN_H, C["comp"], 2, 0.08)
    arr(ax, CA_X+CA_W, CA_Y+0.2, MN1_X+1.7, MN_Y+MN_H, C["comp"], 2, 0.04)
    arr(ax, CA_X+CA_W, CA_Y+0.1, MN2_X+1.7, MN_Y+MN_H, C["comp"], 2, 0)
    lbl(ax, 7.0, 4.7, "RDMA WRITE (log commit)", 8, C["comp"])

    # ── Step 6: CAS Primary (commit point) ──
    ax = axes[2][1]
    draw_scene(ax, 6, mn0_kv="hello:world", mn1_kv="hello:world", mn2_kv="hello:world",
               mn0_idx="Slot X = A", mn1_idx="Slot X = A", mn2_idx="Slot X = A",
               mn0_log="old_value = 0x..A", mn1_log="old_value = 0x..A", mn2_log="old_value = 0x..A",
               hi_mn0_idx=True)
    ax.set_title("Step 6: RDMA CAS Primary Index  == COMMIT POINT", fontsize=10,
                 fontweight="bold", color=C["commit"], pad=4)
    arr(ax, CA_X+CA_W, CA_Y+1.0, MN0_X+0.9, MN_Y+MN_H, C["commit"], 2.5, 0.1)
    arr(ax, MN0_X+0.9, MN_Y+MN_H, CA_X+CA_W, CA_Y+0.8, C["commit"], 1.5, 0.1, dash=True)
    lbl(ax, 4.0, 4.9, "CAS(Slot, 0x0->A)", 8.5, C["commit"])
    lbl(ax, 4.0, 4.55, "ret=0x0 (success)", 8, C["ok"])

    # commit point line
    ax.axhline(y=3.9, xmin=0.25, xmax=0.98, ls="-", lw=3,
               color=C["commit"], alpha=0.4, zorder=1)
    lbl(ax, 10.0, 3.7, "COMMIT POINT", 10, C["commit"])

    # final state box
    ax.add_patch(FancyBboxPatch((4.5, 0.05), 10.5, 0.55,
                 boxstyle="round,pad=0.1", fc="#E8F5E9", ec=C["ok"], lw=1.5, zorder=8))
    lbl(ax, 9.7, 0.32,
        "Final: Primary Idx=A | Backup Idx=A | KV on all MNs | Log committed",
        8.5, C["ok"])

    # ── Global legend ──
    fig.legend(handles=[
        mpatches.Patch(fc=C["comp_l"], ec=C["comp"], lw=1.5, label="Compute Node"),
        mpatches.Patch(fc=C["mem_l"], ec=C["mem"], lw=1.5, label="Memory Node"),
        mpatches.Patch(fc=C["idx"], ec=C["gray"], lw=0.8, label="Index"),
        mpatches.Patch(fc=C["data"], ec=C["gray"], lw=0.8, label="KV Data"),
        mpatches.Patch(fc=C["log"], ec=C["gray"], lw=0.8, label="Log"),
        mpatches.Patch(fc=C["hi"], ec=C["hi"], lw=1.5, label="Modified this step"),
    ], loc="lower center", fontsize=8.5, framealpha=0.9, ncol=6,
       bbox_to_anchor=(0.5, -0.01))

    fig.tight_layout(rect=[0, 0.03, 1, 0.97], h_pad=1.5)
    fig.savefig("docs/fig1_rdma_consensus.png", dpi=200, bbox_inches="tight")
    fig.savefig("docs/fig1_rdma_consensus.pdf", bbox_inches="tight")
    print("Saved fig1_rdma_consensus")


if __name__ == "__main__":
    draw_all()
    plt.show()
