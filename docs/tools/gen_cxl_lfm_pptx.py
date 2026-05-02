#!/usr/bin/env python3
"""
New CXL consensus design: Per-Slot LFM + Shared Update Log.
Step-by-step PPTX + PNGs.
3 unified nodes (top) -> CXL shared memory (bottom).
"""

import os
from pptx import Presentation
from pptx.util import Inches, Pt
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN
from pptx.enum.shapes import MSO_SHAPE
from pptx.oxml.ns import qn

import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch

os.makedirs("docs", exist_ok=True)

plt.rcParams.update({
    "font.family": "sans-serif",
    "font.sans-serif": ["DejaVu Sans"],
    "font.size": 9,
    "figure.facecolor": "white",
})

# ── Colors (matplotlib) ──
C = {
    "comp": "#1565C0", "comp_l": "#BBDEFB",
    "voter": "#5E35B1", "voter_l": "#D1C4E9",
    "cxl": "#E65100", "cxl_l": "#FFE0B2",
    "idx": "#FFF9C4", "data": "#E3F2FD", "log": "#F3E5F5",
    "lock": "#FFCCBC",
    "gray": "#9E9E9E", "lgray": "#F5F5F5", "dg": "#333333",
    "white": "#FFFFFF", "ok": "#1B5E20", "fail": "#B71C1C",
    "commit": "#FF6F00", "hi": "#FF8F00",
}

# ── Colors (pptx) ──
P_WHITE = RGBColor(0xFF, 0xFF, 0xFF)
P_COMP  = RGBColor(0x15, 0x65, 0xC0); P_COMP_L = RGBColor(0xBB, 0xDE, 0xFB)
P_VOTER = RGBColor(0x5E, 0x35, 0xB1); P_VOTER_L = RGBColor(0xD1, 0xC4, 0xE9)
P_CXL   = RGBColor(0xE6, 0x51, 0x00); P_CXL_L = RGBColor(0xFF, 0xE0, 0xB2)
P_IDX   = RGBColor(0xFF, 0xF9, 0xC4)
P_DATA  = RGBColor(0xE3, 0xF2, 0xFD)
P_LOG   = RGBColor(0xF3, 0xE5, 0xF5)
P_LOCK  = RGBColor(0xFF, 0xCC, 0xBC)
P_GRAY  = RGBColor(0x9E, 0x9E, 0x9E)
P_LGRAY = RGBColor(0xEE, 0xEE, 0xEE)
P_DG    = RGBColor(0x33, 0x33, 0x33)
P_OK    = RGBColor(0x1B, 0x5E, 0x20)
P_COMMIT = RGBColor(0xFF, 0x6F, 0x00)
P_HI    = RGBColor(0xFF, 0x8F, 0x00)


# ════════════════════════════════════════════════
#  Layout (shared between matplotlib and pptx)
# ════════════════════════════════════════════════
COL_X = [0.5, 4.8, 9.1]
NW = 3.8
NH = 2.0
NY = 0.8

CXL_X = 0.3
CXL_Y = 4.2
CXL_W = 12.7
CXL_H = 2.8

# CXL inner regions
LK_X = CXL_X + 0.2          # Lock Table (NEW)
LK_Y = CXL_Y + 0.5
LK_W = 5.0
LK_H = 1.9

OL_X = CXL_X + 5.5          # OpLog
OL_Y = LK_Y
OL_W = 2.7
OL_H = 1.9

ST_X = CXL_X + 8.5          # Staging
ST_Y = LK_Y
ST_W = 2.4
ST_H = 1.9

AD_X = CXL_X + 11.1         # Alloc Dir
AD_Y = LK_Y
AD_W = 1.5
AD_H = 1.9


# ════════════════════════════════════════════════
#  PPTX helpers
# ════════════════════════════════════════════════
def _in(v):
    return Inches(v)

def _pbox(slide, l, t, w, h, fc, ec=None, bw=Pt(1.5), text="", fs=9, c=P_DG, bold=True):
    s = slide.shapes.add_shape(MSO_SHAPE.ROUNDED_RECTANGLE,
                               _in(l), _in(t), _in(w), _in(h))
    s.fill.solid(); s.fill.fore_color.rgb = fc
    if ec:
        s.line.color.rgb = ec; s.line.width = bw
    else:
        s.line.fill.background()
    if text:
        tf = s.text_frame; tf.word_wrap = True
        p = tf.paragraphs[0]; p.alignment = PP_ALIGN.CENTER
        p.space_before = Pt(0); p.space_after = Pt(0)
        run = p.add_run(); run.text = text
        run.font.size = Pt(fs); run.font.color.rgb = c; run.font.bold = bold

def _ptxt(slide, l, t, w, h, text, fs=9, c=P_DG, bold=False, align=PP_ALIGN.CENTER):
    tb = slide.shapes.add_textbox(_in(l), _in(t), _in(w), _in(h))
    tf = tb.text_frame; tf.word_wrap = True
    p = tf.paragraphs[0]; p.alignment = align
    run = p.add_run(); run.text = text
    run.font.size = Pt(fs); run.font.color.rgb = c; run.font.bold = bold

def _parr(slide, x0, y0, x1, y1, c=P_COMP, w=Pt(2), dashed=False):
    conn = slide.shapes.add_connector(1, _in(x0), _in(y0), _in(x1), _in(y1))
    conn.line.color.rgb = c; conn.line.width = w
    if dashed:
        conn.line.dash_style = 2
    fmt = conn.line._ln
    tail = fmt.makeelement(qn('a:tailEnd'), {})
    tail.set('type', 'triangle'); tail.set('w', 'med'); tail.set('len', 'med')
    fmt.append(tail)

def _pnode(slide, col, title, nc, ncl, idx_text="Slot X = 0x0", kv_text="(empty)",
           hi_idx=False, hi_kv=False):
    x = COL_X[col]
    _pbox(slide, x, NY, NW, NH, ncl, nc, Pt(2), title, 10, nc)
    bc = P_HI if hi_idx else P_GRAY; bw = Pt(2.5) if hi_idx else Pt(0.8)
    _pbox(slide, x+0.1, NY+0.55, 1.7, 0.55, P_IDX, bc, bw,
          f"Local Index\n{idx_text}", 7, P_OK if hi_idx else P_DG, bold=hi_idx)
    bc = P_HI if hi_kv else P_GRAY; bw = Pt(2.5) if hi_kv else Pt(0.8)
    _pbox(slide, x+1.95, NY+0.55, 1.7, 0.55, P_DATA, bc, bw,
          f"Local KV\n{kv_text}", 7, P_OK if hi_kv else P_DG, bold=hi_kv)
    _pbox(slide, x+0.1, NY+1.3, NW-0.2, 0.4, P_LGRAY, P_GRAY, Pt(0.5),
          "Replication Fiber", 6, P_GRAY)

def _pcxl(slide, lk_lines=None, ol_text="(empty)", st_text="(empty)",
          hi_lk=False, hi_ol=False, hi_st=False):
    _pbox(slide, CXL_X, CXL_Y, CXL_W, CXL_H, P_CXL_L, P_CXL, Pt(2.5))
    _ptxt(slide, CXL_X, CXL_Y+0.05, CXL_W, 0.35,
          "CXL Type 3 Shared Memory", 12, P_CXL, bold=True)

    # Lock Table (NEW)
    bc = P_HI if hi_lk else P_GRAY
    bw = Pt(2.5) if hi_lk else Pt(0.8)
    _pbox(slide, LK_X, LK_Y, LK_W, LK_H, P_LOCK, bc, bw)
    _ptxt(slide, LK_X, LK_Y+0.05, LK_W, 0.3,
          "SlotLock Table (per-hash-slot)", 9, P_CXL, bold=True)
    if lk_lines:
        for i, line in enumerate(lk_lines):
            _ptxt(slide, LK_X+0.15, LK_Y+0.4+i*0.32, LK_W-0.3, 0.3,
                  line, 8, P_OK if hi_lk else P_DG, bold=hi_lk,
                  align=PP_ALIGN.LEFT)

    # OpLog
    bc = P_HI if hi_ol else P_GRAY
    bw = Pt(2.5) if hi_ol else Pt(0.8)
    _pbox(slide, OL_X, OL_Y, OL_W, OL_H, P_LOG, bc, bw,
          f"OpLog (per-node)\n{ol_text}", 8, P_OK if hi_ol else P_DG, bold=hi_ol)

    # Staging
    bc = P_HI if hi_st else P_GRAY
    bw = Pt(2.5) if hi_st else Pt(0.8)
    _pbox(slide, ST_X, ST_Y, ST_W, ST_H, P_DATA, bc, bw,
          f"Staging Buffer\n{st_text}", 8, P_OK if hi_st else P_DG, bold=hi_st)

    # Alloc Dir
    _pbox(slide, AD_X, AD_Y, AD_W, AD_H, P_LGRAY, P_GRAY, Pt(0.8),
          "Alloc\nDir", 7, P_GRAY)

def _switch(slide):
    for col in range(3):
        x = COL_X[col] + NW/2
        _parr(slide, x, NY+NH, x, CXL_Y, P_GRAY, Pt(0.8), dashed=True)
    _ptxt(slide, 5.5, 3.45, 2, 0.25, "CXL Switch", 8, P_GRAY)

def _legend(slide):
    y = 7.1
    items = [("Proposer", P_COMP_L, P_COMP), ("Voter", P_VOTER_L, P_VOTER),
             ("CXL Mem", P_CXL_L, P_CXL), ("SlotLock", P_LOCK, P_GRAY),
             ("Index", P_IDX, P_GRAY), ("OpLog", P_LOG, P_GRAY),
             ("KV/Staging", P_DATA, P_GRAY), ("Modified", P_WHITE, P_HI)]
    for i, (lbl, fc, ec) in enumerate(items):
        x = 0.7 + i * 1.55
        _pbox(slide, x, y, 0.25, 0.18, fc, ec, Pt(1.5))
        _ptxt(slide, x+0.3, y-0.02, 1.1, 0.22, lbl, 7, P_DG)

def _add_slide(title_text, title_color=P_COMP):
    slide = prs.slides.add_slide(prs.slide_layouts[6])
    _ptxt(slide, 0, 0.05, 13.33, 0.5, title_text, 16, title_color, bold=True)
    return slide


# Lock states
LK_FREE = [
    "SlotLock[idx]:",
    "  shm_mutex_t lock = FREE",
    "  uint64_t value = 0x0",
]
LK_HELD = [
    "SlotLock[idx]:",
    "  shm_mutex_t lock = HELD by Node 0",
    "  uint64_t value = 0x0",
]
LK_HELD_NEW = [
    "SlotLock[idx]:",
    "  shm_mutex_t lock = HELD by Node 0",
    "  uint64_t value = A",
]
LK_RELEASED = [
    "SlotLock[idx]:",
    "  shm_mutex_t lock = FREE",
    "  uint64_t value = A",
]


# ════════════════════════════════════════════════
#  PPTX generation
# ════════════════════════════════════════════════
prs = Presentation()
prs.slide_width = Inches(13.33)
prs.slide_height = Inches(7.5)

# ─── STEP 1: hash + locate SlotLock[idx] ───
slide = _add_slide("Step 1: Hash key, locate SlotLock[idx] (local computation)")
_pnode(slide, 0, "Node 0 (Writer)", P_COMP, P_COMP_L)
_pnode(slide, 1, "Node 1", P_VOTER, P_VOTER_L)
_pnode(slide, 2, "Node 2", P_VOTER, P_VOTER_L)
_pcxl(slide, LK_FREE)
_switch(slide)
_ptxt(slide, 0.5, 3.0, 4.5, 0.7,
      "Node 0: idx = hash('hello') % NUM_SLOTS\nLocal computation only - no CXL access yet",
      9, P_COMP, bold=True)
_legend(slide)

# ─── STEP 2: shm_mutex_lock ───
slide = _add_slide("Step 2: shm_mutex_lock(&SlotLock[idx].lock)  [LFM acquire]")
_pnode(slide, 0, "Node 0 (Writer)", P_COMP, P_COMP_L)
_pnode(slide, 1, "Node 1", P_VOTER, P_VOTER_L)
_pnode(slide, 2, "Node 2", P_VOTER, P_VOTER_L)
_pcxl(slide, LK_HELD, hi_lk=True)
_switch(slide)
_parr(slide, COL_X[0]+NW/2-0.3, NY+NH, LK_X+1.5, CXL_Y, P_COMP, Pt(2.5))
_parr(slide, LK_X+1.5, CXL_Y, COL_X[0]+NW/2+0.3, NY+NH, P_COMP, Pt(1.5), dashed=True)
_ptxt(slide, 0.5, 3.0, 4.5, 0.7,
      "LFM fast path (~3 CACHELINE ops):\nb[0]:=1 -> x:=0 -> y:=0 -> read x -> LOCKED",
      9, P_COMP, bold=True)
_legend(slide)

# ─── STEP 3: Read SlotLock[idx].value, verify ───
slide = _add_slide("Step 3: Read SlotLock[idx].value, verify == 0x0 (empty for INSERT)")
_pnode(slide, 0, "Node 0 (Writer)", P_COMP, P_COMP_L)
_pnode(slide, 1, "Node 1", P_VOTER, P_VOTER_L)
_pnode(slide, 2, "Node 2", P_VOTER, P_VOTER_L)
_pcxl(slide, LK_HELD, hi_lk=True)
_switch(slide)
_parr(slide, LK_X+2.5, CXL_Y, COL_X[0]+NW/2, NY+NH, P_COMP, Pt(2), dashed=True)
_ptxt(slide, 0.5, 3.0, 4.5, 0.7,
      "Read CXL slot value -> 0x0 (matches expected)\nIf != 0x0: unlock & abort (slot already taken)",
      9, P_COMP, bold=True)
_ptxt(slide, LK_X+LK_W+0.1, LK_Y+1.3, 1.5, 0.4,
      "value = 0x0\n(empty, ok)", 8, P_OK, bold=True)
_legend(slide)

# ─── STEP 4: Write KV to local + staging + OpLog ───
slide = _add_slide("Step 4: Write KV to local DRAM + CXL Staging + OpLog (IN_PROGRESS)")
_pnode(slide, 0, "Node 0 (Writer)", P_COMP, P_COMP_L,
       kv_text="hello:world", hi_kv=True)
_pnode(slide, 1, "Node 1", P_VOTER, P_VOTER_L)
_pnode(slide, 2, "Node 2", P_VOTER, P_VOTER_L)
_pcxl(slide, LK_HELD, "Node 0:\nIN_PROGRESS", "hello:world",
      hi_lk=False, hi_ol=True, hi_st=True)
_switch(slide)
_parr(slide, COL_X[0]+NW/2+0.5, NY+NH, ST_X+ST_W/2, CXL_Y, P_COMP, Pt(2))
_parr(slide, COL_X[0]+NW/2-0.3, NY+NH, OL_X+OL_W/2, CXL_Y, P_COMP, Pt(2))
_ptxt(slide, 0.5, 3.0, 4.5, 0.7,
      "Node 0 writes KV to own DRAM\n+ stages KV in CXL for replication\n+ OpLog := IN_PROGRESS (for crash recovery)",
      9, P_COMP, bold=True)
_legend(slide)

# ─── STEP 5: Write SlotLock[idx].value = A (COMMIT POINT) ───
slide = _add_slide("Step 5: Write SlotLock[idx].value = A  ==  COMMIT POINT", P_COMMIT)
_pnode(slide, 0, "Node 0 (Writer)", P_COMP, P_COMP_L,
       idx_text="Slot X = A", kv_text="hello:world", hi_idx=True)
_pnode(slide, 1, "Node 1", P_VOTER, P_VOTER_L)
_pnode(slide, 2, "Node 2", P_VOTER, P_VOTER_L)
_pcxl(slide, LK_HELD_NEW, "Node 0:\nCOMMITTED", "hello:world",
      hi_lk=True, hi_ol=True)
_switch(slide)
_parr(slide, COL_X[0]+NW/2-0.5, NY+NH, LK_X+2.0, CXL_Y, P_COMMIT, Pt(2.5))
_parr(slide, COL_X[0]+NW/2+0.5, NY+NH, OL_X+OL_W/2, CXL_Y, P_COMP, Pt(2))
_ptxt(slide, 0.5, 2.8, 4.5, 1.0,
      "1. SlotLock[idx].value := A\n   (single source of truth in CXL)\n"
      "2. Update own local Index cache\n3. OpLog := COMMITTED",
      9, P_COMMIT, bold=True)
# commit point line
line = slide.shapes.add_connector(1, _in(0.3), _in(3.85), _in(13.0), _in(3.85))
line.line.color.rgb = P_COMMIT; line.line.width = Pt(3)
_ptxt(slide, 9.5, 3.6, 3.5, 0.35, "COMMIT POINT", 14, P_COMMIT, bold=True)
_legend(slide)

# ─── STEP 6: shm_mutex_unlock ───
slide = _add_slide("Step 6: shm_mutex_unlock(&SlotLock[idx].lock)")
_pnode(slide, 0, "Node 0 (Writer)", P_COMP, P_COMP_L,
       idx_text="Slot X = A", kv_text="hello:world")
_pnode(slide, 1, "Node 1", P_VOTER, P_VOTER_L)
_pnode(slide, 2, "Node 2", P_VOTER, P_VOTER_L)
_pcxl(slide, LK_RELEASED, "Node 0:\nCOMMITTED", "hello:world", hi_lk=True)
_switch(slide)
_parr(slide, COL_X[0]+NW/2-0.3, NY+NH, LK_X+1.5, CXL_Y, P_COMP, Pt(2))
_ptxt(slide, 0.5, 3.0, 4.5, 0.7,
      "LFM release (~2 CACHELINE ops):\ny:=0, b[0]:=0\nLock free for next writer",
      9, P_COMP, bold=True)
_legend(slide)

# ─── STEP 7: Background apply on other nodes ───
slide = _add_slide("Step 7: Background — Voter nodes apply update to local DRAM", P_OK)
_pnode(slide, 0, "Node 0 (Writer)", P_COMP, P_COMP_L,
       idx_text="Slot X = A", kv_text="hello:world")
_pnode(slide, 1, "Node 1", P_VOTER, P_VOTER_L,
       idx_text="Slot X = A", kv_text="hello:world", hi_idx=True, hi_kv=True)
_pnode(slide, 2, "Node 2", P_VOTER, P_VOTER_L,
       idx_text="Slot X = A", kv_text="hello:world", hi_idx=True, hi_kv=True)
_pcxl(slide, LK_RELEASED, "Node 0:\nCOMMITTED", "hello:world")
_switch(slide)
# arrows from CXL to nodes 1 and 2
for col in [1, 2]:
    _parr(slide, LK_X+LK_W/2, CXL_Y, COL_X[col]+NW/2-0.5, NY+NH,
          P_OK, Pt(1.5), dashed=True)
    _parr(slide, ST_X+ST_W/2, CXL_Y, COL_X[col]+NW/2+0.5, NY+NH,
          P_OK, Pt(1.5), dashed=True)
_ptxt(slide, 0.5, 2.8, 4.5, 1.0,
      "Replication fiber on each node:\n"
      "1. Periodically scans SlotLock table\n"
      "2. Sees value changed -> updates local Index\n"
      "3. Copies KV from CXL Staging -> local DRAM",
      9, P_OK, bold=True)
# final state box
_pbox(slide, 0.5, CXL_Y+CXL_H+0.2, 12.3, 0.4,
      RGBColor(0xE8, 0xF5, 0xE9), P_OK, Pt(1.5),
      "Final: SlotLock value = A | All 3 nodes have Slot X = A | KV replicated | OpLog committed",
      9, P_OK)
_legend(slide)

# ─── Comparison summary slide ───
slide = _add_slide("Old vs New Design Comparison", P_DG)
cols = [("", 2.5), ("Old: Propose-Vote-Commit", 4.7), ("New: Per-Slot LFM", 4.7)]
x = 0.4
for label, w in cols:
    _pbox(slide, x, 1.2, w, 0.45, P_CXL_L, P_CXL, Pt(1.5), label, 10, P_CXL)
    x += w + 0.15

rows = [
    ("Steps", "7 (incl. voter polling)", "6 (no polling)"),
    ("CXL ops\n(uncontended)", "~10 + voter wait", "~9, no wait"),
    ("Latency\nestimate", "15-50 us\n(voter polling bottleneck)", "5-10 us\n(LFM fast path)"),
    ("Conflict\nresolution", "ABORT + retry whole flow", "Spin-wait on lock\n(automatic queue)"),
    ("Race in\nclaim phase", "Yes (two nodes can both 'win')", "Impossible\n(LFM is mutex)"),
]
for i, (n, v1, v2) in enumerate(rows):
    y = 1.85 + i * 1.0
    x = 0.4
    _pbox(slide, x, y, 2.5, 0.85, P_IDX, P_GRAY, Pt(0.8), n, 8, P_DG)
    x += 2.65
    _pbox(slide, x, y, 4.7, 0.85, P_WHITE, P_GRAY, Pt(0.8), v1, 9, P_DG, bold=False)
    x += 4.85
    _pbox(slide, x, y, 4.7, 0.85, RGBColor(0xE8,0xF5,0xE9), P_OK, Pt(0.8), v2, 9, P_OK, bold=False)

# ── Save PPTX ──
out_pptx = "docs/cxl_lfm_consensus_steps.pptx"
prs.save(out_pptx)
print(f"Saved {out_pptx}")


# ════════════════════════════════════════════════
#  PNG generation (matplotlib, one PNG per step)
# ════════════════════════════════════════════════
def _mbox(ax, x, y, w, h, fc, ec="white", lw=1.5):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.08",
                 facecolor=fc, edgecolor=ec, lw=lw, zorder=3))

def _mregion(ax, x, y, w, h, fc, title, sub="", hi=False):
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

def _mlbl(ax, x, y, t, fs=9, c="black", bold=True, ha="center"):
    ax.text(x, y, t, ha=ha, va="center", fontsize=fs, color=c,
            weight="bold" if bold else "normal", zorder=8)

def _marr(ax, x0, y0, x1, y1, c, lw=2, rad=0, dashed=False):
    ax.add_patch(FancyArrowPatch(
        (x0, y0), (x1, y1),
        arrowstyle="->,head_length=0.35,head_width=0.2",
        connectionstyle=f"arc3,rad={rad}", color=c, lw=lw,
        linestyle="--" if dashed else "-", mutation_scale=12, zorder=7))

def _step_badge(ax, n, bg=None):
    bg = bg or C["comp"]
    ax.add_patch(plt.Circle((0.6, 8.6), 0.35, fc=bg, ec="white", lw=2, zorder=9))
    ax.text(0.6, 8.6, str(n), ha="center", va="center",
            fontsize=14, color="white", weight="bold", zorder=10)


def draw_step(step_num, title, title_color=C["comp"],
              # node states
              idx_states=("Slot X = 0x0",)*3,
              kv_states=("(empty)",)*3,
              hi_idx=(False, False, False),
              hi_kv=(False, False, False),
              # cxl states
              lock_state="FREE",
              value_state="0x0",
              ol_state="(empty)",
              st_state="(empty)",
              hi_lk=False, hi_ol=False, hi_st=False,
              # arrows: list of (x0, y0, x1, y1, color, dashed)
              arrows=None,
              # description text
              desc="",
              show_commit_line=False):
    fig, ax = plt.subplots(figsize=(15, 9))
    ax.set_xlim(-0.3, 15.3)
    ax.set_ylim(-0.3, 9.2)
    ax.set_aspect("equal")
    ax.axis("off")

    _mlbl(ax, 7.5, 8.6, f"Step {step_num}: {title}", 12, title_color)
    _step_badge(ax, step_num, title_color)

    # ── 3 nodes (top) ──
    nw, nh_c = 4.0, 1.8
    cy_n = 5.5
    col_x_m = [0.5, 5.5, 10.5]
    titles = [("Node 0 (Writer)", C["comp"], C["comp_l"]),
              ("Node 1", C["voter"], C["voter_l"]),
              ("Node 2", C["voter"], C["voter_l"])]
    for i, (cx, (t, nc, ncl)) in enumerate(zip(col_x_m, titles)):
        _mbox(ax, cx, cy_n, nw, nh_c, ncl, nc, 2)
        _mlbl(ax, cx + nw/2, cy_n + nh_c - 0.2, t, 10, nc)
        _mregion(ax, cx+0.1, cy_n+0.55, 1.8, 0.55, C["idx"],
                 "Local Index", idx_states[i], hi=hi_idx[i])
        _mregion(ax, cx+2.0, cy_n+0.55, 1.8, 0.55, C["data"],
                 "Local KV", kv_states[i], hi=hi_kv[i])
        _mregion(ax, cx+0.1, cy_n+0.1, 3.8, 0.4, C["lgray"],
                 "Replication Fiber")

    # ── CXL Memory (bottom) ──
    cx_, cy_ = 0.3, 0.5
    cxw, cxh = 14.7, 3.5
    _mbox(ax, cx_, cy_, cxw, cxh, C["cxl_l"], C["cxl"], 2.5)
    _mlbl(ax, cx_ + cxw/2, cy_ + cxh - 0.25, "CXL Type 3 Shared Memory",
          11, C["cxl"])

    # SlotLock Table
    lk_x, lk_y, lk_w, lk_h = cx_+0.3, cy_+0.4, 5.5, 2.4
    ec = C["hi"] if hi_lk else C["gray"]
    lw = 2.5 if hi_lk else 0.7
    ax.add_patch(FancyBboxPatch((lk_x, lk_y), lk_w, lk_h,
                 boxstyle="round,pad=0.05", facecolor=C["lock"],
                 edgecolor=ec, lw=lw, zorder=4))
    _mlbl(ax, lk_x + lk_w/2, lk_y + lk_h - 0.2,
          "SlotLock Table (per-hash-slot)", 9, C["cxl"])
    fields = [
        f"SlotLock[idx]:",
        f"  shm_mutex_t lock = {lock_state}",
        f"  uint64_t   value = {value_state}",
    ]
    for i, f in enumerate(fields):
        ax.text(lk_x+0.2, lk_y+lk_h-0.55-i*0.4, f,
                fontsize=8, color=C["ok"] if hi_lk else C["dg"],
                family="monospace", weight="bold" if hi_lk else "normal", zorder=5)

    # OpLog
    ol_x = cx_+6.1
    ec = C["hi"] if hi_ol else C["gray"]
    lw = 2.5 if hi_ol else 0.7
    ax.add_patch(FancyBboxPatch((ol_x, lk_y), 3.0, lk_h,
                 boxstyle="round,pad=0.05", facecolor=C["log"],
                 edgecolor=ec, lw=lw, zorder=4))
    _mlbl(ax, ol_x+1.5, lk_y+lk_h-0.2, "OpLog (per-node)", 9, C["cxl"])
    ax.text(ol_x+0.2, lk_y+lk_h-0.6, f"Node 0:",
            fontsize=8, color=C["dg"], family="monospace", zorder=5)
    ax.text(ol_x+0.2, lk_y+lk_h-0.95, f"  {ol_state}",
            fontsize=8, color=C["ok"] if hi_ol else C["dg"],
            family="monospace", weight="bold" if hi_ol else "normal", zorder=5)

    # Staging
    st_x = cx_+9.4
    ec = C["hi"] if hi_st else C["gray"]
    lw = 2.5 if hi_st else 0.7
    ax.add_patch(FancyBboxPatch((st_x, lk_y), 3.0, lk_h,
                 boxstyle="round,pad=0.05", facecolor=C["data"],
                 edgecolor=ec, lw=lw, zorder=4))
    _mlbl(ax, st_x+1.5, lk_y+lk_h-0.2, "Staging Buffer", 9, C["cxl"])
    ax.text(st_x+0.2, lk_y+lk_h-0.6, f"{st_state}",
            fontsize=8, color=C["ok"] if hi_st else C["dg"],
            family="monospace", weight="bold" if hi_st else "normal", zorder=5)

    # Alloc dir
    ad_x = cx_+12.6
    ax.add_patch(FancyBboxPatch((ad_x, lk_y), 1.8, lk_h,
                 boxstyle="round,pad=0.05", facecolor=C["lgray"],
                 edgecolor=C["gray"], lw=0.7, zorder=4))
    _mlbl(ax, ad_x+0.9, lk_y+lk_h/2, "Alloc\nDir", 8, C["gray"], bold=False)

    # CXL switch lines
    for cx_node in col_x_m:
        x_mid = cx_node + nw/2
        ax.plot([x_mid, cx_+cxw/2], [cy_n, cy_+cxh],
                ls=":", lw=0.9, color=C["gray"], zorder=1)
    _mlbl(ax, cx_+cxw/2, cy_+cxh+0.1, "CXL Switch", 8, C["gray"], bold=False)

    # arrows from spec
    if arrows:
        for x0, y0, x1, y1, col, dashed in arrows:
            _marr(ax, x0, y0, x1, y1, col, 2, dashed=dashed)

    # description
    if desc:
        ax.text(0.7, 4.6, desc, fontsize=9, color=C["dg"],
                ha="left", va="top", weight="bold",
                bbox=dict(boxstyle="round,pad=0.3", fc="#FFF8E1",
                          ec=C["commit"], lw=1))

    # commit point line
    if show_commit_line:
        ax.axhline(y=4.5, xmin=0.03, xmax=0.97, ls="-", lw=3,
                   color=C["commit"], alpha=0.4, zorder=1)
        _mlbl(ax, 12.5, 4.75, "COMMIT POINT", 11, C["commit"])

    # legend
    ax.legend(handles=[
        mpatches.Patch(fc=C["comp_l"], ec=C["comp"], lw=1.5, label="Writer"),
        mpatches.Patch(fc=C["voter_l"], ec=C["voter"], lw=1.5, label="Other Node"),
        mpatches.Patch(fc=C["cxl_l"], ec=C["cxl"], lw=1.5, label="CXL Mem"),
        mpatches.Patch(fc=C["lock"], ec=C["gray"], lw=0.8, label="SlotLock"),
        mpatches.Patch(fc=C["idx"], ec=C["gray"], lw=0.8, label="Index"),
        mpatches.Patch(fc=C["log"], ec=C["gray"], lw=0.8, label="OpLog"),
        mpatches.Patch(fc=C["data"], ec=C["gray"], lw=0.8, label="KV"),
        mpatches.Patch(fc="white", ec=C["hi"], lw=2, label="Modified"),
    ], loc="lower center", fontsize=7.5, framealpha=0.9, ncol=8,
       bbox_to_anchor=(0.5, -0.05))

    fig.tight_layout()
    fname = f"docs/cxl_lfm_step{step_num}.png"
    fig.savefig(fname, dpi=200, bbox_inches="tight")
    print(f"  Saved {fname}")
    plt.close(fig)


# ── Generate PNG steps ──
print("\nGenerating PNG steps...")

# Step 1
draw_step(1, "Hash key, locate SlotLock[idx] (local computation)",
          desc="Node 0: idx = hash('hello') % NUM_SLOTS\nLocal computation, no CXL access")

# Step 2: Lock acquire
draw_step(2, "shm_mutex_lock(&SlotLock[idx].lock)  [LFM acquire]",
          lock_state="HELD by Node 0",
          hi_lk=True,
          arrows=[
              (0.5+4.0/2, 5.5, 0.3+5.5/2+0.3, 0.5+3.5, C["comp"], False),
              (0.3+5.5/2-0.3, 0.5+3.5, 0.5+4.0/2, 5.5, C["comp"], True),
          ],
          desc="LFM fast path (~3 CACHELINE ops):\nb[0]:=1 -> x:=0 -> y:=0 -> read x -> LOCKED")

# Step 3: Read & verify
draw_step(3, "Read SlotLock[idx].value, verify == 0x0",
          lock_state="HELD by Node 0",
          hi_lk=True,
          arrows=[
              (0.3+5.5/2+0.5, 0.5+3.5, 0.5+4.0/2+0.3, 5.5, C["comp"], True),
          ],
          desc="Read CXL value -> 0x0 (matches expected)\nIf != 0x0: unlock & abort")

# Step 4: Write KV + OpLog
draw_step(4, "Write KV to local DRAM + CXL Staging + OpLog (IN_PROGRESS)",
          kv_states=("hello:world", "(empty)", "(empty)"),
          hi_kv=(True, False, False),
          lock_state="HELD by Node 0",
          ol_state="IN_PROGRESS",
          st_state="hello:world",
          hi_ol=True, hi_st=True,
          arrows=[
              (0.5+4.0/2+0.5, 5.5, 0.3+9.4+1.5, 0.5+3.5, C["comp"], False),
              (0.5+4.0/2-0.3, 5.5, 0.3+6.1+1.5, 0.5+3.5, C["comp"], False),
          ],
          desc="Write KV to own DRAM\n+ stage in CXL for replication\n+ OpLog := IN_PROGRESS (crash recovery)")

# Step 5: COMMIT POINT
draw_step(5, "Write SlotLock[idx].value = A  ==  COMMIT POINT",
          C["commit"],
          idx_states=("Slot X = A", "Slot X = 0x0", "Slot X = 0x0"),
          kv_states=("hello:world", "(empty)", "(empty)"),
          hi_idx=(True, False, False),
          lock_state="HELD by Node 0",
          value_state="A",
          ol_state="COMMITTED",
          st_state="hello:world",
          hi_lk=True, hi_ol=True,
          arrows=[
              (0.5+4.0/2-0.5, 5.5, 0.3+5.5/2, 0.5+3.5, C["commit"], False),
              (0.5+4.0/2+0.5, 5.5, 0.3+6.1+1.5, 0.5+3.5, C["comp"], False),
          ],
          desc="1. SlotLock[idx].value := A (single source of truth)\n"
               "2. Update own local Index cache\n"
               "3. OpLog := COMMITTED",
          show_commit_line=True)

# Step 6: Unlock
draw_step(6, "shm_mutex_unlock(&SlotLock[idx].lock)",
          idx_states=("Slot X = A", "Slot X = 0x0", "Slot X = 0x0"),
          kv_states=("hello:world", "(empty)", "(empty)"),
          lock_state="FREE",
          value_state="A",
          ol_state="COMMITTED",
          st_state="hello:world",
          hi_lk=True,
          arrows=[
              (0.5+4.0/2-0.3, 5.5, 0.3+5.5/2+0.3, 0.5+3.5, C["comp"], False),
          ],
          desc="LFM release (~2 CACHELINE ops):\ny:=0, b[0]:=0\nLock free for next writer")

# Step 7: Apply on other nodes
draw_step(7, "Background: Other nodes apply update to local DRAM",
          C["ok"],
          idx_states=("Slot X = A", "Slot X = A", "Slot X = A"),
          kv_states=("hello:world", "hello:world", "hello:world"),
          hi_idx=(False, True, True),
          hi_kv=(False, True, True),
          lock_state="FREE",
          value_state="A",
          ol_state="COMMITTED",
          st_state="hello:world",
          arrows=[
              # CXL Lock -> Node 1 and Node 2
              (0.3+5.5/2+1.5, 0.5+3.5, 5.5+4.0/2, 5.5, C["ok"], True),
              (0.3+5.5/2+1.5, 0.5+3.5, 10.5+4.0/2, 5.5, C["ok"], True),
              # CXL Staging -> Node 1 and 2
              (0.3+9.4+1.5, 0.5+3.5, 5.5+4.0/2+0.5, 5.5, C["ok"], True),
              (0.3+9.4+1.5, 0.5+3.5, 10.5+4.0/2+0.5, 5.5, C["ok"], True),
          ],
          desc="Replication fiber on each node:\n"
               "1. Periodically scan SlotLock table\n"
               "2. See value changed -> update local Index\n"
               "3. Copy KV from CXL Staging -> local DRAM")

print("\nDone.")