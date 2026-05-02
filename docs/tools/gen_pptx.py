#!/usr/bin/env python3
"""
Generate a PPTX with RDMA consensus step-by-step slides.
Each slide = one step, compute nodes top, memory nodes bottom.
All drawn with python-pptx shapes (no images needed).
"""

from pptx import Presentation
from pptx.util import Inches, Pt, Emu
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN, MSO_ANCHOR
from pptx.enum.shapes import MSO_SHAPE

prs = Presentation()
prs.slide_width = Inches(13.33)
prs.slide_height = Inches(7.5)

# ── Colors ──
WHITE   = RGBColor(0xFF, 0xFF, 0xFF)
COMP    = RGBColor(0x15, 0x65, 0xC0)
COMP_L  = RGBColor(0xBB, 0xDE, 0xFB)
COMPB   = RGBColor(0xC6, 0x28, 0x28)
COMPB_L = RGBColor(0xFF, 0xCD, 0xD2)
MEM     = RGBColor(0x2E, 0x7D, 0x32)
MEM_L   = RGBColor(0xC8, 0xE6, 0xC9)
IDX     = RGBColor(0xFF, 0xF9, 0xC4)
DATA    = RGBColor(0xE3, 0xF2, 0xFD)
LOG     = RGBColor(0xF3, 0xE5, 0xF5)
GRAY    = RGBColor(0x9E, 0x9E, 0x9E)
DG      = RGBColor(0x33, 0x33, 0x33)
OK      = RGBColor(0x1B, 0x5E, 0x20)
FAIL    = RGBColor(0xB7, 0x1C, 0x1C)
COMMIT  = RGBColor(0xFF, 0x6F, 0x00)
HI      = RGBColor(0xFF, 0x8F, 0x00)
LGRAY   = RGBColor(0xEE, 0xEE, 0xEE)

def _in(v):
    return Inches(v)

def _add_box(slide, left, top, w, h, fill, border_color=None, border_w=Pt(1.5), text="",
             font_size=9, font_color=DG, bold=True, round_corners=True):
    shape = slide.shapes.add_shape(
        MSO_SHAPE.ROUNDED_RECTANGLE if round_corners else MSO_SHAPE.RECTANGLE,
        _in(left), _in(top), _in(w), _in(h))
    shape.fill.solid()
    shape.fill.fore_color.rgb = fill
    if border_color:
        shape.line.color.rgb = border_color
        shape.line.width = border_w
    else:
        shape.line.fill.background()
    if text:
        tf = shape.text_frame
        tf.word_wrap = True
        tf.auto_size = None
        p = tf.paragraphs[0]
        p.alignment = PP_ALIGN.CENTER
        run = p.add_run()
        run.text = text
        run.font.size = Pt(font_size)
        run.font.color.rgb = font_color
        run.font.bold = bold
    shape.text_frame.paragraphs[0].space_before = Pt(0)
    shape.text_frame.paragraphs[0].space_after = Pt(0)
    return shape

def _add_text(slide, left, top, w, h, text, font_size=9, color=DG, bold=False, align=PP_ALIGN.CENTER):
    txBox = slide.shapes.add_textbox(_in(left), _in(top), _in(w), _in(h))
    tf = txBox.text_frame
    tf.word_wrap = True
    p = tf.paragraphs[0]
    p.alignment = align
    run = p.add_run()
    run.text = text
    run.font.size = Pt(font_size)
    run.font.color.rgb = color
    run.font.bold = bold
    return txBox

def _add_arrow(slide, x0, y0, x1, y1, color=COMP, width=Pt(2), dashed=False):
    connector = slide.shapes.add_connector(
        1,  # straight connector
        _in(x0), _in(y0), _in(x1), _in(y1))
    connector.line.color.rgb = color
    connector.line.width = width
    if dashed:
        connector.line.dash_style = 2  # dash
    # arrowhead
    connector.begin_x = _in(x0)
    connector.begin_y = _in(y0)
    connector.end_x = _in(x1)
    connector.end_y = _in(y1)
    fmt = connector.line._ln  # access lxml element
    from pptx.oxml.ns import qn
    tail = fmt.makeelement(qn('a:tailEnd'), {})
    tail.set('type', 'triangle')
    tail.set('w', 'med')
    tail.set('len', 'med')
    fmt.append(tail)
    return connector

# ── Layout constants (inches) ──
COL_X = [0.8, 5.0, 9.2]  # 3 columns
NW, NH_C, NH_M = 3.3, 1.5, 2.3
CY = 1.2   # compute node top
MY = 4.5   # memory node top
RW, RH = 1.45, 0.55  # region size
IDX_DX, KV_DX = 0.1, 1.7
REG_Y_TOP = 0.55  # relative to node top
REG_Y_BOT = 0.1
LOG_W = NW - 0.2

def _draw_compute_a(slide):
    _add_box(slide, COL_X[0], CY, NW, NH_C, COMP_L, COMP, Pt(2),
             "Compute Node A", 11, COMP)
    _add_box(slide, COL_X[0]+0.1, CY+0.8, NW-0.2, 0.45, IDX, GRAY, Pt(0.8),
             "Root Cache", 8, DG)
    _add_box(slide, COL_X[0]+0.1, CY+0.35, NW-0.2, 0.38, WHITE, GRAY, Pt(0.8),
             "KV Buf: key=hello val=world", 7, GRAY)

def _draw_compute_b(slide):
    _add_box(slide, COL_X[1], CY, NW, NH_C, COMPB_L, COMPB, Pt(2),
             "Compute Node B", 11, COMPB)
    _add_box(slide, COL_X[1]+0.1, CY+0.35, NW-0.2, 0.38, WHITE, GRAY, Pt(0.8),
             "KV Buf: key=hello val=xyz", 7, GRAY)

def _draw_mn(slide, col, title, idx_text, kv_text, log_text,
             hi_idx=False, hi_kv=False, hi_log=False):
    mx = COL_X[col]
    _add_box(slide, mx, MY, NW, NH_M, MEM_L, MEM, Pt(2), title, 10, MEM)
    # Index
    bc = HI if hi_idx else GRAY
    bw = Pt(2.5) if hi_idx else Pt(0.8)
    _add_box(slide, mx+IDX_DX, MY+REG_Y_TOP, RW, RH, IDX, bc, bw,
             f"Index\n{idx_text}", 7, OK if hi_idx else DG, bold=hi_idx)
    # KV Data
    bc = HI if hi_kv else GRAY
    bw = Pt(2.5) if hi_kv else Pt(0.8)
    _add_box(slide, mx+KV_DX, MY+REG_Y_TOP, RW, RH, DATA, bc, bw,
             f"KV Data\n{kv_text}", 7, OK if hi_kv else DG, bold=hi_kv)
    # Log
    bc = HI if hi_log else GRAY
    bw = Pt(2.5) if hi_log else Pt(0.8)
    _add_box(slide, mx+REG_Y_BOT, MY+1.55, LOG_W, 0.5, LOG, bc, bw,
             f"Log: {log_text}", 7, OK if hi_log else DG, bold=hi_log)


def _add_slide(title_text, title_color=COMP):
    slide = prs.slides.add_slide(prs.slide_layouts[6])  # blank
    # title
    _add_text(slide, 0, 0.1, 13.33, 0.6, title_text, 18, title_color, bold=True)
    return slide

def _legend(slide):
    y = 7.0
    items = [("Compute", COMP_L, COMP), ("Memory", MEM_L, MEM),
             ("Index", IDX, GRAY), ("KV Data", DATA, GRAY),
             ("Log", LOG, GRAY), ("Modified", WHITE, HI)]
    for i, (lbl, fc, ec) in enumerate(items):
        x = 1.5 + i * 1.9
        _add_box(slide, x, y, 0.3, 0.2, fc, ec, Pt(1.5))
        _add_text(slide, x+0.35, y-0.02, 1.2, 0.25, lbl, 7, DG)


# ================================================================
#  STEP 1
# ================================================================
slide = _add_slide("Step 1: RDMA WRITE KV data to all replicas")
_draw_compute_a(slide)
_draw_compute_b(slide)
_draw_mn(slide, 0, "MN 0 (Primary)", "Slot X=0x0", "hello:world", "old_value=0", hi_kv=True)
_draw_mn(slide, 1, "MN 1 (Backup)",  "Slot X=0x0", "hello:world", "old_value=0", hi_kv=True)
_draw_mn(slide, 2, "MN 2 (Backup)",  "Slot X=0x0", "hello:world", "old_value=0", hi_kv=True)
# arrows from A to all MN KV Data
for col in [0, 1, 2]:
    _add_arrow(slide, COL_X[0]+NW/2, CY+NH_C, COL_X[col]+KV_DX+RW/2, MY, COMP)
_add_text(slide, 4.5, 3.2, 4, 0.4, "RDMA WRITE (KV data)", 12, COMP, bold=True)
_legend(slide)

# ================================================================
#  STEP 2
# ================================================================
slide = _add_slide("Step 2: RDMA READ hash bucket from Primary Index")
_draw_compute_a(slide)
_draw_compute_b(slide)
_draw_mn(slide, 0, "MN 0 (Primary)", "Slot X=0x0", "hello:world", "old_value=0")
_draw_mn(slide, 1, "MN 1 (Backup)",  "Slot X=0x0", "hello:world", "old_value=0")
_draw_mn(slide, 2, "MN 2 (Backup)",  "Slot X=0x0", "hello:world", "old_value=0")
_add_arrow(slide, COL_X[0]+IDX_DX+RW/2, CY+NH_C, COL_X[0]+IDX_DX+RW/2, MY, COMP)
_add_arrow(slide, COL_X[0]+IDX_DX+RW/2+0.2, MY, COL_X[0]+IDX_DX+RW/2+0.2, CY+NH_C, COMP, dashed=True)
_add_text(slide, 0.5, 3.2, 3.5, 0.6, "RDMA READ\nA learns: Slot X = 0x0", 10, COMP, bold=True)
_legend(slide)

# ================================================================
#  STEP 3
# ================================================================
slide = _add_slide("Step 3: RDMA CAS Backup Index  (A wins, B loses)")
_draw_compute_a(slide)
_draw_compute_b(slide)
_draw_mn(slide, 0, "MN 0 (Primary)", "Slot X=0x0", "hello:world", "old_value=0")
_draw_mn(slide, 1, "MN 1 (Backup)",  "Slot X = A", "hello:world", "old_value=0", hi_idx=True)
_draw_mn(slide, 2, "MN 2 (Backup)",  "Slot X = A", "hello:world", "old_value=0", hi_idx=True)
# A arrows
_add_arrow(slide, COL_X[0]+NW/2+0.3, CY+NH_C, COL_X[1]+IDX_DX+RW/2, MY, COMP, Pt(2.5))
_add_arrow(slide, COL_X[0]+NW/2+0.5, CY+NH_C, COL_X[2]+IDX_DX+RW/2, MY, COMP, Pt(2.5))
# B arrows
_add_arrow(slide, COL_X[1]+NW/2, CY+NH_C, COL_X[1]+IDX_DX+RW/2+0.3, MY, COMPB, Pt(1.5))
_add_arrow(slide, COL_X[1]+NW/2+0.3, CY+NH_C, COL_X[2]+IDX_DX+RW/2+0.3, MY, COMPB, Pt(1.5))
_add_text(slide, 3.0, 3.0, 3.5, 0.4, "A: CAS(Slot, 0x0->A)", 10, COMP, bold=True)
_add_text(slide, 7.5, 3.0, 3.5, 0.4, "B: CAS(Slot, 0x0->B)", 10, COMPB, bold=True)
_add_text(slide, 5.0, 3.5, 3, 0.35, "A: ret=0x0 (win)", 10, OK, bold=True)
_add_text(slide, 5.0, 3.85, 3, 0.35, "B: ret=A (lose)", 10, FAIL, bold=True)
_legend(slide)

# ================================================================
#  STEP 4
# ================================================================
slide = _add_slide("Step 4: Check CAS results (local computation)", DG)
_draw_compute_a(slide)
_draw_compute_b(slide)
_draw_mn(slide, 0, "MN 0 (Primary)", "Slot X=0x0", "hello:world", "old_value=0")
_draw_mn(slide, 1, "MN 1 (Backup)",  "Slot X = A", "hello:world", "old_value=0")
_draw_mn(slide, 2, "MN 2 (Backup)",  "Slot X = A", "hello:world", "old_value=0")
# result boxes
_add_box(slide, COL_X[0]-0.1, 3.1, 3.5, 0.55, RGBColor(0xE8, 0xF5, 0xE9), OK, Pt(2),
         "A: all backups agreed -> WIN_ALL", 10, OK)
_add_box(slide, COL_X[1]-0.1, 3.1, 3.5, 0.55, RGBColor(0xFF, 0xEB, 0xEE), FAIL, Pt(2),
         "B: lost CAS race -> FAIL (abort)", 10, FAIL)
_legend(slide)

# ================================================================
#  STEP 5
# ================================================================
slide = _add_slide("Step 5: RDMA WRITE commit log to all replicas")
_draw_compute_a(slide)
_draw_mn(slide, 0, "MN 0 (Primary)", "Slot X=0x0", "hello:world", "old_value=orig", hi_log=True)
_draw_mn(slide, 1, "MN 1 (Backup)",  "Slot X = A", "hello:world", "old_value=orig", hi_log=True)
_draw_mn(slide, 2, "MN 2 (Backup)",  "Slot X = A", "hello:world", "old_value=orig", hi_log=True)
for col in [0, 1, 2]:
    _add_arrow(slide, COL_X[0]+NW/2, CY+NH_C, COL_X[col]+NW/2, MY+NH_M-0.3, COMP)
_add_text(slide, 4.0, 3.2, 5, 0.5,
          "RDMA WRITE commit mark\nKVLogTail.old_value := original slot", 10, COMP, bold=True)
_legend(slide)

# ================================================================
#  STEP 6
# ================================================================
slide = _add_slide("Step 6: RDMA CAS Primary Index  ==  COMMIT POINT", COMMIT)
_draw_compute_a(slide)
_draw_mn(slide, 0, "MN 0 (Primary)", "Slot X = A", "hello:world", "old_value=orig", hi_idx=True)
_draw_mn(slide, 1, "MN 1 (Backup)",  "Slot X = A", "hello:world", "old_value=orig")
_draw_mn(slide, 2, "MN 2 (Backup)",  "Slot X = A", "hello:world", "old_value=orig")
_add_arrow(slide, COL_X[0]+IDX_DX+RW/2, CY+NH_C, COL_X[0]+IDX_DX+RW/2, MY, COMMIT, Pt(3))
_add_arrow(slide, COL_X[0]+IDX_DX+RW/2+0.2, MY,
           COL_X[0]+IDX_DX+RW/2+0.2, CY+NH_C, COMMIT, Pt(1.5), dashed=True)
_add_text(slide, 0.5, 3.1, 3.5, 0.4, "CAS(Slot, 0x0 -> A)", 12, COMMIT, bold=True)
_add_text(slide, 0.5, 3.5, 3.5, 0.35, "ret = 0x0 (success)", 10, OK, bold=True)
# commit point line
line = slide.shapes.add_connector(1, _in(4), _in(3.3), _in(12.5), _in(3.3))
line.line.color.rgb = COMMIT
line.line.width = Pt(3)
_add_text(slide, 10.5, 3.0, 2.5, 0.4, "COMMIT POINT", 14, COMMIT, bold=True)
# final state
_add_box(slide, 3.5, MY+NH_M+0.2, 9.0, 0.45,
         RGBColor(0xE8, 0xF5, 0xE9), OK, Pt(1.5),
         "Final: Primary Idx=A | Backup Idx=A | KV on all MNs | Log committed", 9, OK)
_legend(slide)

# ── Save ──
out = "docs/rdma_consensus_steps.pptx"
prs.save(out)
print(f"Saved {out}")
