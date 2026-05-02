#!/usr/bin/env python3
"""
Research-paper-style sequence diagrams for FUSEE consensus protocols.
Fig 1: RDMA CAS-based consensus (original FUSEE)
Fig 2: CXL software-based Propose-Vote-Commit consensus
"""

import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch
import matplotlib.lines as mlines

# ── Global style (paper-like) ──────────────────────────────
plt.rcParams.update({
    "font.family": "serif",
    "font.serif": ["Times New Roman", "DejaVu Serif"],
    "font.size": 10,
    "mathtext.fontset": "dejavuserif",
    "figure.facecolor": "white",
    "axes.facecolor": "white",
    "axes.linewidth": 0,
})

# Muted palette (paper-friendly, prints well in B&W)
PAL = {
    "blue":   "#3B6EA5",
    "red":    "#C0392B",
    "green":  "#27864E",
    "orange": "#D4841C",
    "purple": "#6C4EA1",
    "gray":   "#888888",
    "lgray":  "#D9D9D9",
    "bg":     "#F5F5F0",
    "black":  "#222222",
}


# ── Helpers ─────────────────────────────────────────────────
def _lifeline(ax, x, y_top, y_bot, **kw):
    """Dashed vertical lifeline."""
    ax.plot([x, x], [y_top, y_bot], ls="--", lw=0.8, color=PAL["gray"], zorder=0)


def _msg(ax, x0, x1, y, label, color=PAL["black"], dashed=False, fontsize=8.5,
         label_side="above", bold=False):
    """Horizontal arrow with label (message)."""
    style = "->" if x1 > x0 else "<-"
    ls = "--" if dashed else "-"
    ax.annotate("", xy=(x1, y), xytext=(x0, y),
                arrowprops=dict(arrowstyle="->", color=color, lw=1.4,
                                linestyle=ls, shrinkA=0, shrinkB=0))
    offset = 0.18 if label_side == "above" else -0.22
    weight = "bold" if bold else "normal"
    ax.text((x0 + x1) / 2, y + offset, label, ha="center", va="center",
            fontsize=fontsize, color=color, weight=weight, zorder=5)


def _self_msg(ax, x, y, label, color=PAL["black"], fontsize=8.5, w=0.6):
    """Self-loop arrow (local computation)."""
    ax.annotate("", xy=(x + w, y - 0.25), xytext=(x + w, y + 0.05),
                arrowprops=dict(arrowstyle="->", color=color, lw=1.2,
                                connectionstyle="arc3,rad=-0.5"))
    ax.text(x + w + 0.15, y - 0.1, label, fontsize=fontsize, va="center",
            ha="left", color=color)


def _entity(ax, x, y, label, color=PAL["blue"], w=1.8, h=0.55):
    """Entity box at top of lifeline."""
    box = FancyBboxPatch((x - w / 2, y), w, h, boxstyle="round,pad=0.08",
                         facecolor=color, edgecolor="white", lw=0, alpha=0.88, zorder=3)
    ax.add_patch(box)
    ax.text(x, y + h / 2, label, ha="center", va="center",
            fontsize=9.5, color="white", weight="bold", zorder=4)


def _note(ax, x, y, text, w=3.2, h=0.45, fontsize=8, color=PAL["black"], bg="#FFFDE7"):
    """Annotation note box."""
    box = FancyBboxPatch((x - w / 2, y - h / 2), w, h, boxstyle="round,pad=0.12",
                         facecolor=bg, edgecolor=PAL["gray"], lw=0.8, zorder=3)
    ax.add_patch(box)
    ax.text(x, y, text, ha="center", va="center", fontsize=fontsize, color=color, zorder=4)


def _phase_bg(ax, y_top, y_bot, x_left, x_right, label, idx):
    """Light phase background band with label on the right margin."""
    if idx % 2 == 0:
        ax.fill_between([x_left, x_right], y_bot, y_top,
                        color=PAL["lgray"], alpha=0.25, zorder=0)
    # circled number
    cx = x_right - 0.15
    cy = (y_top + y_bot) / 2
    ax.add_patch(plt.Circle((cx, cy), 0.22, facecolor=PAL["blue"], edgecolor="none", zorder=3, alpha=0.8))
    ax.text(cx, cy, str(idx + 1), ha="center", va="center",
            fontsize=8, color="white", weight="bold", zorder=4)
    ax.text(cx - 0.35, cy, label, ha="right", va="center",
            fontsize=8, color=PAL["gray"], style="italic", zorder=4)


# ================================================================
#  FIGURE 1 — RDMA CAS-Based Consensus
# ================================================================
def draw_fig1():
    fig, ax = plt.subplots(figsize=(8.5, 8))
    ax.set_xlim(-0.5, 10.5)
    ax.set_ylim(-0.8, 9.5)
    ax.axis("off")
    ax.set_title("(a) FUSEE RDMA CAS-Based Consensus (INSERT)",
                 fontsize=12, fontweight="bold", pad=8, loc="center")

    # ── Entities ──
    # x positions for lifelines
    CA, CB, PR, BK = 1.0, 3.2, 5.8, 8.5
    top_y = 8.6
    bot_y = -0.2

    _entity(ax, CA, top_y, "Client A", PAL["blue"])
    _entity(ax, CB, top_y, "Client B", PAL["red"])
    _entity(ax, PR, top_y, "Primary\n(MN 0)", PAL["green"], h=0.65)
    _entity(ax, BK, top_y, "Backup\n(MN 1)", PAL["green"], h=0.65)

    _lifeline(ax, CA, top_y, bot_y)
    _lifeline(ax, CB, top_y, bot_y)
    _lifeline(ax, PR, top_y, bot_y)
    _lifeline(ax, BK, top_y, bot_y)

    # ── Phase bands ──
    phases = [
        (8.3, 7.1, "Write KV\n+ Read Idx"),
        (7.1, 5.4, "CAS\nBackup"),
        (5.4, 4.2, "Check\nResult"),
        (4.2, 3.1, "Commit\nLog"),
        (3.1, 1.6, "CAS\nPrimary"),
    ]
    for i, (yt, yb, lbl) in enumerate(phases):
        _phase_bg(ax, yt, yb, -0.4, 10.2, lbl, i)

    # ── Step 1: Write KV + Read Bucket ──
    y = 7.9
    _msg(ax, CA, PR, y, "RDMA WRITE (KV data)", PAL["blue"])
    y -= 0.5
    _msg(ax, CA, BK, y, "RDMA WRITE (KV data)", PAL["blue"])
    y -= 0.45
    _msg(ax, CA, PR, y, "RDMA READ (hash bucket)", PAL["blue"], dashed=True)

    # ── Step 2: CAS Backup ──
    y = 6.35
    _msg(ax, CA, BK, y, "CAS(Slot, 0x0 → A)", PAL["blue"], bold=True)
    y -= 0.6
    _msg(ax, CB, BK, y, "CAS(Slot, 0x0 → B)", PAL["red"], bold=True)

    # return arrows
    _msg(ax, BK, CA, y + 0.6 - 0.2, "", PAL["blue"], dashed=True)
    ax.text(BK + 0.2, y + 0.6 - 0.08, "ret = 0x0 (win)", fontsize=7.5, color=PAL["green"],
            weight="bold")
    _msg(ax, BK, CB, y - 0.2, "", PAL["red"], dashed=True)
    ax.text(BK + 0.2, y - 0.08, "ret = A (lose)", fontsize=7.5, color=PAL["red"], weight="bold")

    # ── Step 3: Check ──
    y = 4.9
    _self_msg(ax, CA - 0.3, y, "votes → WIN_ALL", PAL["blue"])
    _self_msg(ax, CB - 0.3, y - 0.5, "votes → FAIL (abort)", PAL["red"])

    # ── Step 4: Commit Log ──
    y = 3.7
    _msg(ax, CA, PR, y, "RDMA WRITE (log commit)", PAL["blue"])
    y -= 0.25
    _msg(ax, CA, BK, y, "RDMA WRITE (log commit)", PAL["blue"])

    # ── Step 5: CAS Primary ──
    y = 2.7
    _msg(ax, CA, PR, y, "CAS(Slot, 0x0 → A)", PAL["blue"], bold=True)
    _msg(ax, PR, CA, y - 0.35, "", PAL["blue"], dashed=True)
    ax.text(PR + 0.2, y - 0.22, "ret = 0x0 (win)", fontsize=7.5, color=PAL["green"], weight="bold")

    # commit point marker
    y_cp = 2.0
    ax.plot([0.0, 9.8], [y_cp, y_cp], ls="-", lw=1.8, color=PAL["orange"], zorder=1)
    ax.text(5.0, y_cp - 0.25, "── commit point ──", ha="center", fontsize=9,
            color=PAL["orange"], weight="bold")

    # bottom note
    _note(ax, 5.0, 0.4,
          "Atomicity guarantee: RDMA NIC ensures at most one\n"
          "CAS succeeds per address. Loser sees winner's value.",
          w=6.5, h=0.65, fontsize=8.5, bg="#FFF8E1")

    # legend
    h_a = mlines.Line2D([], [], color=PAL["blue"], lw=2, label="Client A")
    h_b = mlines.Line2D([], [], color=PAL["red"], lw=2, label="Client B")
    h_solid = mlines.Line2D([], [], color="black", lw=1.2, label="Request")
    h_dash = mlines.Line2D([], [], color="black", lw=1.2, ls="--", label="Response")
    ax.legend(handles=[h_a, h_b, h_solid, h_dash], loc="lower left",
              fontsize=7.5, frameon=True, fancybox=True, framealpha=0.9,
              ncol=4, bbox_to_anchor=(-0.3, -0.1))

    fig.tight_layout()
    fig.savefig("docs/fig1_rdma_consensus.png", dpi=200, bbox_inches="tight")
    fig.savefig("docs/fig1_rdma_consensus.pdf", bbox_inches="tight")
    print("Saved fig1")
    return fig


# ================================================================
#  FIGURE 2 — CXL Software Consensus
# ================================================================
def draw_fig2():
    fig, ax = plt.subplots(figsize=(8.5, 9.5))
    ax.set_xlim(-0.5, 10.5)
    ax.set_ylim(-1.2, 10.5)
    ax.axis("off")
    ax.set_title("(b) CXL-FUSEE Software Propose-Vote-Commit Consensus (INSERT)",
                 fontsize=12, fontweight="bold", pad=8, loc="center")

    # ── Entities ──
    N0, N1, N2, CXL = 1.0, 3.3, 5.6, 8.5
    top_y = 9.6
    bot_y = -0.4

    _entity(ax, N0, top_y, "Node 0", PAL["blue"])
    _entity(ax, N1, top_y, "Node 1", PAL["purple"])
    _entity(ax, N2, top_y, "Node 2", PAL["purple"])
    _entity(ax, CXL, top_y, "CXL Mem", PAL["orange"], w=1.6)

    _lifeline(ax, N0, top_y, bot_y)
    _lifeline(ax, N1, top_y, bot_y)
    _lifeline(ax, N2, top_y, bot_y)
    _lifeline(ax, CXL, top_y, bot_y)

    # ── Phase bands ──
    phases = [
        (9.3, 8.0,  "Claim"),
        (8.0, 6.4,  "Propose"),
        (6.4, 4.0,  "Vote"),
        (4.0, 2.3,  "Commit"),
        (2.3, 0.3,  "Apply"),
    ]
    for i, (yt, yb, lbl) in enumerate(phases):
        _phase_bg(ax, yt, yb, -0.4, 10.2, lbl, i)

    # ── Phase 0: Claim ──
    y = 8.85
    _msg(ax, N0, CXL, y, "store(proposer=0, epoch=42)", PAL["blue"])
    y -= 0.35
    ax.text(N0 + 0.1, y + 0.05, "sfence", fontsize=7, color=PAL["gray"], style="italic")
    y -= 0.25
    _msg(ax, CXL, N0, y, "load: proposer still 0 (ok)", PAL["blue"], dashed=True)

    # ── Phase 1: Propose ──
    y = 7.55
    _msg(ax, N0, CXL, y,
         "store(old=0x0, new=A,\n status=PROPOSED, vote[0]=ACK)",
         PAL["blue"], fontsize=8)
    y -= 0.55
    ax.text(N0 + 0.1, y + 0.15, "sfence", fontsize=7, color=PAL["gray"], style="italic")

    # show the CXL log entry
    _note(ax, CXL, y - 0.15,
          "PROPOSED\nvote=[ACK, ?, ?]",
          w=1.8, h=0.55, fontsize=7.5, bg="#FFF3E0", color=PAL["orange"])

    # ── Phase 2: Vote ──
    # Node 1 reads CXL, checks local, writes vote
    y = 5.9
    _msg(ax, CXL, N1, y, "poll: see PROPOSED", PAL["purple"], dashed=True, fontsize=8)
    y -= 0.4
    _self_msg(ax, N1 - 0.3, y, "local Slot==0x0 (ok)", PAL["purple"], fontsize=7.5, w=0.5)
    y -= 0.45
    _msg(ax, N1, CXL, y, "store vote[1]=ACK", PAL["purple"], fontsize=8)

    # Node 2 reads CXL, checks local, writes vote
    y -= 0.45
    _msg(ax, CXL, N2, y, "poll: see PROPOSED", PAL["purple"], dashed=True, fontsize=8)
    y -= 0.4
    _self_msg(ax, N2 - 0.3, y, "local Slot==0x0 (ok)", PAL["purple"], fontsize=7.5, w=0.5)
    y -= 0.45
    _msg(ax, N2, CXL, y, "store vote[2]=ACK", PAL["purple"], fontsize=8)

    # updated note
    _note(ax, CXL, y - 0.15,
          "PROPOSED\nvote=[ACK,ACK,ACK]",
          w=1.8, h=0.55, fontsize=7.5, bg="#E8F5E9", color=PAL["green"])

    # ── Phase 3: Commit ──
    y = 3.5
    _msg(ax, CXL, N0, y, "poll votes: 3 ACK ≥ 2", PAL["blue"], dashed=True, fontsize=8)
    y -= 0.45
    _msg(ax, N0, CXL, y, "store status=COMMITTED", PAL["blue"], bold=True, fontsize=8.5)
    y -= 0.3
    ax.text(N0 + 0.1, y + 0.1, "sfence", fontsize=7, color=PAL["gray"], style="italic")

    # commit point
    y_cp = 2.45
    ax.plot([0.0, 9.8], [y_cp, y_cp], ls="-", lw=1.8, color=PAL["orange"], zorder=1)
    ax.text(5.0, y_cp - 0.22, "── commit point ──", ha="center", fontsize=9,
            color=PAL["orange"], weight="bold")

    # ── Phase 4: Apply ──
    y = 1.8
    for nx, lbl, col in [(N0, "Node 0", PAL["blue"]),
                          (N1, "Node 1", PAL["purple"]),
                          (N2, "Node 2", PAL["purple"])]:
        _msg(ax, CXL, nx, y, "", col, dashed=True)
        y -= 0.08

    ax.text((N0 + N2) / 2, y + 0.2, "all nodes see COMMITTED", ha="center",
            fontsize=8, color=PAL["gray"], style="italic")

    y -= 0.35
    for nx, lbl, col in [(N0, "Node 0", PAL["blue"]),
                          (N1, "Node 1", PAL["purple"]),
                          (N2, "Node 2", PAL["purple"])]:
        _self_msg(ax, nx - 0.3, y, "Slot X := A", col, fontsize=7.5, w=0.5)

    ax.text((N0 + N2) / 2, y - 0.5, "write new_slot to local DRAM", ha="center",
            fontsize=8.5, color=PAL["black"])

    # bottom note
    _note(ax, 5.0, -0.6,
          "No hardware atomics. Only load/store + sfence on CXL memory.\n"
          "Majority (2 of 3) vote ensures safety under single node failure.",
          w=7.0, h=0.65, fontsize=8.5, bg="#FFF8E1")

    # legend
    h_n0 = mlines.Line2D([], [], color=PAL["blue"], lw=2, label="Node 0 (proposer)")
    h_nx = mlines.Line2D([], [], color=PAL["purple"], lw=2, label="Node 1 / 2 (voter)")
    h_solid = mlines.Line2D([], [], color="black", lw=1.2, label="store (write)")
    h_dash = mlines.Line2D([], [], color="black", lw=1.2, ls="--", label="load (read)")
    ax.legend(handles=[h_n0, h_nx, h_solid, h_dash], loc="lower left",
              fontsize=7.5, frameon=True, fancybox=True, framealpha=0.9,
              ncol=4, bbox_to_anchor=(-0.3, -0.17))

    fig.tight_layout()
    fig.savefig("docs/fig2_cxl_consensus.png", dpi=200, bbox_inches="tight")
    fig.savefig("docs/fig2_cxl_consensus.pdf", bbox_inches="tight")
    print("Saved fig2")
    return fig


# ── Main ────────────────────────────────────────────────────
if __name__ == "__main__":
    draw_fig1()
    draw_fig2()
    plt.show()