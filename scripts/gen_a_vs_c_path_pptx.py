#!/usr/bin/env python3
"""Generate Protocol A vs Protocol C path-comparison pptx for supervisor present.

Output: docs/protocol_a_vs_c_path_comparison.pptx (7 slides)

Slides:
  1. Title
  2. Protocol A — Write path topology   (sequence diagram, 3 actors)
  3. Protocol C — Write path topology   (sequence diagram, 1 actor)
  4. Protocol A — Read path topology    (sequence diagram, 4 actors)
  5. Protocol C — Read path topology    (sequence diagram, 1 actor)
  6. Write path measured latency table  (T=64 workload-A)
  7. Read path measured latency table   (T=64 workload-A)

Data sources baked into this script (so the file is fully self-contained):
  - A: docs/path_decomp_iter11A_20260511_023247/per_cell/workloada_best/
       per_stage_decomp.md (healthy capture, FUSEE_PROBE=1 build)
  - C: docs/iters/latency_decomp_C_iter2_20260423_053919.md
       (per-slot LFM, iter-2 build)

Run from repo root:
  python3 scripts/gen_a_vs_c_path_pptx.py
"""

import os
from pptx import Presentation
from pptx.util import Inches, Pt
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN, MSO_ANCHOR
from pptx.enum.shapes import MSO_SHAPE, MSO_CONNECTOR
from pptx.oxml.ns import qn
from lxml import etree

# ──────────────────────────────────────────────────────────────────────
# Colours
# ──────────────────────────────────────────────────────────────────────
WHITE  = RGBColor(0xFF, 0xFF, 0xFF)
BLACK  = RGBColor(0x10, 0x10, 0x10)
GREY_D = RGBColor(0x33, 0x33, 0x33)
GREY   = RGBColor(0x9E, 0x9E, 0x9E)
GREY_L = RGBColor(0xEE, 0xEE, 0xEE)

# Protocol A palette
A_DEEP  = RGBColor(0x15, 0x4F, 0xC2)
A_LIGHT = RGBColor(0xC8, 0xDA, 0xFB)
A_TXT   = RGBColor(0x0D, 0x32, 0x7D)

# Protocol C palette
C_DEEP  = RGBColor(0xE6, 0x51, 0x00)
C_LIGHT = RGBColor(0xFF, 0xE0, 0xB2)
C_TXT   = RGBColor(0x8C, 0x32, 0x00)

# Highlights
LIN_BG   = RGBColor(0xFF, 0xF4, 0x9C)
LIN_EDGE = RGBColor(0xF9, 0xA8, 0x25)
BLK_BG   = RGBColor(0xFF, 0xCD, 0xD2)
BLK_EDGE = RGBColor(0xC6, 0x28, 0x28)

CXL_BG   = RGBColor(0xFF, 0xF3, 0xE0)
CXL_EDGE = RGBColor(0xE6, 0x51, 0x00)
DRAM_BG  = RGBColor(0xE3, 0xF2, 0xFD)
DRAM_EDGE= RGBColor(0x15, 0x65, 0xC0)


# ──────────────────────────────────────────────────────────────────────
# Slide / shape helpers
# ──────────────────────────────────────────────────────────────────────
def _setup_prs():
    prs = Presentation()
    prs.slide_width  = Inches(13.33)
    prs.slide_height = Inches(7.5)
    return prs


def _blank_slide(prs, title, subtitle=None, accent=GREY_D):
    s = prs.slides.add_slide(prs.slide_layouts[6])
    tb = s.shapes.add_textbox(Inches(0.3), Inches(0.10), Inches(12.8), Inches(0.55))
    tf = tb.text_frame
    tf.margin_top = Pt(0); tf.margin_bottom = Pt(0)
    p = tf.paragraphs[0]
    r = p.add_run()
    r.text = title
    r.font.size = Pt(22); r.font.bold = True; r.font.color.rgb = accent
    if subtitle:
        sb = s.shapes.add_textbox(Inches(0.3), Inches(0.62), Inches(12.8), Inches(0.32))
        sf = sb.text_frame
        sf.margin_top = Pt(0); sf.margin_bottom = Pt(0)
        p = sf.paragraphs[0]
        r = p.add_run()
        r.text = subtitle
        r.font.size = Pt(11); r.font.color.rgb = GREY; r.font.italic = True
    return s


def _rect(slide, l, t, w, h, fill, edge, text, fs=10, bold=False, txt_col=GREY_D,
          rounded=True, shadow=False):
    shape_type = MSO_SHAPE.ROUNDED_RECTANGLE if rounded else MSO_SHAPE.RECTANGLE
    shp = slide.shapes.add_shape(shape_type, Inches(l), Inches(t),
                                 Inches(w), Inches(h))
    shp.fill.solid(); shp.fill.fore_color.rgb = fill
    shp.line.color.rgb = edge; shp.line.width = Pt(1.25)
    if shadow:
        shp.shadow.inherit = False
    tf = shp.text_frame
    tf.margin_left = Pt(3); tf.margin_right = Pt(3)
    tf.margin_top = Pt(2); tf.margin_bottom = Pt(2)
    tf.word_wrap = True
    tf.vertical_anchor = MSO_ANCHOR.MIDDLE
    p = tf.paragraphs[0]
    p.alignment = PP_ALIGN.CENTER
    r = p.add_run()
    r.text = text
    r.font.size = Pt(fs); r.font.bold = bold; r.font.color.rgb = txt_col
    return shp


def _line(slide, x0, y0, x1, y1, colour, lw=1.25, dashed=False, arrow=False):
    """Plain line / arrow (no label). For dashed/dotted lifelines use dashed=True."""
    conn = slide.shapes.add_connector(MSO_CONNECTOR.STRAIGHT,
                                      Inches(x0), Inches(y0),
                                      Inches(x1), Inches(y1))
    conn.line.color.rgb = colour; conn.line.width = Pt(lw)
    line = conn.line._get_or_add_ln()
    if arrow:
        tail = line.find(qn('a:tailEnd'))
        if tail is None:
            tail = etree.SubElement(line, qn('a:tailEnd'))
        tail.set('type', 'triangle')
        tail.set('w', 'med'); tail.set('h', 'med')
    if dashed:
        prstDash = line.find(qn('a:prstDash'))
        if prstDash is None:
            prstDash = etree.SubElement(line, qn('a:prstDash'))
        prstDash.set('val', 'dash')
    return conn


def _step_circle(slide, cx, cy, num, colour, lin=False, blocking=False, diam=0.32):
    """Numbered step circle. lin=True paints it yellow; blocking=True red."""
    bg = LIN_BG if lin else (BLK_BG if blocking else WHITE)
    edge = LIN_EDGE if lin else (BLK_EDGE if blocking else colour)
    shp = slide.shapes.add_shape(MSO_SHAPE.OVAL,
                                 Inches(cx - diam / 2.0), Inches(cy - diam / 2.0),
                                 Inches(diam), Inches(diam))
    shp.fill.solid(); shp.fill.fore_color.rgb = bg
    shp.line.color.rgb = edge; shp.line.width = Pt(1.5)
    tf = shp.text_frame
    tf.margin_left = Pt(0); tf.margin_right = Pt(0)
    tf.margin_top = Pt(0); tf.margin_bottom = Pt(0)
    tf.vertical_anchor = MSO_ANCHOR.MIDDLE
    p = tf.paragraphs[0]; p.alignment = PP_ALIGN.CENTER
    r = p.add_run(); r.text = str(num)
    r.font.size = Pt(8); r.font.bold = True
    r.font.color.rgb = colour if not (lin or blocking) else GREY_D
    return shp


def _step_label(slide, x, y, w, text, colour, fs=9, bold=True, italic=False,
                align=PP_ALIGN.LEFT):
    tb = slide.shapes.add_textbox(Inches(x), Inches(y), Inches(w), Inches(0.24))
    tf = tb.text_frame
    tf.margin_left = Pt(2); tf.margin_right = Pt(2)
    tf.margin_top = Pt(0); tf.margin_bottom = Pt(0)
    tf.word_wrap = False
    p = tf.paragraphs[0]; p.alignment = align
    r = p.add_run(); r.text = text
    r.font.size = Pt(fs); r.font.bold = bold; r.font.italic = italic
    r.font.color.rgb = colour
    # White bg so it sits on the lifeline cleanly
    tb.fill.solid(); tb.fill.fore_color.rgb = WHITE
    tb.line.fill.background()


def _step_arrow(slide, num, y, from_x, to_x, label, colour,
                dashed=False, blocking=False, lin=False, label_above=True,
                label_dx=0.0):
    """Draw a labelled step arrow between two lifelines at vertical y.

    num: integer or string shown in the circle on the arrow midpoint.
    Set blocking=True for cross-host blocking spans (red marker).
    Set lin=True for linearization points (yellow marker)."""
    _line(slide, from_x, y, to_x, y, colour, lw=1.6, dashed=dashed, arrow=True)
    midx = (from_x + to_x) / 2.0
    _step_circle(slide, midx, y, num, colour, lin=lin, blocking=blocking)
    # Label slightly above (default) or below
    label_y = y - 0.28 if label_above else y + 0.10
    label_x = midx + label_dx - 1.6
    _step_label(slide, label_x, label_y, 3.2, label, colour, fs=8.5,
                bold=True)


def _self_step(slide, num, y, x, label, colour, blocking=False, lin=False,
               label_left=False, label_w=3.0):
    """A step that happens at one actor — circle + label.
    label_left places label to the LEFT of the circle (use on right-edge actors).
    """
    _step_circle(slide, x, y, num, colour, lin=lin, blocking=blocking)
    if label_left:
        _step_label(slide, x - label_w - 0.20, y - 0.10, label_w, label,
                    colour, fs=8.5, bold=True, align=PP_ALIGN.RIGHT)
    else:
        _step_label(slide, x + 0.20, y - 0.10, label_w, label,
                    colour, fs=8.5, bold=True)


def _block_span(slide, x, y0, y1, label=None):
    """Draw a red 'blocking' bar on a lifeline (from y0 to y1)."""
    w = 0.18
    bar = slide.shapes.add_shape(MSO_SHAPE.RECTANGLE,
                                 Inches(x - w/2.0), Inches(y0),
                                 Inches(w), Inches(y1 - y0))
    bar.fill.solid(); bar.fill.fore_color.rgb = BLK_BG
    bar.line.color.rgb = BLK_EDGE; bar.line.width = Pt(0.5)
    if label:
        _step_label(slide, x + 0.15, (y0 + y1) / 2.0 - 0.10, 1.6, label,
                    BLK_EDGE, fs=8, italic=True)


def _actor_header(slide, x, y_top, w, label, palette_deep, palette_light):
    _rect(slide, x - w/2.0, y_top, w, 0.45, palette_light, palette_deep,
          label, fs=10, bold=True, txt_col=palette_deep, rounded=True)


def _lifeline(slide, x, y_top, y_bot, colour):
    _line(slide, x, y_top, x, y_bot, colour, lw=0.6, dashed=True)


def _legend_inline(slide, l, t, items):
    """Compact 1-row legend.  items = list of (label, colour) tuples."""
    box = slide.shapes.add_shape(MSO_SHAPE.ROUNDED_RECTANGLE,
                                 Inches(l), Inches(t), Inches(8.5), Inches(0.40))
    box.fill.solid(); box.fill.fore_color.rgb = WHITE
    box.line.color.rgb = GREY; box.line.width = Pt(0.6)
    x = l + 0.10
    for lbl, col in items:
        tb = slide.shapes.add_textbox(Inches(x), Inches(t + 0.04),
                                      Inches(2.0), Inches(0.32))
        tf = tb.text_frame
        tf.margin_left = Pt(2); tf.margin_right = Pt(2)
        tf.margin_top = Pt(0); tf.margin_bottom = Pt(0)
        p = tf.paragraphs[0]; p.alignment = PP_ALIGN.LEFT
        r = p.add_run()
        r.text = lbl
        r.font.size = Pt(9); r.font.bold = True; r.font.color.rgb = col
        x += 2.05


# ──────────────────────────────────────────────────────────────────────
# Slide 1 — Title
# ──────────────────────────────────────────────────────────────────────
def slide_title(prs):
    s = prs.slides.add_slide(prs.slide_layouts[6])
    tb = s.shapes.add_textbox(Inches(0.5), Inches(2.0), Inches(12.3), Inches(1.5))
    tf = tb.text_frame
    p = tf.paragraphs[0]; p.alignment = PP_ALIGN.CENTER
    r = p.add_run(); r.text = 'Protocol A vs Protocol C'
    r.font.size = Pt(44); r.font.bold = True; r.font.color.rgb = GREY_D

    sb = s.shapes.add_textbox(Inches(0.5), Inches(3.4), Inches(12.3), Inches(0.8))
    sf = sb.text_frame
    p = sf.paragraphs[0]; p.alignment = PP_ALIGN.CENTER
    r = p.add_run(); r.text = 'Read / Write Path Comparison'
    r.font.size = Pt(28); r.font.color.rgb = A_DEEP

    sb2 = s.shapes.add_textbox(Inches(0.5), Inches(4.3), Inches(12.3), Inches(0.6))
    sf2 = sb2.text_frame
    p = sf2.paragraphs[0]; p.alignment = PP_ALIGN.CENTER
    r = p.add_run()
    r.text = 'measured T=64 workload-A on g3+g4 testbed  •  iter-11A snapshot'
    r.font.size = Pt(14); r.font.color.rgb = GREY; r.font.italic = True

    sb3 = s.shapes.add_textbox(Inches(0.7), Inches(5.2), Inches(12.0), Inches(1.7))
    sf3 = sb3.text_frame
    sf3.word_wrap = True
    p = sf3.paragraphs[0]; p.alignment = PP_ALIGN.CENTER
    r = p.add_run()
    r.text = ('Thesis: strict-A linearizability (Protocol A) requires '
              'fundamentally more stages, more linearization points, '
              'and more cross-host coordination than LRC (Protocol C). '
              'The measured per-stage data confirms this is a structural '
              'property, not an engineering artefact.')
    r.font.size = Pt(14); r.font.color.rgb = GREY_D; r.font.italic = True


# ──────────────────────────────────────────────────────────────────────
# Slide 2 — Protocol A write path (sequence diagram)
# ──────────────────────────────────────────────────────────────────────
def slide_a_write(prs):
    s = _blank_slide(
        prs,
        'Protocol A — Write path: cross-host  update(K, V)  with peer cached',
        '14 numbered steps · 3 actors · 2 cross-host blocking points '
        '(A4 WriteRing spin, A9 InvalRing spin · 5 ms timeout cap each) · '
        'W9 = single commit point',
        accent=A_DEEP,
    )

    # ── Actor columns ────────────────────────────────────────────────
    # 3 actors + CXL header strip
    Y_HEADER = 1.30
    Y_TOP    = 1.85
    Y_BOT    = 6.55
    # x coordinates (centred in their column)
    X_W   = 1.60   # host 0 worker
    X_WR  = 6.00   # host 1 WriteReceiver
    X_IR  = 11.20  # host 0 InvalReceiver
    col_w = 2.8

    _actor_header(s, X_W,  Y_HEADER, col_w,
                  'host 0  worker (caller, cpu 0..T-1)',
                  DRAM_EDGE, DRAM_BG)
    _actor_header(s, X_WR, Y_HEADER, col_w,
                  'host 1  WriteReceiver (cpu 65)',
                  A_DEEP, A_LIGHT)
    _actor_header(s, X_IR, Y_HEADER, col_w,
                  'host 0  InvalReceiver (cpu 69)',
                  A_DEEP, A_LIGHT)

    # Lifelines (dashed vertical)
    for x in (X_W, X_WR, X_IR):
        _lifeline(s, x, Y_HEADER + 0.45, Y_BOT, GREY)

    # CXL annotations (left margin)
    cxl_note = s.shapes.add_textbox(Inches(0.20), Inches(1.85),
                                    Inches(0.95), Inches(4.5))
    tf = cxl_note.text_frame; tf.word_wrap = True
    tf.margin_left = Pt(2); tf.margin_right = Pt(2)
    tf.margin_top = Pt(0); tf.margin_bottom = Pt(0)
    p = tf.paragraphs[0]; p.alignment = PP_ALIGN.LEFT
    r = p.add_run(); r.text = 'CXL\nshared\nregion'
    r.font.size = Pt(8); r.font.italic = True; r.font.color.rgb = C_TXT
    r.font.bold = True

    # ── 14 steps ─────────────────────────────────────────────────────
    # Vertical pitch
    y = 2.10
    dy = 0.32

    # A1: worker → ForwardStaging (annotate "→ ForwardStaging[0][1][slot]")
    _self_step(s, 'A1', y, X_W, 'write V bytes → ForwardStaging[0][1][slot]',
               A_DEEP)
    y += dy

    # A2: worker → WriteRing.tail.fetch_add
    _self_step(s, 'A2', y, X_W, 'WriteRing[0][1].tail.fetch_add(1)  RMW-CXL',
               A_DEEP)
    y += dy

    # A3: worker → write WriteEntry + flush + publish req_op_id
    _self_step(s, 'A3', y, X_W,
               'write WriteEntry{key, op_kind, value_len, staging_off} + flush + sfence',
               A_DEEP)
    y += dy

    # A4: worker spin (cross-host BLOCKING) — draw block span
    y_a4_start = y
    _step_arrow(s, 'A4', y, X_W, X_WR, 'worker spin on resp_op_id  (5 ms timeout cap)',
                A_DEEP, blocking=True)
    y += dy

    # A5: WriteReceiver picks up new tail
    _self_step(s, 'A5', y, X_WR, 'poll WriteRing[0][1].tail; read ForwardStaging bytes',
               A_DEEP)
    y += dy

    # A6: WriteReceiver runs execute_write_local W1..W12 (call out invocation)
    _self_step(s, 'A6', y, X_WR,
               'execute_write_local: W1 hash+flush  · W2 spinlock+scan  · W3 bitmap',
               A_DEEP)
    y += dy

    # A7: WriteReceiver → InvalRing.tail.fetch_add (start invalidate sub-path)
    _self_step(s, 'A7', y, X_WR,
               'inside W4-W6: InvalRing[1][0].tail.fetch_add(1)  RMW-CXL',
               A_DEEP)
    y += dy

    # A8: write InvalEntry + flush
    _self_step(s, 'A8', y, X_WR, 'write InvalEntry{key} + flush + sfence', A_DEEP)
    y += dy

    # A9: WriteReceiver spin on inval resp_op_id (BLOCKING) — arrow to IR
    y_a9_start = y
    _step_arrow(s, 'A9', y, X_WR, X_IR,
                'WriteReceiver spin on InvalRing resp_op_id  (5 ms cap)',
                A_DEEP, blocking=True)
    y += dy

    # A10: InvalReceiver picks up new tail (label to left of column)
    _self_step(s, 'A10', y, X_IR, 'poll InvalRing tail; read InvalEntry',
               A_DEEP, label_left=True, label_w=2.6)
    y += dy

    # A11: InvalReceiver runs cache_pool_set_stale + bucket_epoch++ → ACK
    _self_step(s, 'A11', y, X_IR,
               'cache_pool_set_stale(K); bucket_epoch++ (seqlock CAS)',
               A_DEEP, lin=False, label_left=True, label_w=2.8)
    y += dy

    # A12: InvalReceiver → InvalRing resp_op_id (ACK back to WriteReceiver)
    _step_arrow(s, 'A12', y, X_IR, X_WR,
                'write InvalRing resp_op_id  (ACK)', A_DEEP, label_above=False)
    y += dy
    _block_span(s, X_WR, y_a9_start - 0.05, y - 0.02, label='blocked')

    # A13: WriteReceiver completes W7-W9-W10-W12 (★ W9 commit point ★)
    _self_step(s, 'A13', y, X_WR,
               'W7-W12: pool_alloc · pool_write · ★ W9 publish_slot_cow ★ · W10 dir + cache_pool_insert',
               A_DEEP, lin=True)
    y += dy

    # A14: WriteReceiver → WriteRing resp_op_id (ACK back to worker)
    _step_arrow(s, 'A14', y, X_WR, X_W,
                'write WriteRing resp_op_id  (ACK)  ⇒ worker A4 returns 0',
                A_DEEP, label_above=False)
    y += dy
    _block_span(s, X_W, y_a4_start - 0.05, y - 0.02, label='blocked')

    # Bottom legend + key takeaways
    _legend_inline(s, 0.3, 6.95,
                   [('— ▶  Step arrow', A_DEEP),
                    ('● Linearization (W9, A13)', LIN_EDGE),
                    ('● Cross-host blocking (A4, A9)', BLK_EDGE),
                    ('▌ blocked lifeline', BLK_EDGE)])
    tally_box = s.shapes.add_textbox(Inches(9.0), Inches(6.95), Inches(4.2),
                                     Inches(0.40))
    tf = tally_box.text_frame
    tf.margin_top = Pt(3)
    p = tf.paragraphs[0]; p.alignment = PP_ALIGN.RIGHT
    r = p.add_run(); r.text = '14 steps · 3 actors · 2 blocking points'
    r.font.size = Pt(11); r.font.bold = True; r.font.color.rgb = A_TXT


# ──────────────────────────────────────────────────────────────────────
# Slide 3 — Protocol C write path
# ──────────────────────────────────────────────────────────────────────
def slide_c_write(prs):
    s = _blank_slide(
        prs,
        'Protocol C — Write path: in-place under cross-host LFM lock',
        '6 numbered steps · 1 actor · 1 cross-host blocking (C1 LFM acquire) · '
        'C5 atomic write_epoch bump = single commit point',
        accent=C_DEEP,
    )

    # 1 actor (worker) + CXL state shown as boxes on the right
    Y_HEADER = 1.30
    Y_TOP    = 1.85
    Y_BOT    = 6.55
    X_W   = 2.4

    _actor_header(s, X_W, Y_HEADER, 3.2,
                  'host 0  worker (caller, cpu 0..T-1)',
                  DRAM_EDGE, DRAM_BG)
    _lifeline(s, X_W, Y_HEADER + 0.45, Y_BOT, GREY)

    # CXL state column on the right
    _rect(s, 6.5, Y_HEADER, 6.4, 0.45, CXL_BG, CXL_EDGE,
          'CXL state touched on the write path', fs=11, bold=True, txt_col=C_TXT)
    # BucketLockEntry (mutex + write_epoch)
    _rect(s, 6.7, 2.10, 6.0, 0.55, A_LIGHT, A_DEEP,
          'BucketLockEntry  •  shm_mutex_t (LFM) cross-host  •  write_epoch (cacheline_u64)',
          fs=10, bold=True, txt_col=A_TXT)
    # CxlKvBucket
    _rect(s, 6.7, 2.85, 6.0, 0.55, A_LIGHT, A_DEEP,
          'CxlKvBucket  •  7 × {key, value} = 112 B = 2 cachelines',
          fs=10, bold=True, txt_col=A_TXT)
    # KvBlockPool (optional)
    _rect(s, 6.7, 3.60, 6.0, 0.55, GREY_L, GREY,
          'KvBlockPool  (optional for value_len > 8 B)',
          fs=10, bold=False, txt_col=GREY_D)
    # OpLog (optional)
    _rect(s, 6.7, 4.35, 6.0, 0.55, GREY_L, GREY,
          'OpLog  (optional, recovery)',
          fs=10, bold=False, txt_col=GREY_D)

    # ── 6 steps ──────────────────────────────────────────────────────
    y = 2.40
    dy = 0.42

    # C1: LFM acquire (cross-host BLOCKING)
    _step_arrow(s, 'C1', y, X_W, 6.7,
                'lock_table_.lock(idx)   LFM acquire  (cross-host blocking spin)',
                C_DEEP, dashed=True, blocking=True)
    y += dy

    # C2 + C3: flush bucket + scan (combined to one step visual)
    _step_arrow(s, 'C2', y, X_W, 6.7,
                'flush_line(slots[0]); flush_line(slots[4]); mfence  (Phase-2.6 flush-collapse)',
                C_DEEP, dashed=True)
    y += dy
    _step_arrow(s, 'C3', y, X_W, 6.7,
                'scan 7 slots; pick target_slot for UPDATE / INSERT / DELETE',
                C_DEEP, dashed=True)
    y += dy

    # C4: publish (★ implicit commit happens with C5 atomic, but slot write is here)
    _step_arrow(s, 'C4', y, X_W, 6.7,
                'slot.value = V; flush; sfence    (or INSERT key+value pair publish)',
                C_DEEP, dashed=True)
    y += dy

    # C5: epoch bump (★ LINEARIZATION POINT ★)
    _step_arrow(s, 'C5', y, X_W, 6.7,
                '★ atomic_add_fetch(&write_epoch, 1) + flush + sfence ★  '
                '(1.4 µs CXL atomic = peer-visible commit)',
                C_DEEP, dashed=True, lin=True)
    y += dy

    # C6: unlock
    _step_arrow(s, 'C6', y, X_W, 6.7,
                'lock_table_.unlock()   LFM release  →  return 0',
                C_DEEP, dashed=True)
    y += dy

    # Key callout box bottom-right
    cb = s.shapes.add_shape(MSO_SHAPE.ROUNDED_RECTANGLE,
                            Inches(0.3), Inches(5.4),
                            Inches(5.7), Inches(1.4))
    cb.fill.solid(); cb.fill.fore_color.rgb = GREY_L
    cb.line.color.rgb = GREY; cb.line.width = Pt(0.6)
    tf = cb.text_frame
    tf.margin_left = Pt(8); tf.margin_right = Pt(8)
    tf.margin_top = Pt(5); tf.margin_bottom = Pt(5)
    tf.word_wrap = True
    p = tf.paragraphs[0]
    r = p.add_run(); r.text = 'Key features of Protocol C write path:'
    r.font.size = Pt(11); r.font.bold = True; r.font.color.rgb = C_TXT
    for txt in [
        '• 1 actor — no senders / receivers / dispatchers',
        '• 0 cross-host messages — coordination via shared CXL mutex + epoch',
        '• 1 linearization point — write_epoch atomic bump (C5)',
        '• Failure mode: hot-bucket LFM contention scales catastrophically with T',
    ]:
        p = tf.add_paragraph()
        r = p.add_run(); r.text = txt
        r.font.size = Pt(10); r.font.color.rgb = GREY_D

    # Legend + tally
    _legend_inline(s, 0.3, 6.95,
                   [('╌╌▶  Step arrow', C_DEEP),
                    ('● Linearization (C5 epoch)', LIN_EDGE),
                    ('● Cross-host blocking (C1 LFM)', BLK_EDGE),
                    ('—', WHITE)])
    tally_box = s.shapes.add_textbox(Inches(9.0), Inches(6.95), Inches(4.2),
                                     Inches(0.40))
    tf = tally_box.text_frame; tf.margin_top = Pt(3)
    p = tf.paragraphs[0]; p.alignment = PP_ALIGN.RIGHT
    r = p.add_run(); r.text = '6 steps · 1 actor · 1 blocking point'
    r.font.size = Pt(11); r.font.bold = True; r.font.color.rgb = C_TXT


# ──────────────────────────────────────────────────────────────────────
# Slide 4 — Protocol A read path
# ──────────────────────────────────────────────────────────────────────
def slide_a_read(prs):
    s = _blank_slide(
        prs,
        'Protocol A — Read path: TLS L1 → KvCachePool L2 → cross-host forwarder-pool-direct',
        '16 numbered steps · 4 actors · 1 cross-host blocking (A8r staging poll, 200 ms cap) · '
        '3 linearization checkpoints (TLS epoch, L2 seqlock seq, C13 lookup_epoch)',
        accent=A_DEEP,
    )

    Y_HEADER = 1.30
    Y_BOT    = 6.55
    col_w = 2.5

    X_W   = 1.40   # worker
    X_TLS = 4.05   # TLS L1 (per-worker private DRAM)
    X_L2  = 6.70   # L2 KvCachePool (host 0 shared DRAM)
    X_RR  = 9.85   # host 1 ReadReceiver
    X_RS  = 12.30  # ReadStaging (CXL)

    _actor_header(s, X_W,  Y_HEADER, col_w,
                  'host 0  worker (caller)', DRAM_EDGE, DRAM_BG)
    _actor_header(s, X_TLS, Y_HEADER, col_w,
                  'TLS L1\n(per-worker private DRAM)', DRAM_EDGE, DRAM_BG)
    _actor_header(s, X_L2,  Y_HEADER, col_w,
                  'L2 KvCachePool\n(host-shared DRAM, seqlock)',
                  DRAM_EDGE, DRAM_BG)
    _actor_header(s, X_RR,  Y_HEADER, col_w,
                  'host 1  ReadReceiver (cpu 67)', A_DEEP, A_LIGHT)
    # ReadStaging is a memory region not an actor — small narrow box
    _rect(s, X_RS - 0.9, Y_HEADER, 1.8, 0.45, CXL_BG, CXL_EDGE,
          'ReadStaging[0][1] (CXL)', fs=9, bold=True, txt_col=C_TXT)

    for x in (X_W, X_TLS, X_L2, X_RR, X_RS):
        _lifeline(s, x, Y_HEADER + 0.45, Y_BOT, GREY)

    # ── 16 steps ─────────────────────────────────────────────────────
    y = 1.95
    dy = 0.28

    _self_step(s, 'A1r', y, X_W, 'enter search(K)', A_DEEP)
    y += dy

    _step_arrow(s, 'A2r', y, X_W, X_TLS,
                'tls_lookup: read bucket_epoch (1 LD); compare observed_epoch',
                A_DEEP, lin=True)
    y += dy

    # If TLS hit → R0_tls_hit return (annotate)
    _self_step(s, 'R0', y, X_W,
               '★ on hit: ~80 ns total, return value to caller ★', A_DEEP, lin=True)
    y += dy

    _step_arrow(s, 'A3r', y, X_W, X_L2,
                'else cache_pool_lookup: seqlock CAS read of L2 entry',
                A_DEEP, lin=True)
    y += dy

    # If L2 hit → R2hit return
    _self_step(s, 'R2h', y, X_W,
               '★ on L2 hit: ~5-7 µs total (KV memcpy under MESI), return ★',
               A_DEEP)
    y += dy

    # L2 miss → R3 forward_read_direct
    _step_arrow(s, 'A4r', y, X_W, X_L2,
                'on L2 miss: re-load bucket_epoch  ← my_epoch_at_send (C13 tag)',
                A_DEEP, label_above=False)
    y += dy

    _step_arrow(s, 'A5r', y, X_W, X_RS,
                'ReadStaging[0][1][slot].ready_op_id = 0; flush; sfence',
                A_DEEP)
    y += dy

    _step_arrow(s, 'A6r', y, X_W, X_RR,
                'ReadRing[0][1].tail.fetch_add(1)  RMW-CXL', A_DEEP)
    y += dy

    _step_arrow(s, 'A7r', y, X_W, X_RR,
                'write ReadEntry{key, req_op_id}; flush; sfence',
                A_DEEP)
    y += dy

    # A8r: worker spin on staging.ready_op_id (CROSS-HOST BLOCKING, 200 ms cap)
    y_a8 = y
    _step_arrow(s, 'A8r', y, X_W, X_RS,
                'worker spin on ReadStaging.ready_op_id  (200 ms cap)',
                A_DEEP, blocking=True)
    y += dy

    # A9r..A14r ReadReceiver work
    _self_step(s, 'A9r', y, X_RR,
               'poll ReadRing tail; flush bucket; scan 7 slots', A_DEEP)
    y += dy

    _self_step(s, 'A10r', y, X_RR,
               'acquire SlotDirectoryEntry.spinlock; '
               'sharer_bitmap |= (1<<src)  ★ peer-visible state change ★',
               A_DEEP, lin=True)
    y += dy

    _self_step(s, 'A11r', y, X_RR,
               'pool->read(blk_off, 4) → value_len; pool->read(blk_off+4, vlen)',
               A_DEEP)
    y += dy

    _step_arrow(s, 'A12r', y, X_RR, X_RS,
                'write ReadStaging value_bytes + lookup_epoch (C13 tag) + status; flush',
                A_DEEP)
    y += dy

    _step_arrow(s, 'A13r', y, X_RR, X_RS,
                '★ publish staging.ready_op_id = req_op_id ★ (release-publish)',
                A_DEEP, lin=True, label_above=False)
    y += dy

    # A8r sees the ready_op_id (worker observes from staging) → A14r
    _step_arrow(s, 'A14r', y, X_RS, X_W,
                'worker A8r poll observes ready_op_id == req_op_id  → break spin',
                A_DEEP, label_above=False)
    y += dy
    _block_span(s, X_W, y_a8 - 0.05, y - 0.02)

    _self_step(s, 'A15r', y, X_W,
               'C13: if staging.lookup_epoch < my_epoch_at_send → return -3 (retry from L2)',
               A_DEEP, lin=True)
    y += dy

    _self_step(s, 'A16r', y, X_W,
               'memcpy value_bytes from staging; populate L2; populate TLS L1; return',
               A_DEEP)

    # Legend + tally
    _legend_inline(s, 0.3, 6.95,
                   [('—▶  Step arrow', A_DEEP),
                    ('● Linearization checkpoint', LIN_EDGE),
                    ('● Cross-host blocking', BLK_EDGE),
                    ('▌ blocked lifeline', BLK_EDGE)])
    tally_box = s.shapes.add_textbox(Inches(9.0), Inches(6.95), Inches(4.2),
                                     Inches(0.40))
    tf = tally_box.text_frame; tf.margin_top = Pt(3)
    p = tf.paragraphs[0]; p.alignment = PP_ALIGN.RIGHT
    r = p.add_run(); r.text = '16 steps · 4 actors · 1 blocking point'
    r.font.size = Pt(11); r.font.bold = True; r.font.color.rgb = A_TXT


# ──────────────────────────────────────────────────────────────────────
# Slide 5 — Protocol C read path
# ──────────────────────────────────────────────────────────────────────
def slide_c_read(prs):
    s = _blank_slide(
        prs,
        'Protocol C — Read path: 1 epoch load + 1 flush + scan  (LRC read-singleshot)',
        '4 numbered steps · 1 actor · 0 cross-host blocking · '
        'no seqlock revalidation (LRC: value is committed at SOME instant during scan)',
        accent=C_DEEP,
    )

    Y_HEADER = 1.30
    Y_BOT    = 6.55
    col_w = 3.2
    X_W   = 2.4

    _actor_header(s, X_W, Y_HEADER, col_w,
                  'host 0  worker (caller)', DRAM_EDGE, DRAM_BG)
    _lifeline(s, X_W, Y_HEADER + 0.45, Y_BOT, GREY)

    # CXL state
    _rect(s, 6.5, Y_HEADER, 6.4, 0.45, CXL_BG, CXL_EDGE,
          'CXL state read on the search path', fs=11, bold=True, txt_col=C_TXT)
    _rect(s, 6.7, 2.10, 6.0, 0.55, A_LIGHT, A_DEEP,
          'BucketLockEntry.write_epoch (cacheline_u64) — read seqlock tag',
          fs=10, bold=True, txt_col=A_TXT)
    _rect(s, 6.7, 2.85, 6.0, 0.55, A_LIGHT, A_DEEP,
          'CxlKvBucket  •  flush 2 cachelines, scan 7 slots in L1',
          fs=10, bold=True, txt_col=A_TXT)
    _rect(s, 6.7, 3.60, 6.0, 0.55, C_LIGHT, C_DEEP,
          'DRAM bucket cache (optional, per-process)  •  cache_buckets_[idx], cache_epoch_[idx]',
          fs=10, bold=True, txt_col=C_TXT)

    # ── 4 steps ──────────────────────────────────────────────────────
    y = 2.40
    dy = 0.55

    _self_step(s, 'C1r', y, X_W,
               'bucket_idx = fnv1a_u64(K) % B; entry = lock_table_.entry(idx)',
               C_DEEP)
    y += dy

    _step_arrow(s, 'C2r', y, X_W, 6.7,
                'load CXL write_epoch  (1 LD-CXL cacheline)  '
                '★ if == cached_epoch_[idx]: scan DRAM copy + return ★',
                C_DEEP, dashed=True, lin=True)
    y += dy

    _step_arrow(s, 'C3r', y, X_W, 6.7,
                'cache miss / cache disabled: flush_line(slots[0]) + flush_line(slots[4]) + mfence',
                C_DEEP, dashed=True)
    y += dy

    _self_step(s, 'C4r', y, X_W,
               'scan 7 slots in L1 (now fresh from CXL); return value or -1   '
               '★ NO post-scan revalidation (LRC) ★',
               C_DEEP, lin=True)
    y += dy

    # Key callout box
    cb = s.shapes.add_shape(MSO_SHAPE.ROUNDED_RECTANGLE,
                            Inches(0.3), Inches(5.4),
                            Inches(5.7), Inches(1.4))
    cb.fill.solid(); cb.fill.fore_color.rgb = GREY_L
    cb.line.color.rgb = GREY; cb.line.width = Pt(0.6)
    tf = cb.text_frame
    tf.margin_left = Pt(8); tf.margin_right = Pt(8)
    tf.margin_top = Pt(5); tf.margin_bottom = Pt(5)
    tf.word_wrap = True
    p = tf.paragraphs[0]
    r = p.add_run(); r.text = 'Key features of Protocol C read path:'
    r.font.size = Pt(11); r.font.bold = True; r.font.color.rgb = C_TXT
    for txt in [
        '• Reader takes NO lock — bucket lock only serializes writers',
        '• Optional DRAM cache hit path: ~700 ns (1 CXL epoch load + DRAM scan)',
        '• Cache-miss path: ~1 µs (2 flushes + mfence + L1 scan)',
        '• LRC semantics: returned value committed at SOME instant during scan',
    ]:
        p = tf.add_paragraph()
        r = p.add_run(); r.text = txt
        r.font.size = Pt(10); r.font.color.rgb = GREY_D

    _legend_inline(s, 0.3, 6.95,
                   [('╌╌▶  Step arrow', C_DEEP),
                    ('● Linearization checkpoint', LIN_EDGE),
                    ('● Cross-host blocking (none!)', BLK_EDGE),
                    ('—', WHITE)])
    tally_box = s.shapes.add_textbox(Inches(9.0), Inches(6.95), Inches(4.2),
                                     Inches(0.40))
    tf = tally_box.text_frame; tf.margin_top = Pt(3)
    p = tf.paragraphs[0]; p.alignment = PP_ALIGN.RIGHT
    r = p.add_run(); r.text = '4 steps · 1 actor · 0 blocking points'
    r.font.size = Pt(11); r.font.bold = True; r.font.color.rgb = C_TXT


# ──────────────────────────────────────────────────────────────────────
# Latency table data
# ──────────────────────────────────────────────────────────────────────

A_WRITE_ROWS = [
    ('W1',  'Entry + bucket flush + mfence',                 '0.87',  '4.50',  '43.6',  ''),
    ('W2',  'Bucket scan + DRAM spinlock acquire',           '0.15',  '0.27',  '1.89',  ''),
    ('W3',  'Re-flush + read sharer_bitmap',                 '0.03',  '0.19',  '0.69',  ''),
    ('W4',  'Enter invalidate broadcast loop',               '0.76',  '0.83',  '1.10',  'per remote sharer'),
    ('I1',  'InvalRing.tail.fetch_add (RMW-CXL)',            '1.69',  '1.65',  '1.87',  'cross-host'),
    ('I2',  'Write InvalEntry + flush + sfence',             '3.45',  '3.70',  '5.29',  'cross-host'),
    ('I3',  '(peer) Dispatcher sees new tail',               '0.99',  '1.12',  '1.73',  ''),
    ('I4',  '(peer) Read entry',                             '0.58',  '0.63',  '1.33',  ''),
    ('I5',  '(peer) cache_pool_set_stale (DRAM CAS)',        '0.03',  '0.03',  '0.05',  ''),
    ('I7',  '(producer) Observes ACK',                       '0.03',  '0.04',  '0.06',  ''),
    ('I8',  'Free InvalRing slot',                           '0.05',  '0.65',  '6.38',  ''),
    ('W6',  'End of inval broadcast',                        '1.37',  '1.22',  '1.77',  'per remote sharer'),
    ('W7',  'pool->alloc (RMW-CXL bump cursor)',             '0.04',  '0.11',  '1.96',  ''),
    ('W8',  'pool->write (memcpy + per-CL flush)',           '0.03',  '0.16',  '1.59',  '8 cachelines KV=512'),
    ('W9',  'COMMIT POINT: publish_slot_cow',                '0.02',  '0.05',  '0.04',  '★ linearization #1 ★'),
    ('W10', 'directory + cache_pool_insert + epoch++',       '3.56',  '6.22',  '18.1',  '★ DOMINANT ★ + lin. #2'),
    ('W12', 'Return',                                         '0.34',  '1.50',  '9.05',  ''),
]

C_WRITE_ROWS = [
    ('lock',    'LFM acquire (CROSS-HOST blocking)',          '9.0',   '25.0',  '427.1', '★ DOMINANT ★ 84% of total'),
    ('scan',    'flush 2 CL + mfence + scan 7 slots',         '~1.4',  '1.40',  '—',     'Phase-2.6 flush-collapse'),
    ('publish', 'slot.value = V + flush + sfence',            '~0.02', '0.018', '—',     ''),
    ('epoch',   '__atomic_add_fetch(write_epoch) + flush',    '~4.0',  '4.07',  '—',     '★ linearization #1 ★ CXL atomic'),
    ('unlock',  'LFM release',                                '~0.03', '0.026', '—',     ''),
    ('TOTAL',   'end-to-end UPDATE',                          '13.4',  '30.6',  '439.6', ''),
]


A_READ_ROWS = [
    ('R0_tls_hit', 'TLS L1 hit return (when fresh)',          '0.025', '0.079', '0.81',  '★ 80 ns ideal ★ (~18% of reads)'),
    ('R1',         'Entry + bucket_epoch.load',               '7.07',  '5.26',  '16.5',  'high mean from shared-CL MESI'),
    ('R2hit',      'L2 cache_pool seqlock CAS read hit',      '0.079', '0.149', '0.42',  '~64% of reads'),
    ('R2miss',     'L2 miss',                                 '2.80',  '3.07',  '8.12',  '~17% of reads'),
    ('R3',         'forward_read_direct (CROSS-HOST)',        '13.6',  '14.0',  '26.4',  '★ DOMINANT ★ (~0.2% but tail)'),
    ('R4',         'C13 validate + memcpy from staging',      '0.34',  '0.39',  '1.00',  ''),
    ('R6',         'Return',                                  '0.73',  '1.14',  '5.82',  ''),
]

C_READ_ROWS = [
    ('C1r',                  'hash(K) → bucket_idx',                  '~0.015', '0.015', '—',  'inferred from primitives'),
    ('C2r (cache hit)',      'CACHELINE_LOAD write_epoch + DRAM scan', '~0.70',  '0.70',  '—',  'opt DRAM cache hit path'),
    ('C3r (cache miss)',     'flush 2 CL + mfence',                   '~0.15',  '0.15',  '—',  '2 flush_line@66ns + mfence'),
    ('C4r',                  'scan 7 slots in L1 (now fresh)',        '~0.05',  '0.05',  '—',  ''),
    ('TOTAL (cache hit)',    'best case end-to-end',                  '~0.70',  '0.70',  '—',  ''),
    ('TOTAL (cache miss)',   'flush + scan path',                     '~1.0',   '1.0',   '—',  ''),
]


def _make_table(slide, rows, headers, left, top, width, height,
                col_widths, accent, dominant_idx=None, lin_idx=None,
                header_fs=10, body_fs=9):
    nrows = len(rows) + 1
    ncols = len(headers)
    tbl_shape = slide.shapes.add_table(nrows, ncols,
                                       Inches(left), Inches(top),
                                       Inches(width), Inches(height))
    tbl = tbl_shape.table
    for i, w in enumerate(col_widths):
        tbl.columns[i].width = Inches(w)
    for j, h in enumerate(headers):
        cell = tbl.cell(0, j)
        cell.fill.solid(); cell.fill.fore_color.rgb = accent
        tf = cell.text_frame
        tf.margin_left = Pt(3); tf.margin_right = Pt(3)
        tf.margin_top = Pt(2); tf.margin_bottom = Pt(2)
        p = tf.paragraphs[0]; p.alignment = PP_ALIGN.CENTER
        r = p.add_run()
        r.text = h
        r.font.size = Pt(header_fs); r.font.bold = True
        r.font.color.rgb = WHITE
    dominant_idx = dominant_idx or set()
    lin_idx = lin_idx or set()
    for i, row in enumerate(rows, start=1):
        for j, val in enumerate(row):
            cell = tbl.cell(i, j)
            if (i - 1) in dominant_idx:
                cell.fill.solid(); cell.fill.fore_color.rgb = BLK_BG
            elif (i - 1) in lin_idx:
                cell.fill.solid(); cell.fill.fore_color.rgb = LIN_BG
            else:
                cell.fill.solid(); cell.fill.fore_color.rgb = WHITE
            tf = cell.text_frame
            tf.margin_left = Pt(3); tf.margin_right = Pt(3)
            tf.margin_top = Pt(1); tf.margin_bottom = Pt(1)
            tf.word_wrap = True
            p = tf.paragraphs[0]
            if j == 0 or j == 1:
                p.alignment = PP_ALIGN.LEFT
            elif 2 <= j <= 4:
                p.alignment = PP_ALIGN.RIGHT
            else:
                p.alignment = PP_ALIGN.LEFT
            r = p.add_run()
            r.text = val
            r.font.size = Pt(body_fs)
            if j == 0:
                r.font.bold = True
                r.font.color.rgb = accent
            else:
                r.font.color.rgb = GREY_D
            if (i - 1) in dominant_idx and j != 0:
                r.font.bold = True


def slide_write_latency_table(prs):
    s = _blank_slide(
        prs,
        'Write path — measured per-stage latency (T=64 workload-A)',
        'Source: A from iter-11A Phase 5 FUSEE_PROBE=1 capture; C from latency_decomp_C iter-2 per-slot LFM build  '
        '•  Yellow = linearization point  •  Red = dominant cost stage'
    )
    header = ['Stage', 'What it does', 'p50 µs', 'mean µs', 'p99 µs', 'Note']
    col_w_a = [0.55, 2.5, 0.55, 0.65, 0.55, 1.6]
    _make_table(s, A_WRITE_ROWS, header, left=0.3, top=1.2,
                width=6.4, height=4.3,
                col_widths=col_w_a, accent=A_DEEP,
                dominant_idx={15}, lin_idx={14},
                header_fs=10, body_fs=8)

    col_w_c = [0.65, 2.3, 0.55, 0.55, 0.55, 1.7]
    _make_table(s, C_WRITE_ROWS, header, left=6.9, top=1.2,
                width=6.3, height=2.0,
                col_widths=col_w_c, accent=C_DEEP,
                dominant_idx={0}, lin_idx={3},
                header_fs=10, body_fs=9)

    lbl_a = s.shapes.add_textbox(Inches(0.3), Inches(1.0), Inches(6.4), Inches(0.25))
    p = lbl_a.text_frame.paragraphs[0]; p.alignment = PP_ALIGN.LEFT
    r = p.add_run(); r.text = 'Protocol A — 17 distinct stages (W1-W12 + I1-I8 sub-path)'
    r.font.size = Pt(12); r.font.bold = True; r.font.color.rgb = A_TXT
    lbl_c = s.shapes.add_textbox(Inches(6.9), Inches(1.0), Inches(6.1), Inches(0.25))
    p = lbl_c.text_frame.paragraphs[0]; p.alignment = PP_ALIGN.LEFT
    r = p.add_run(); r.text = 'Protocol C — 6 stages total (Lock/Scan/Publish/Epoch/Unlock + Total)'
    r.font.size = Pt(12); r.font.bold = True; r.font.color.rgb = C_TXT

    sm = s.shapes.add_shape(MSO_SHAPE.ROUNDED_RECTANGLE,
                            Inches(6.9), Inches(3.4),
                            Inches(6.1), Inches(2.2))
    sm.fill.solid(); sm.fill.fore_color.rgb = GREY_L
    sm.line.color.rgb = GREY; sm.line.width = Pt(0.75)
    tf = sm.text_frame
    tf.margin_left = Pt(8); tf.margin_right = Pt(8)
    tf.margin_top = Pt(5); tf.margin_bottom = Pt(5)
    tf.word_wrap = True
    p = tf.paragraphs[0]
    r = p.add_run(); r.text = 'Mean per-op cost'
    r.font.size = Pt(11); r.font.bold = True; r.font.color.rgb = GREY_D
    for txt, col in [
        ('  Protocol A (no remote sharer):    ~ 13 µs/op', A_TXT),
        ('  Protocol A (with remote sharer):  ~ 19 µs/op  (+ inval path I1-I8)', A_TXT),
        ('  Protocol C (always):              30.6 µs/op  (84% in lock alone)', C_TXT),
        ('', GREY_D),
        ('Dominant stage shape:', GREY_D),
        ('  A: spread across 12 stages (W10 = 6.2 µs largest, MESI on shared L2 insert)', GREY_D),
        ('  C: concentrated in 1 stage (LFM lock acquire under cross-host Zipf hot-bucket)', GREY_D),
    ]:
        p = tf.add_paragraph()
        r = p.add_run(); r.text = txt
        r.font.size = Pt(10 if not txt.startswith('Dominant') else 11); r.font.color.rgb = col
        if txt.startswith('Dominant'):
            r.font.bold = True


def slide_read_latency_table(prs):
    s = _blank_slide(
        prs,
        'Read path — measured per-stage latency (T=64 workload-A)',
        'A measured from iter-11A Phase 5; C inferred from primitives (no historical search-side decomp run for C)  '
        '•  Probabilities by outcome shown for A'
    )

    header = ['Stage', 'What it does', 'p50 µs', 'mean µs', 'p99 µs', 'Note']
    col_w_a = [0.85, 2.4, 0.55, 0.6, 0.55, 1.6]
    _make_table(s, A_READ_ROWS, header, left=0.3, top=1.2,
                width=6.55, height=2.2,
                col_widths=col_w_a, accent=A_DEEP,
                dominant_idx={4}, lin_idx={0, 2},
                header_fs=10, body_fs=9)

    col_w_c = [1.45, 1.95, 0.50, 0.55, 0.45, 1.45]
    _make_table(s, C_READ_ROWS, header, left=6.95, top=1.2,
                width=6.35, height=2.0,
                col_widths=col_w_c, accent=C_DEEP,
                dominant_idx=set(), lin_idx={1, 4},
                header_fs=10, body_fs=9)

    lbl = s.shapes.add_textbox(Inches(0.3), Inches(3.7), Inches(6.55), Inches(0.30))
    p = lbl.text_frame.paragraphs[0]; p.alignment = PP_ALIGN.LEFT
    r = p.add_run(); r.text = 'Protocol A read-path outcome distribution (T=64 wl-A)'
    r.font.size = Pt(12); r.font.bold = True; r.font.color.rgb = A_TXT

    outcomes_a = [
        ('TLS L1 hit',              '~18%',  '~0.08 µs', 0.18),
        ('L2 cache_pool hit',       '~64%',  '~7 µs',    0.64),
        ('Owner-self CXL fetch',    '~17%',  '~10 µs',   0.17),
        ('Cross-host miss (R3+R4)', '~0.2%', '~22 µs',   0.002),
    ]
    y = 4.10
    bar_x = 0.3
    for nm, prob, lat, frac in outcomes_a:
        tb = s.shapes.add_textbox(Inches(bar_x), Inches(y), Inches(1.8),
                                  Inches(0.28))
        p = tb.text_frame.paragraphs[0]
        r = p.add_run(); r.text = nm
        r.font.size = Pt(10); r.font.color.rgb = GREY_D; r.font.bold = True
        bg = s.shapes.add_shape(MSO_SHAPE.RECTANGLE,
                                Inches(bar_x + 1.85), Inches(y + 0.05),
                                Inches(2.5), Inches(0.18))
        bg.fill.solid(); bg.fill.fore_color.rgb = GREY_L
        bg.line.fill.background()
        w_in = max(0.04, 2.5 * (frac ** 0.5))
        fg = s.shapes.add_shape(MSO_SHAPE.RECTANGLE,
                                Inches(bar_x + 1.85), Inches(y + 0.05),
                                Inches(w_in), Inches(0.18))
        fg.fill.solid(); fg.fill.fore_color.rgb = A_DEEP
        fg.line.fill.background()
        tb = s.shapes.add_textbox(Inches(bar_x + 4.45), Inches(y), Inches(0.8),
                                  Inches(0.28))
        p = tb.text_frame.paragraphs[0]
        r = p.add_run(); r.text = prob
        r.font.size = Pt(9); r.font.color.rgb = GREY_D
        tb = s.shapes.add_textbox(Inches(bar_x + 5.25), Inches(y), Inches(1.4),
                                  Inches(0.28))
        p = tb.text_frame.paragraphs[0]
        r = p.add_run(); r.text = lat
        r.font.size = Pt(9); r.font.bold = True; r.font.color.rgb = A_TXT
        y += 0.30

    sm = s.shapes.add_shape(MSO_SHAPE.ROUNDED_RECTANGLE,
                            Inches(6.95), Inches(3.7),
                            Inches(6.05), Inches(3.4))
    sm.fill.solid(); sm.fill.fore_color.rgb = GREY_L
    sm.line.color.rgb = GREY; sm.line.width = Pt(0.75)
    tf = sm.text_frame
    tf.margin_left = Pt(8); tf.margin_right = Pt(8)
    tf.margin_top = Pt(5); tf.margin_bottom = Pt(5)
    tf.word_wrap = True
    p = tf.paragraphs[0]
    r = p.add_run(); r.text = 'Reader best-case'
    r.font.size = Pt(11); r.font.bold = True; r.font.color.rgb = GREY_D
    for txt, col, bold in [
        ('  A (TLS L1 hit):      ~ 80 ns', A_TXT, False),
        ('  C (DRAM cache hit):  ~ 700 ns', C_TXT, False),
        ('', GREY_D, False),
        ('Reader worst-case (cross-host miss)', GREY_D, True),
        ('  A: ~ 22 µs (R1+R2miss+R3+R4+R6)', A_TXT, False),
        ('  C: ~ 1 µs  (every read is a local flush+scan)', C_TXT, False),
        ('', GREY_D, False),
        ('Key insight', GREY_D, True),
        ('  Only TLS L1 hits beat C. Every other A path pays for L2 seqlock or '
         'cross-host coordination. C readers are near-CXL-physics regardless of '
         'contention.', GREY_D, False),
    ]:
        p = tf.add_paragraph()
        r = p.add_run(); r.text = txt
        r.font.size = Pt(11 if bold else 10)
        r.font.color.rgb = col
        r.font.bold = bold


# ──────────────────────────────────────────────────────────────────────
# Main
# ──────────────────────────────────────────────────────────────────────
def main():
    out = os.path.join(os.path.dirname(__file__), '..',
                       'docs', 'protocol_a_vs_c_path_comparison.pptx')
    out = os.path.abspath(out)
    prs = _setup_prs()
    slide_title(prs)
    slide_a_write(prs)
    slide_c_write(prs)
    slide_a_read(prs)
    slide_c_read(prs)
    slide_write_latency_table(prs)
    slide_read_latency_table(prs)
    prs.save(out)
    print('wrote', out)


if __name__ == '__main__':
    main()
