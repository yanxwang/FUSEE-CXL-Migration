#!/usr/bin/env python3
"""
Animated / cartoon-style diagrams for FUSEE consensus protocols.
Fig 1: RDMA CAS — two clients racing to grab a slot (like two hands reaching for the same cookie)
Fig 2: CXL Propose-Vote-Commit — election-style voting through a shared bulletin board
"""

import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.patches import FancyBboxPatch, Circle, FancyArrowPatch, Arc
from matplotlib.collections import PatchCollection
import numpy as np

plt.rcParams.update({
    "font.family": "sans-serif",
    "font.sans-serif": ["DejaVu Sans"],
    "font.size": 10,
    "figure.facecolor": "white",
})

# ── Colors ──
C = {
    "sky":      "#E3F2FD",
    "blue":     "#1565C0",
    "blue_l":   "#90CAF9",
    "red":      "#C62828",
    "red_l":    "#EF9A9A",
    "green":    "#2E7D32",
    "green_l":  "#A5D6A7",
    "orange":   "#E65100",
    "orange_l": "#FFB74D",
    "purple":   "#6A1B9A",
    "purple_l": "#CE93D8",
    "gray":     "#9E9E9E",
    "lgray":    "#EEEEEE",
    "dgray":    "#424242",
    "white":    "#FFFFFF",
    "yellow":   "#FFF9C4",
    "gold":     "#FFD54F",
}


def _cloud(ax, cx, cy, w, h, color, label="", fontsize=9, text_color="white"):
    """Rounded cloud-like box."""
    box = FancyBboxPatch((cx - w/2, cy - h/2), w, h,
                         boxstyle="round,pad=0.25", facecolor=color,
                         edgecolor="white", lw=2, zorder=3, alpha=0.92)
    ax.add_patch(box)
    if label:
        ax.text(cx, cy, label, ha="center", va="center", fontsize=fontsize,
                color=text_color, weight="bold", zorder=4)


def _slot_box(ax, cx, cy, value="0x0", color=C["lgray"], label="Slot X", w=1.2, h=0.6):
    """A small memory slot visualization."""
    box = FancyBboxPatch((cx - w/2, cy - h/2), w, h,
                         boxstyle="round,pad=0.08", facecolor=color,
                         edgecolor=C["dgray"], lw=1.5, zorder=3)
    ax.add_patch(box)
    ax.text(cx, cy + 0.08, label, ha="center", va="center", fontsize=7,
            color=C["dgray"], zorder=4)
    ax.text(cx, cy - 0.12, value, ha="center", va="center", fontsize=8,
            color=C["dgray"], weight="bold", zorder=4, family="monospace")


def _curved_arrow(ax, x0, y0, x1, y1, color="black", lw=2, style="->",
                  rad=0.2, zorder=5):
    a = FancyArrowPatch((x0, y0), (x1, y1), arrowstyle=style,
                        connectionstyle=f"arc3,rad={rad}",
                        color=color, lw=lw, zorder=zorder, mutation_scale=18)
    ax.add_patch(a)


def _speech(ax, cx, cy, text, w=2.0, h=0.5, color=C["yellow"], fontsize=8,
            tail_x=None, tail_y=None):
    """Speech bubble."""
    box = FancyBboxPatch((cx - w/2, cy - h/2), w, h,
                         boxstyle="round,pad=0.15", facecolor=color,
                         edgecolor=C["gray"], lw=1, zorder=6)
    ax.add_patch(box)
    ax.text(cx, cy, text, ha="center", va="center", fontsize=fontsize,
            color=C["dgray"], zorder=7, style="italic")
    if tail_x is not None:
        ax.plot([tail_x, cx], [tail_y, cy - h/2], color=C["gray"], lw=1, zorder=5)


def _big_arrow(ax, cx, cy, direction="down", color=C["orange"], size=0.3):
    """Fat triangle arrow."""
    if direction == "down":
        tri = plt.Polygon([(cx - size, cy + size), (cx + size, cy + size), (cx, cy - size)],
                          facecolor=color, edgecolor="white", lw=1.5, zorder=5)
    elif direction == "right":
        tri = plt.Polygon([(cx - size, cy + size), (cx - size, cy - size), (cx + size, cy)],
                          facecolor=color, edgecolor="white", lw=1.5, zorder=5)
    ax.add_patch(tri)


def _person(ax, cx, cy, color, label="", scale=1.0):
    """Simple stick-figure person icon (circle head + body)."""
    r = 0.18 * scale
    # head
    head = Circle((cx, cy + r * 1.5), r, facecolor=color, edgecolor="white", lw=1.5, zorder=4)
    ax.add_patch(head)
    # body
    ax.plot([cx, cx], [cy + r * 0.5, cy - r * 1.2], color=color, lw=2.5 * scale, zorder=3)
    # arms
    ax.plot([cx - r * 1.2, cx + r * 1.2], [cy, cy], color=color, lw=2 * scale, zorder=3)
    # legs
    ax.plot([cx, cx - r * 0.8], [cy - r * 1.2, cy - r * 2.5], color=color, lw=2 * scale, zorder=3)
    ax.plot([cx, cx + r * 0.8], [cy - r * 1.2, cy - r * 2.5], color=color, lw=2 * scale, zorder=3)
    if label:
        ax.text(cx, cy - r * 3.2, label, ha="center", va="center", fontsize=9,
                color=color, weight="bold", zorder=5)


def _server_icon(ax, cx, cy, color, label="", scale=1.0):
    """Simple server rack icon."""
    w, h = 1.0 * scale, 1.3 * scale
    # main box
    box = FancyBboxPatch((cx - w/2, cy - h/2), w, h,
                         boxstyle="round,pad=0.06", facecolor=color,
                         edgecolor="white", lw=2, zorder=3, alpha=0.9)
    ax.add_patch(box)
    # rack lines
    for dy in [-0.25, 0.0, 0.25]:
        ax.plot([cx - w * 0.35, cx + w * 0.35], [cy + dy * scale, cy + dy * scale],
                color="white", lw=1.5, zorder=4, alpha=0.6)
    # blinking light
    ax.plot(cx - w * 0.3, cy + 0.35 * scale, 'o', color=C["green_l"], markersize=4, zorder=4)
    if label:
        ax.text(cx, cy - h/2 - 0.22, label, ha="center", va="center", fontsize=9,
                color=color, weight="bold", zorder=5)


# ================================================================
#  FIGURE 1 — RDMA Consensus: "Two Hands, One Cookie"
# ================================================================
def draw_fig1_animated():
    fig, axes = plt.subplots(1, 3, figsize=(15, 5.5),
                             gridspec_kw={"width_ratios": [1, 1, 1]})
    fig.suptitle("RDMA CAS Consensus: Two Clients Race for One Slot",
                 fontsize=14, fontweight="bold", y=0.98)

    for ax in axes:
        ax.set_xlim(-0.5, 6.5)
        ax.set_ylim(-1.0, 6.5)
        ax.set_aspect("equal")
        ax.axis("off")

    # ── Panel 1: Both clients reach for the slot ──
    ax = axes[0]
    ax.set_title("Step 1: Both attempt CAS", fontsize=11, fontweight="bold", pad=6)

    # background
    ax.add_patch(FancyBboxPatch((-0.3, -0.8), 6.6, 7.0, boxstyle="round,pad=0.2",
                 facecolor=C["sky"], edgecolor="none", zorder=0))

    # servers
    _server_icon(ax, 3.0, 5.2, C["green"], "Backup (MN 1)", scale=0.8)
    _slot_box(ax, 3.0, 3.8, "0x0", C["white"], "Slot X")

    # clients
    _person(ax, 0.8, 2.0, C["blue"], "Client A")
    _person(ax, 5.2, 2.0, C["red"], "Client B")

    # reaching arrows
    _curved_arrow(ax, 1.1, 2.8, 2.5, 3.7, C["blue"], lw=2.5, rad=0.3)
    _curved_arrow(ax, 4.9, 2.8, 3.5, 3.7, C["red"], lw=2.5, rad=-0.3)

    _speech(ax, 1.0, 0.2, 'CAS(0x0 -> A)', w=2.0, h=0.4, color=C["blue_l"],
            tail_x=0.8, tail_y=0.7)
    _speech(ax, 5.0, 0.2, 'CAS(0x0 -> B)', w=2.0, h=0.4, color=C["red_l"],
            tail_x=5.2, tail_y=0.7)

    ax.text(3.0, -0.6, "Hardware CAS is atomic:\nonly ONE can succeed!",
            ha="center", fontsize=9, color=C["dgray"], style="italic",
            bbox=dict(boxstyle="round,pad=0.3", fc=C["yellow"], ec=C["gold"], lw=1.2))

    # ── Panel 2: A wins, B loses ──
    ax = axes[1]
    ax.set_title("Step 2: A wins the CAS", fontsize=11, fontweight="bold", pad=6)

    ax.add_patch(FancyBboxPatch((-0.3, -0.8), 6.6, 7.0, boxstyle="round,pad=0.2",
                 facecolor=C["sky"], edgecolor="none", zorder=0))

    _server_icon(ax, 3.0, 5.2, C["green"], "Backup (MN 1)", scale=0.8)
    _slot_box(ax, 3.0, 3.8, "= A", C["blue_l"], "Slot X")

    _person(ax, 0.8, 2.0, C["blue"], "Client A")
    _person(ax, 5.2, 2.0, C["red"], "Client B")

    # return arrows
    _curved_arrow(ax, 2.5, 3.6, 1.1, 2.8, C["green"], lw=2, rad=-0.3)
    ax.text(1.0, 3.6, "ret=0x0", fontsize=9, color=C["green"], weight="bold")

    _curved_arrow(ax, 3.5, 3.6, 4.9, 2.8, C["red"], lw=2, rad=0.3)
    ax.text(4.5, 3.6, "ret=A", fontsize=9, color=C["red"], weight="bold")

    # reaction speech
    _speech(ax, 0.8, 0.2, "I won!\nWIN_ALL", w=1.6, h=0.5, color="#C8E6C9",
            tail_x=0.8, tail_y=0.7)
    _speech(ax, 5.2, 0.2, "I lost...\nFAIL", w=1.6, h=0.5, color="#FFCDD2",
            tail_x=5.2, tail_y=0.7)

    # trophy for A
    ax.text(0.8, 4.2, "W", fontsize=22, ha="center", va="center",
            color=C["gold"], weight="bold", zorder=6,
            bbox=dict(boxstyle="circle,pad=0.2", fc=C["gold"], ec=C["orange"], lw=2))

    # X for B
    ax.text(5.2, 4.2, "X", fontsize=20, ha="center", va="center",
            color=C["red"], weight="bold", zorder=6,
            bbox=dict(boxstyle="circle,pad=0.2", fc=C["red_l"], ec=C["red"], lw=2))

    # ── Panel 3: A commits to Primary ──
    ax = axes[2]
    ax.set_title("Step 3: Winner commits to Primary", fontsize=11, fontweight="bold", pad=6)

    ax.add_patch(FancyBboxPatch((-0.3, -0.8), 6.6, 7.0, boxstyle="round,pad=0.2",
                 facecolor=C["sky"], edgecolor="none", zorder=0))

    _server_icon(ax, 1.8, 5.2, C["green"], "Primary (MN 0)", scale=0.8)
    _slot_box(ax, 1.8, 3.8, "0x0 -> A", C["green_l"], "Slot X")

    _server_icon(ax, 4.5, 5.2, C["green"], "Backup (MN 1)", scale=0.8)
    _slot_box(ax, 4.5, 3.8, "= A", C["blue_l"], "Slot X")

    _person(ax, 1.8, 1.5, C["blue"], "Client A")

    _curved_arrow(ax, 1.8, 2.3, 1.8, 3.4, C["blue"], lw=2.5, rad=0)

    _speech(ax, 1.8, 0.0, "CAS Primary\n(0x0 -> A)", w=2.0, h=0.5, color=C["blue_l"],
            tail_x=1.8, tail_y=0.5)

    # big checkmark
    ax.text(4.5, 1.2, "DONE", fontsize=18, ha="center", va="center",
            color=C["green"], weight="bold", zorder=6,
            bbox=dict(boxstyle="round,pad=0.4", fc="#E8F5E9", ec=C["green"], lw=2.5))

    # commit point line
    ax.plot([-0.2, 6.3], [2.8, 2.8], ls="-", lw=2.5, color=C["orange"], zorder=1)
    ax.text(3.2, 2.55, "commit point", fontsize=9, color=C["orange"],
            ha="center", weight="bold")

    fig.tight_layout(rect=[0, 0, 1, 0.94])
    fig.savefig("docs/fig1_rdma_animated.png", dpi=200, bbox_inches="tight")
    fig.savefig("docs/fig1_rdma_animated.pdf", bbox_inches="tight")
    print("Saved fig1_rdma_animated")


# ================================================================
#  FIGURE 2 — CXL Consensus: "Bulletin Board Election"
# ================================================================
def draw_fig2_animated():
    fig, axes = plt.subplots(2, 2, figsize=(13, 11))
    fig.suptitle("CXL Software Consensus: Propose-Vote-Commit via Shared Memory",
                 fontsize=14, fontweight="bold", y=0.98)

    for row in axes:
        for ax in row:
            ax.set_xlim(-0.5, 8.5)
            ax.set_ylim(-0.8, 6.8)
            ax.set_aspect("equal")
            ax.axis("off")

    # ── Helper: draw the common scene ──
    def _scene(ax, title):
        ax.set_title(title, fontsize=11, fontweight="bold", pad=6)
        # background
        ax.add_patch(FancyBboxPatch((-0.3, -0.6), 8.6, 7.2, boxstyle="round,pad=0.2",
                     facecolor="#F3E5F5", edgecolor="none", zorder=0))
        # three nodes
        _person(ax, 1.2, 1.2, C["blue"], "Node 0")
        _person(ax, 4.0, 1.2, C["purple"], "Node 1")
        _person(ax, 6.8, 1.2, C["purple"], "Node 2")
        # local DRAM boxes
        for x, lbl in [(1.2, "DRAM 0"), (4.0, "DRAM 1"), (6.8, "DRAM 2")]:
            _slot_box(ax, x, -0.2, "0x0", C["white"], lbl, w=1.1, h=0.45)

    def _cxl_board(ax, lines, highlight_color=None):
        """The CXL shared memory as a bulletin board."""
        bx, by, bw, bh = 2.5, 4.5, 3.5, 1.8
        # board
        box = FancyBboxPatch((bx, by), bw, bh, boxstyle="round,pad=0.1",
                             facecolor=C["orange_l"], edgecolor=C["orange"],
                             lw=2.5, zorder=3, alpha=0.9)
        ax.add_patch(box)
        ax.text(bx + bw/2, by + bh - 0.2, "CXL Shared Memory",
                ha="center", fontsize=9, color=C["orange"], weight="bold", zorder=4)
        # pin
        ax.plot(bx + bw/2, by + bh + 0.05, 'o', color=C["red"], markersize=8, zorder=5)
        # content lines
        for i, line in enumerate(lines):
            color = C["dgray"]
            fw = "normal"
            if highlight_color and i == len(lines) - 1:
                color = highlight_color
                fw = "bold"
            ax.text(bx + 0.25, by + bh - 0.55 - i * 0.32, line,
                    fontsize=8, color=color, weight=fw, zorder=4, family="monospace")

    # ════════════════════════════════════════
    # Panel 1: Propose
    # ════════════════════════════════════════
    ax = axes[0][0]
    _scene(ax, "Phase 1: Node 0 Proposes")

    _cxl_board(ax, [
        "proposer: Node 0",
        "old=0x0  new=A",
        "status: PROPOSED",
        "votes: [ACK, ?, ?]",
    ])

    # arrow from Node 0 to board
    _curved_arrow(ax, 1.5, 2.2, 3.2, 4.5, C["blue"], lw=2.5, rad=0.3)
    _speech(ax, 1.2, 3.2, "I propose\nSlot X = A", w=1.8, h=0.55, color=C["blue_l"],
            tail_x=1.2, tail_y=2.6)

    # sfence annotation
    ax.text(2.0, 4.0, "sfence", fontsize=8, color=C["gray"], style="italic",
            rotation=30)

    # ════════════════════════════════════════
    # Panel 2: Vote
    # ════════════════════════════════════════
    ax = axes[0][1]
    _scene(ax, "Phase 2: Nodes Vote")

    _cxl_board(ax, [
        "proposer: Node 0",
        "old=0x0  new=A",
        "status: PROPOSED",
        "votes: [ACK, ACK, ACK]",
    ], highlight_color=C["green"])

    # Node 1 checks local then votes
    _curved_arrow(ax, 4.0, 2.2, 4.0, 4.5, C["purple"], lw=2, rad=0.15)
    _speech(ax, 4.5, 3.0, "my Slot X\n== 0x0\nACK!", w=1.4, h=0.6, color=C["purple_l"],
            tail_x=4.0, tail_y=2.6)

    # Node 2 checks local then votes
    _curved_arrow(ax, 6.8, 2.2, 5.5, 4.5, C["purple"], lw=2, rad=-0.2)
    _speech(ax, 7.2, 3.3, "my Slot X\n== 0x0\nACK!", w=1.4, h=0.6, color=C["purple_l"],
            tail_x=6.8, tail_y=2.6)

    # Node 0 self-vote
    ax.text(0.8, 3.2, "ACK", fontsize=10, color=C["blue"], weight="bold",
            bbox=dict(boxstyle="round,pad=0.15", fc=C["blue_l"], ec=C["blue"], lw=1))

    # ════════════════════════════════════════
    # Panel 3: Commit
    # ════════════════════════════════════════
    ax = axes[1][0]
    _scene(ax, "Phase 3: Majority Reached -> COMMITTED")

    _cxl_board(ax, [
        "proposer: Node 0",
        "old=0x0  new=A",
        "status: COMMITTED",
        "votes: [ACK, ACK, ACK]",
    ], highlight_color=C["green"])

    # Node 0 reads votes and writes committed
    _curved_arrow(ax, 1.5, 2.2, 3.0, 4.5, C["blue"], lw=2.5, rad=0.3)

    # tally box
    ax.text(1.2, 3.5, "3/3\nACK", fontsize=12, ha="center", va="center",
            color=C["green"], weight="bold", zorder=6,
            bbox=dict(boxstyle="round,pad=0.3", fc="#E8F5E9", ec=C["green"], lw=2))

    # arrow pointing to "COMMITTED"
    _big_arrow(ax, 4.25, 4.35, "down", C["green"], size=0.15)
    ax.text(4.25, 3.95, "COMMITTED!", fontsize=10, ha="center",
            color=C["green"], weight="bold")

    # commit point line
    ax.plot([-0.2, 8.3], [3.4, 3.4], ls="-", lw=2.5, color=C["orange"], zorder=1)
    ax.text(6.5, 3.15, "commit point", fontsize=9, color=C["orange"],
            ha="center", weight="bold")

    # ════════════════════════════════════════
    # Panel 4: Apply
    # ════════════════════════════════════════
    ax = axes[1][1]
    _scene(ax, "Phase 4: All Nodes Apply to Local DRAM")

    _cxl_board(ax, [
        "proposer: Node 0",
        "old=0x0  new=A",
        "status: COMMITTED",
        "votes: [ACK, ACK, ACK]",
    ], highlight_color=C["green"])

    # overwrite the DRAM boxes with updated values
    for x, lbl in [(1.2, "DRAM 0"), (4.0, "DRAM 1"), (6.8, "DRAM 2")]:
        _slot_box(ax, x, -0.2, "= A", C["green_l"], lbl, w=1.1, h=0.45)

    # arrows from board down to each node
    for x in [1.2, 4.0, 6.8]:
        _curved_arrow(ax, 4.25, 4.5, x, 0.15, C["green"], lw=2, rad=0, style="->")

    # celebration text
    ax.text(4.0, 2.8, "All replicas\nconsistent!", fontsize=12,
            ha="center", va="center", color=C["green"], weight="bold",
            bbox=dict(boxstyle="round,pad=0.4", fc="#E8F5E9", ec=C["green"], lw=2))

    # bottom annotation
    fig.text(0.5, 0.01,
             "No hardware atomics needed. Only load/store + sfence on CXL shared memory.  "
             "Majority (2/3) ensures safety.",
             ha="center", fontsize=10, color=C["dgray"], style="italic",
             bbox=dict(boxstyle="round,pad=0.4", fc=C["yellow"], ec=C["gold"], lw=1.2))

    fig.tight_layout(rect=[0, 0.04, 1, 0.94])
    fig.savefig("docs/fig2_cxl_animated.png", dpi=200, bbox_inches="tight")
    fig.savefig("docs/fig2_cxl_animated.pdf", bbox_inches="tight")
    print("Saved fig2_cxl_animated")


# ── Main ──
if __name__ == "__main__":
    draw_fig1_animated()
    draw_fig2_animated()
    plt.show()