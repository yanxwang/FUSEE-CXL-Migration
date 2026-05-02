"""Shared matplotlib style for FUSEE-CXL plots — STYLE B (greyscale + accent).

**Project plotting standard, set 2026-04-26.**

Two-line integration:

    from plot_style import apply_style, COLORS, STYLE_B
    apply_style()

Visual signature:
- 3-tone greyscale + 1 accent red (#c44e52) for ordered 3-class data
  (kv-size, flusher-N). Accent always lands on the largest / focal
  category.
- Title `pad=10` so titles are not crowded against bars.
- Y-axis upper bound = `max(data) × 1.25` (use `bar_with_headroom`
  helper, or set explicitly) so bar-top annotations breathe.
- Bar edges: 0.8 pt at color #222 — gives bars visual weight.
- Otherwise matplotlib defaults (font, figsize 9×5 for these plots).

To change the project plot style globally, edit THIS file. Do not
hardcode colors or rcParams inside individual `plot_*.py` scripts.

Reference figure (the look this style produces):
  docs/iter5_kv_n_compare/iter5_bestn_thpt_bars.png   (Style B canonical)
"""
from __future__ import annotations

import matplotlib.pyplot as _plt


# ---- Palettes ----

# STYLE B — the project standard. Greyscale + accent.
# Index 0 = light grey (smallest / least focal),
# index 1 = mid grey,
# index 2 = accent red (largest / focal).
STYLE_B = ["#d9d9d9", "#969696", "#c44e52"]

# 4-tone variant for ordered 4-class data (e.g. iter-4 kv ∈ {8,256,512,1024}).
# Lightest → accent.
STYLE_B4 = ["#ededed", "#bdbdbd", "#737373", "#c44e52"]

STYLE_B_EDGE = "#222222"
STYLE_B_EDGE_LW = 0.8
STYLE_B_TARGET = "gray"     # for axhline reference (e.g. 20 Mops/s bar)

# Legacy seaborn-deep — kept for line plots and N≥4-class data where
# greyscale+accent doesn't apply naturally. NOT the default.
SEABORN_DEEP = {
    "blue":   "#4c72b0",
    "green":  "#55a868",
    "red":    "#c44e52",
    "purple": "#8172b2",
    "yellow": "#ccb974",
    "cyan":   "#64b5cd",
}

# Semantic mapping: stable color per dimension across all plots.
# "smaller / less focal → lighter; largest → accent" is the rule.
COLORS = {
    # Value-size (3-class with natural ordering 256 < 512 < 1024).
    # When 4-class (kv8 + 256 + 512 + 1024), use STYLE_B4 directly.
    "kv256":  STYLE_B[0],
    "kv512":  STYLE_B[1],
    "kv1024": STYLE_B[2],

    # 4-class kv variant (use STYLE_B4 indices)
    "kv8_4":   STYLE_B4[0],
    "kv256_4": STYLE_B4[1],
    "kv512_4": STYLE_B4[2],
    "kv1024_4": STYLE_B4[3],
    # Convenience: kv8 alone defaults to STYLE_B[0] for the rare case
    # someone plots kv ∈ {8, 256, 512} only.
    "kv8":    STYLE_B[0],

    # Flusher counts (iter-5 multi-flusher: 1 < 2 < 4)
    "N1": STYLE_B[0],
    "N2": STYLE_B[1],
    "N4": STYLE_B[2],

    # Protocols (focus is usually A — write-path bottleneck)
    "A": STYLE_B[2],
    "B": STYLE_B[1],
    "C": STYLE_B[0],

    # YCSB workloads — fall back to seaborn-deep (5 classes; greyscale
    # palette doesn't extend cleanly past 3).
    "workloada": SEABORN_DEEP["red"],
    "workloadb": SEABORN_DEEP["green"],
    "workloadc": SEABORN_DEEP["blue"],
    "workloadd": SEABORN_DEEP["purple"],
    "workloadf": SEABORN_DEEP["yellow"],

    # Reference / target lines
    "target":  STYLE_B_TARGET,
}


def apply_style() -> None:
    """Apply project plotting standard rcParams.

    Call once at module import time of every plot_*.py. Sets:
    - title pad (avoid title crowding against bar tops)
    - savefig dpi + tight bbox
    - Otherwise leaves matplotlib defaults intact.
    """
    _plt.rcParams.update({
        "axes.titlepad":  10,       # breathing room between title and plot
        "savefig.dpi":    120,
        "savefig.bbox":   "tight",
    })


def bar_with_headroom(ax, *, headroom: float = 1.25) -> None:
    """Apply Style B bar chart polish in one call.

    - Set y-upper limit to `max(bar_height) × headroom` so bar-top
      annotations don't crowd the title.
    - Apply Style B edge color + width to all bars currently on ax.
    - Y-axis grid only, behind the bars.

    Call AFTER all `ax.bar(...)` calls but BEFORE `fig.tight_layout()`.
    """
    # Edges.
    for patch in ax.patches:
        patch.set_edgecolor(STYLE_B_EDGE)
        patch.set_linewidth(STYLE_B_EDGE_LW)
    # Y-headroom from the tallest bar currently drawn.
    max_y = 0.0
    for patch in ax.patches:
        h = patch.get_height()
        if h > max_y:
            max_y = h
    if max_y > 0:
        ax.set_ylim(0, max_y * headroom)
    # Subtle y-grid behind bars.
    ax.grid(axis="y", alpha=0.3)
    ax.set_axisbelow(True)
