#!/usr/bin/env python3
"""Generate Protocol A vs Protocol C path-comparison pptx for supervisor present.

Output: docs/protocol_a_vs_c_path_comparison.pptx (5 slides)

Slides:
  1. Title
  2. Write path topology  — both protocols on one canvas, numbered arrows
  3. Read path topology   — both protocols on one canvas, numbered arrows
  4. Write path measured latency table (T=64 workload-A)
  5. Read path measured latency table (T=64 workload-A)

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
from pptx.util import Inches, Pt, Emu
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN, MSO_ANCHOR
from pptx.enum.shapes import MSO_SHAPE, MSO_CONNECTOR

# ──────────────────────────────────────────────────────────────────────
# Colours
# ──────────────────────────────────────────────────────────────────────
WHITE  = RGBColor(0xFF, 0xFF, 0xFF)
BLACK  = RGBColor(0x10, 0x10, 0x10)
GREY_D = RGBColor(0x33, 0x33, 0x33)
GREY   = RGBColor(0x9E, 0x9E, 0x9E)
GREY_L = RGBColor(0xEE, 0xEE, 0xEE)

# Protocol A palette (solid arrows + blue boxes)
A_DEEP  = RGBColor(0x15, 0x4F, 0xC2)
A_LIGHT = RGBColor(0xC8, 0xDA, 0xFB)
A_TXT   = RGBColor(0x0D, 0x32, 0x7D)

# Protocol C palette (dashed arrows + orange boxes)
C_DEEP  = RGBColor(0xE6, 0x51, 0x00)
C_LIGHT = RGBColor(0xFF, 0xE0, 0xB2)
C_TXT   = RGBColor(0x8C, 0x32, 0x00)

# Highlights
LIN_BG   = RGBColor(0xFF, 0xF4, 0x9C)   # linearization-point yellow
LIN_EDGE = RGBColor(0xF9, 0xA8, 0x25)
BLK_BG   = RGBColor(0xFF, 0xCD, 0xD2)   # cross-host blocking red
BLK_EDGE = RGBColor(0xC6, 0x28, 0x28)

CXL_BG   = RGBColor(0xFF, 0xF3, 0xE0)
CXL_EDGE = RGBColor(0xE6, 0x51, 0x00)
DRAM_BG  = RGBColor(0xE3, 0xF2, 0xFD)
DRAM_EDGE= RGBColor(0x15, 0x65, 0xC0)


def _setup_prs():
    prs = Presentation()
    prs.slide_width  = Inches(13.33)
    prs.slide_height = Inches(7.5)
    return prs


def _blank_slide(prs, title, subtitle=None):
    s = prs.slides.add_slide(prs.slide_layouts[6])  # blank
    # Title bar
    tb = s.shapes.add_textbox(Inches(0.3), Inches(0.15), Inches(12.8), Inches(0.55))
    tf = tb.text_frame
    tf.margin_top = Pt(0); tf.margin_bottom = Pt(0)
    p = tf.paragraphs[0]
    r = p.add_run()
    r.text = title
    r.font.size = Pt(24); r.font.bold = True; r.font.color.rgb = GREY_D
    if subtitle:
        sb = s.shapes.add_textbox(Inches(0.3), Inches(0.7), Inches(12.8), Inches(0.35))
        sf = sb.text_frame
        sf.margin_top = Pt(0); sf.margin_bottom = Pt(0)
        p = sf.paragraphs[0]
        r = p.add_run()
        r.text = subtitle
        r.font.size = Pt(12); r.font.color.rgb = GREY; r.font.italic = True
    return s


def _box(slide, l, t, w, h, fill, edge, text, fs=10, bold=False, txt_col=GREY_D,
         dash=False):
    shp = slide.shapes.add_shape(MSO_SHAPE.ROUNDED_RECTANGLE,
                                 Inches(l), Inches(t), Inches(w), Inches(h))
    shp.fill.solid()
    shp.fill.fore_color.rgb = fill
    shp.line.color.rgb = edge
    shp.line.width = Pt(1.5)
    if dash:
        # MSO_LINE_DASH_STYLE doesn't expose dotted easily; use width as hint
        pass
    tf = shp.text_frame
    tf.margin_left = Pt(3); tf.margin_right = Pt(3)
    tf.margin_top = Pt(2); tf.margin_bottom = Pt(2)
    tf.word_wrap = True
    tf.vertical_anchor = MSO_ANCHOR.MIDDLE
    p = tf.paragraphs[0]
    p.alignment = PP_ALIGN.CENTER
    r = p.add_run()
    r.text = text
    r.font.size = Pt(fs)
    r.font.bold = bold
    r.font.color.rgb = txt_col
    return shp


def _arrow(slide, x0, y0, x1, y1, colour, label_text=None, dashed=False,
           label_above=True, lw=2.5, label_fs=10):
    conn = slide.shapes.add_connector(MSO_CONNECTOR.STRAIGHT,
                                      Inches(x0), Inches(y0),
                                      Inches(x1), Inches(y1))
    conn.line.color.rgb = colour
    conn.line.width = Pt(lw)
    # Add arrow head
    line = conn.line._get_or_add_ln()
    from pptx.oxml.ns import qn
    from lxml import etree
    # Add tail arrow end
    tail = line.find(qn('a:tailEnd'))
    if tail is None:
        tail = etree.SubElement(line, qn('a:tailEnd'))
    tail.set('type', 'triangle')
    tail.set('w', 'med')
    tail.set('h', 'med')
    if dashed:
        prstDash = line.find(qn('a:prstDash'))
        if prstDash is None:
            prstDash = etree.SubElement(line, qn('a:prstDash'))
        prstDash.set('val', 'dash')
    if label_text:
        mx = (x0 + x1) / 2.0
        my = (y0 + y1) / 2.0
        # Offset label so it doesn't sit on the line
        if abs(x1 - x0) > abs(y1 - y0):
            # Mostly horizontal
            ly = my - 0.20 if label_above else my + 0.05
            lb = slide.shapes.add_textbox(Inches(mx - 0.25), Inches(ly),
                                          Inches(0.6), Inches(0.22))
        else:
            lx = mx + 0.05 if label_above else mx - 0.55
            lb = slide.shapes.add_textbox(Inches(lx), Inches(my - 0.10),
                                          Inches(0.5), Inches(0.22))
        ltf = lb.text_frame
        ltf.margin_left = Pt(1); ltf.margin_right = Pt(1)
        ltf.margin_top = Pt(0); ltf.margin_bottom = Pt(0)
        lp = ltf.paragraphs[0]
        lp.alignment = PP_ALIGN.CENTER
        lr = lp.add_run()
        lr.text = label_text
        lr.font.size = Pt(label_fs)
        lr.font.bold = True
        lr.font.color.rgb = colour
        # White background fill so label is readable over a line
        lb.fill.solid()
        lb.fill.fore_color.rgb = WHITE
        lb.line.fill.background()


def _legend(slide, l, t):
    """Compact horizontal legend, 4 items in one row."""
    box = slide.shapes.add_shape(MSO_SHAPE.ROUNDED_RECTANGLE,
                                 Inches(l), Inches(t), Inches(6.5), Inches(0.45))
    box.fill.solid(); box.fill.fore_color.rgb = WHITE
    box.line.color.rgb = GREY; box.line.width = Pt(0.75)
    items = [
        ('─── ▶  Protocol A',  A_DEEP),
        ('╌╌╌ ▶  Protocol C',  C_DEEP),
        ('● Linearization point', LIN_EDGE),
        ('● Cross-host blocking', BLK_EDGE),
    ]
    # Place each label in its own narrow textbox so we keep colours.
    x = l + 0.1
    widths = [1.6, 1.6, 1.7, 1.7]
    for (lbl, col), w in zip(items, widths):
        tb = slide.shapes.add_textbox(Inches(x), Inches(t + 0.05),
                                      Inches(w), Inches(0.35))
        tf = tb.text_frame
        tf.margin_left = Pt(2); tf.margin_right = Pt(2)
        tf.margin_top = Pt(0); tf.margin_bottom = Pt(0)
        p = tf.paragraphs[0]; p.alignment = PP_ALIGN.LEFT
        r = p.add_run()
        r.text = lbl
        r.font.size = Pt(9); r.font.bold = True; r.font.color.rgb = col
        x += w


# ──────────────────────────────────────────────────────────────────────
# Slide 1 — Title
# ──────────────────────────────────────────────────────────────────────
def slide_title(prs):
    s = prs.slides.add_slide(prs.slide_layouts[6])
    # Big title
    tb = s.shapes.add_textbox(Inches(0.5), Inches(2.0), Inches(12.3), Inches(1.5))
    tf = tb.text_frame
    p = tf.paragraphs[0]
    p.alignment = PP_ALIGN.CENTER
    r = p.add_run()
    r.text = 'Protocol A vs Protocol C'
    r.font.size = Pt(44); r.font.bold = True; r.font.color.rgb = GREY_D
    # Subtitle
    sb = s.shapes.add_textbox(Inches(0.5), Inches(3.4), Inches(12.3), Inches(0.8))
    sf = sb.text_frame
    p = sf.paragraphs[0]; p.alignment = PP_ALIGN.CENTER
    r = p.add_run()
    r.text = 'Read / Write Path Comparison'
    r.font.size = Pt(28); r.font.color.rgb = A_DEEP
    # Subline
    sb2 = s.shapes.add_textbox(Inches(0.5), Inches(4.3), Inches(12.3), Inches(0.6))
    sf2 = sb2.text_frame
    p = sf2.paragraphs[0]; p.alignment = PP_ALIGN.CENTER
    r = p.add_run()
    r.text = 'measured T=64 workload-A on g3+g4 testbed  •  iter-11A snapshot'
    r.font.size = Pt(14); r.font.color.rgb = GREY; r.font.italic = True
    # Thesis line
    sb3 = s.shapes.add_textbox(Inches(0.5), Inches(5.2), Inches(12.3), Inches(1.5))
    sf3 = sb3.text_frame
    sf3.word_wrap = True
    p = sf3.paragraphs[0]; p.alignment = PP_ALIGN.CENTER
    r = p.add_run()
    r.text = ('Thesis: strict-A linearizability (Protocol A) requires '
              'fundamentally more stages, more linearization points, '
              'and more cross-host coordination than LRC (Protocol C). '
              'The measured per-stage data confirms this is a '
              'structural property, not an engineering artefact.')
    r.font.size = Pt(14); r.font.color.rgb = GREY_D; r.font.italic = True


# ──────────────────────────────────────────────────────────────────────
# Slide 2 — Write path topology
# ──────────────────────────────────────────────────────────────────────
def slide_write_topology(prs):
    s = _blank_slide(
        prs,
        'Write path topology — cross-host update(K, V), host 0 → owner host 1',
        'A: 14 numbered steps, 3 threads, 2 cross-host blocking points  •  '
        'C: 6 numbered steps, 1 thread, 1 cross-host blocking point'
    )

    # ── Canvas zones ──
    # CXL strip across the top (1.3 → 2.4 inches)
    _box(s, 0.3, 1.25, 12.7, 0.95, CXL_BG, CXL_EDGE,
         'CXL shared region — buckets, WriteRing[0][1], ForwardStaging, InvalRing[1][0], '
         'BucketLockTable (per-bucket LFM + write_epoch), KvBlockPool',
         fs=10, bold=True, txt_col=C_TXT)

    # Host 0 worker (left)
    _box(s, 0.4, 3.3, 2.6, 1.0, DRAM_BG, DRAM_EDGE,
         'host 0  worker\n(caller)', fs=12, bold=True, txt_col=A_TXT)

    # Host 1 WriteReceiver (centre)
    _box(s, 5.1, 3.3, 3.0, 1.0, A_LIGHT, A_DEEP,
         'host 1  WriteReceiver\n(cpu 65, single thread)\nProtocol A only',
         fs=11, bold=True, txt_col=A_TXT)

    # Host 0 InvalReceiver (right)
    _box(s, 10.2, 3.3, 2.7, 1.0, A_LIGHT, A_DEEP,
         'host 0  InvalReceiver\n(cpu 69)\nProtocol A only',
         fs=11, bold=True, txt_col=A_TXT)

    # Protocol A path - solid blue arrows ────────────────────────────
    # A1 worker → ForwardStaging (up to CXL strip)
    _arrow(s, 1.4, 3.3, 2.0, 2.2, A_DEEP, 'A1', dashed=False, label_above=True)
    # A2 worker → WriteRing.tail
    _arrow(s, 1.7, 3.3, 4.0, 2.2, A_DEEP, 'A2')
    # A3 worker → WriteEntry write
    _arrow(s, 2.0, 3.3, 5.0, 2.2, A_DEEP, 'A3')
    # A5 staging → WriteReceiver (down)
    _arrow(s, 6.0, 2.2, 6.0, 3.3, A_DEEP, 'A5', label_above=False)
    # A6 WriteReceiver runs W1..W12  — annotate inside its box (text on right of box)
    tb = s.shapes.add_textbox(Inches(5.0), Inches(4.5), Inches(3.2), Inches(0.4))
    tf = tb.text_frame
    p = tf.paragraphs[0]; p.alignment = PP_ALIGN.CENTER
    r = p.add_run()
    r.text = 'A6: execute_write_local W1..W12'
    r.font.size = Pt(10); r.font.bold = True; r.font.color.rgb = A_TXT

    # A7 WriteReceiver → InvalRing.tail
    _arrow(s, 7.6, 3.3, 8.5, 2.2, A_DEEP, 'A7', label_above=True)
    # A8 WriteReceiver → InvalEntry write
    _arrow(s, 7.9, 3.3, 9.5, 2.2, A_DEEP, 'A8')
    # A10 InvalRing → InvalReceiver (down)
    _arrow(s, 10.7, 2.2, 10.7, 3.3, A_DEEP, 'A10', label_above=False)
    # A11 InvalReceiver → cache_pool_set_stale (annotate)
    tb = s.shapes.add_textbox(Inches(10.0), Inches(4.5), Inches(3.1), Inches(0.4))
    tf = tb.text_frame
    p = tf.paragraphs[0]; p.alignment = PP_ALIGN.CENTER
    r = p.add_run()
    r.text = 'A11: cache_pool_set_stale + epoch++'
    r.font.size = Pt(9); r.font.bold = True; r.font.color.rgb = A_TXT

    # A12 InvalReceiver writes ACK back (up)
    _arrow(s, 11.6, 3.3, 11.6, 2.2, A_DEEP, 'A12', label_above=True)
    # A9 WriteReceiver spins on InvalRing ACK (curve left)
    _arrow(s, 11.2, 2.2, 8.8, 2.2, A_DEEP, 'A9 ACK', dashed=False,
           label_above=True, lw=1.5)
    # A13 WriteReceiver → resp_op_id ACK (up to staging)
    _arrow(s, 5.9, 3.3, 5.4, 2.2, A_DEEP, 'A13', label_above=False)
    # A4 worker spins on resp_op_id ACK (curve to far right of staging back to worker)
    _arrow(s, 4.6, 2.2, 2.5, 2.2, A_DEEP, 'A4 ACK', label_above=False, lw=1.5)
    # A14 worker freed
    tb = s.shapes.add_textbox(Inches(0.3), Inches(4.4), Inches(2.8), Inches(0.4))
    tf = tb.text_frame
    p = tf.paragraphs[0]; p.alignment = PP_ALIGN.CENTER
    r = p.add_run()
    r.text = 'A14: free slot, return 0'
    r.font.size = Pt(9); r.font.bold = True; r.font.color.rgb = A_TXT

    # Protocol C path - dashed orange arrows ─────────────────────────
    # C strip below the host row at y=5.0
    _box(s, 0.3, 5.0, 12.7, 1.6, C_LIGHT, C_DEEP,
         'Protocol C — host 0 worker only, in-place under cross-host LFM lock',
         fs=11, bold=True, txt_col=C_TXT)

    # C1 lock acquire (worker box → CXL bucket lock cacheline)
    _arrow(s, 2.0, 5.0, 2.0, 2.2, C_DEEP, 'C1 LFM acquire', dashed=True,
           label_above=False, lw=2.0)
    # C2 flush bucket
    _arrow(s, 3.5, 5.0, 3.5, 2.2, C_DEEP, 'C2 flush', dashed=True)
    # C3 scan
    _arrow(s, 5.0, 5.0, 5.0, 2.2, C_DEEP, 'C3 scan', dashed=True)
    # C4 slot publish (LINEARIZATION POINT)
    _arrow(s, 6.7, 5.0, 6.7, 2.2, C_DEEP, 'C4 commit', dashed=True)
    # C5 epoch bump
    _arrow(s, 8.4, 5.0, 8.4, 2.2, C_DEEP, 'C5 epoch++', dashed=True)
    # C6 unlock
    _arrow(s, 10.1, 5.0, 10.1, 2.2, C_DEEP, 'C6 unlock', dashed=True)

    # Mark linearization points (yellow) at A's W9 (in WriteReceiver box) and C's C4
    # Tiny yellow squares + footnote
    lin1 = s.shapes.add_shape(MSO_SHAPE.OVAL, Inches(6.4), Inches(3.5),
                              Inches(0.25), Inches(0.25))
    lin1.fill.solid(); lin1.fill.fore_color.rgb = LIN_BG
    lin1.line.color.rgb = LIN_EDGE; lin1.line.width = Pt(1.0)
    lin1tf = lin1.text_frame
    lin1tf.margin_left = Pt(1); lin1tf.margin_top = Pt(0)
    lin1tf.margin_right = Pt(1); lin1tf.margin_bottom = Pt(0)
    p = lin1tf.paragraphs[0]; p.alignment = PP_ALIGN.CENTER
    r = p.add_run(); r.text = 'W9'; r.font.size = Pt(7); r.font.bold = True
    r.font.color.rgb = C_TXT

    lin2 = s.shapes.add_shape(MSO_SHAPE.OVAL, Inches(6.7 - 0.12), Inches(5.5),
                              Inches(0.25), Inches(0.25))
    lin2.fill.solid(); lin2.fill.fore_color.rgb = LIN_BG
    lin2.line.color.rgb = LIN_EDGE; lin2.line.width = Pt(1.0)
    tf = lin2.text_frame
    tf.margin_left = Pt(1); tf.margin_right = Pt(1)
    tf.margin_top = Pt(0); tf.margin_bottom = Pt(0)
    p = tf.paragraphs[0]; p.alignment = PP_ALIGN.CENTER
    r = p.add_run(); r.text = '★'; r.font.size = Pt(8); r.font.bold = True

    # Bottom legend + tally (single-row layout, total height 0.45 in)
    _legend(s, 0.3, 6.95)
    tally_box = s.shapes.add_textbox(Inches(7.0), Inches(6.95), Inches(6.0),
                                     Inches(0.45))
    tf = tally_box.text_frame
    tf.margin_top = Pt(3); tf.word_wrap = True
    p = tf.paragraphs[0]; p.alignment = PP_ALIGN.RIGHT
    r = p.add_run()
    r.text = ('A: 14 steps · 3 threads · 2 cross-host blocks   |   '
              'C: 6 steps · 1 thread · 1 LFM acquire')
    r.font.size = Pt(10); r.font.color.rgb = GREY_D; r.font.bold = True


# ──────────────────────────────────────────────────────────────────────
# Slide 3 — Read path topology
# ──────────────────────────────────────────────────────────────────────
def slide_read_topology(prs):
    s = _blank_slide(
        prs,
        'Read path topology — cross-host search(K) on cache miss',
        'A: 16 steps, 2 threads, 1 cross-host blocking (R3 staging poll 200ms cap)  •  '
        'C: 4 steps, 1 thread, 0 cross-host blocking'
    )

    # CXL strip
    _box(s, 0.3, 1.25, 12.7, 0.95, CXL_BG, CXL_EDGE,
         'CXL shared region — buckets, ReadRing[0][1], ReadStaging[0][1], '
         'BucketLockTable.write_epoch, KvBlockPool',
         fs=10, bold=True, txt_col=C_TXT)

    # Host 0 worker
    _box(s, 0.4, 3.3, 2.6, 1.0, DRAM_BG, DRAM_EDGE,
         'host 0  worker\n(caller)\n+ TLS L1 (private DRAM)',
         fs=11, bold=True, txt_col=A_TXT)

    # Host 0 KvCachePool (L2, shared)
    _box(s, 3.3, 3.3, 2.6, 1.0, DRAM_BG, DRAM_EDGE,
         'host 0  KvCachePool L2\n(MAP_SHARED DRAM)\nseqlock CAS reader',
         fs=10, bold=True, txt_col=A_TXT)

    # Host 1 ReadReceiver
    _box(s, 7.5, 3.3, 3.0, 1.0, A_LIGHT, A_DEEP,
         'host 1  ReadReceiver\n(cpu 67, single thread)\nProtocol A only',
         fs=11, bold=True, txt_col=A_TXT)

    # Host 0 DRAM cache (Protocol C optional)
    _box(s, 10.7, 3.3, 2.3, 1.0, C_LIGHT, C_DEEP,
         'host 0  DRAM cache\n(opt, per-process)\nProtocol C only',
         fs=10, bold=True, txt_col=C_TXT)

    # Protocol A — solid blue arrows
    # A1r: bucket_epoch.load (worker → CXL line)
    _arrow(s, 1.0, 3.3, 1.0, 2.2, A_DEEP, 'A1r', label_above=False)
    # A2r: TLS lookup (in-worker, draw inside box as label)
    tb = s.shapes.add_textbox(Inches(0.4), Inches(4.4), Inches(2.6), Inches(0.4))
    tf = tb.text_frame
    p = tf.paragraphs[0]; p.alignment = PP_ALIGN.CENTER
    r = p.add_run(); r.text = 'A2r: TLS hit? ★ R0_tls_hit ~80 ns'
    r.font.size = Pt(9); r.font.bold = True; r.font.color.rgb = A_TXT

    # A3r: worker → L2 cache_pool
    _arrow(s, 3.0, 3.7, 3.3, 3.7, A_DEEP, 'A3r', label_above=True, lw=1.5)
    # A4r: bucket_epoch.load again (snapshot)
    _arrow(s, 2.1, 3.3, 2.1, 2.2, A_DEEP, 'A4r', label_above=True)
    # A5r-A7r: worker → ReadRing.tail + entry
    _arrow(s, 2.6, 3.3, 5.2, 2.2, A_DEEP, 'A5r', label_above=True)
    _arrow(s, 2.7, 3.3, 5.6, 2.2, A_DEEP, 'A6r', label_above=False)
    _arrow(s, 2.8, 3.3, 6.0, 2.2, A_DEEP, 'A7r', label_above=True)
    # A8r: worker spin on staging.ready_op_id (BLOCKING)
    sp_box = s.shapes.add_shape(MSO_SHAPE.OVAL, Inches(2.7), Inches(2.95),
                                Inches(0.4), Inches(0.4))
    sp_box.fill.solid(); sp_box.fill.fore_color.rgb = BLK_BG
    sp_box.line.color.rgb = BLK_EDGE; sp_box.line.width = Pt(1.5)
    tf = sp_box.text_frame
    tf.margin_left = Pt(1); tf.margin_right = Pt(1)
    tf.margin_top = Pt(0); tf.margin_bottom = Pt(0)
    p = tf.paragraphs[0]; p.alignment = PP_ALIGN.CENTER
    r = p.add_run(); r.text = 'A8r'; r.font.size = Pt(8); r.font.bold = True
    # A9r..A15r ReadReceiver work — annotate inside its box
    tb = s.shapes.add_textbox(Inches(7.4), Inches(4.4), Inches(3.2), Inches(0.6))
    tf = tb.text_frame
    tf.word_wrap = True
    p = tf.paragraphs[0]; p.alignment = PP_ALIGN.CENTER
    r = p.add_run()
    r.text = 'A9r-A15r:  flush+scan bucket → lock + sharer_bitmap |=  →  pool->read → ReadStaging write + ready_op_id publish'
    r.font.size = Pt(7); r.font.color.rgb = A_TXT
    # ReadRing → ReadReceiver (down)
    _arrow(s, 6.3, 2.2, 8.3, 3.3, A_DEEP, '', label_above=False, lw=1.5)
    # ReadStaging publish → worker poll
    _arrow(s, 8.8, 3.3, 4.0, 2.2, A_DEEP, 'A15r ready', dashed=False,
           label_above=True, lw=1.5)
    # A16r validate + memcpy
    tb = s.shapes.add_textbox(Inches(3.0), Inches(2.55), Inches(2.0), Inches(0.30))
    tf = tb.text_frame
    p = tf.paragraphs[0]; p.alignment = PP_ALIGN.CENTER
    r = p.add_run(); r.text = 'A16r: C13 + memcpy'
    r.font.size = Pt(8); r.font.bold = True; r.font.color.rgb = A_TXT
    # White background
    tb.fill.solid(); tb.fill.fore_color.rgb = WHITE
    tb.line.fill.background()

    # Protocol C — dashed orange arrows  ────────────────────────────
    _box(s, 0.3, 5.0, 12.7, 1.6, C_LIGHT, C_DEEP,
         'Protocol C — host 0 worker only, no cross-host coordination',
         fs=11, bold=True, txt_col=C_TXT)

    # C1r: bucket_idx (trivial; just annotate)
    tb = s.shapes.add_textbox(Inches(0.5), Inches(5.7), Inches(2.0), Inches(0.5))
    tf = tb.text_frame
    p = tf.paragraphs[0]; p.alignment = PP_ALIGN.CENTER
    r = p.add_run(); r.text = 'C1r: hash(K) → bucket_idx'
    r.font.size = Pt(10); r.font.color.rgb = C_TXT; r.font.bold = True

    # C2r: load write_epoch + opt DRAM cache hit
    _arrow(s, 4.0, 5.0, 4.0, 2.2, C_DEEP, 'C2r epoch', dashed=True)
    _arrow(s, 11.8, 5.0, 11.8, 4.3, C_DEEP, 'C2r DRAM hit?', dashed=True)
    # C3r: flush bucket
    _arrow(s, 6.0, 5.0, 6.0, 2.2, C_DEEP, 'C3r flush', dashed=True)
    # C4r: scan + return
    tb = s.shapes.add_textbox(Inches(7.0), Inches(5.7), Inches(3.5), Inches(0.5))
    tf = tb.text_frame
    p = tf.paragraphs[0]; p.alignment = PP_ALIGN.CENTER
    r = p.add_run(); r.text = 'C4r: scan 7 slots → return (LRC: no revalidation)'
    r.font.size = Pt(10); r.font.color.rgb = C_TXT; r.font.bold = True

    # Linearization point markers
    lin1 = s.shapes.add_shape(MSO_SHAPE.OVAL, Inches(8.7), Inches(3.4),
                              Inches(0.27), Inches(0.27))
    lin1.fill.solid(); lin1.fill.fore_color.rgb = LIN_BG
    lin1.line.color.rgb = LIN_EDGE; lin1.line.width = Pt(1.0)
    tf = lin1.text_frame
    tf.margin_left = Pt(0); tf.margin_right = Pt(0)
    tf.margin_top = Pt(0); tf.margin_bottom = Pt(0)
    p = tf.paragraphs[0]; p.alignment = PP_ALIGN.CENTER
    r = p.add_run(); r.text = '★'; r.font.size = Pt(8); r.font.bold = True

    # Bottom legend + tally
    _legend(s, 0.3, 6.95)
    tally_box = s.shapes.add_textbox(Inches(7.0), Inches(6.95), Inches(6.0),
                                     Inches(0.45))
    tf = tally_box.text_frame
    tf.margin_top = Pt(3); tf.word_wrap = True
    p = tf.paragraphs[0]; p.alignment = PP_ALIGN.RIGHT
    r = p.add_run()
    r.text = ('A: 16 steps · 2 threads · 1 cross-host block   |   '
              'C: 4 steps · 1 thread · 0 cross-host blocks')
    r.font.size = Pt(10); r.font.color.rgb = GREY_D; r.font.bold = True


# ──────────────────────────────────────────────────────────────────────
# Latency tables — Part 2
# ──────────────────────────────────────────────────────────────────────

A_WRITE_ROWS = [
    # (stage, what, p50, mean, p99, hint)
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
    """Build a pptx table. dominant_idx / lin_idx are sets of row indices to
    highlight (red and yellow respectively)."""
    nrows = len(rows) + 1
    ncols = len(headers)
    tbl_shape = slide.shapes.add_table(nrows, ncols,
                                       Inches(left), Inches(top),
                                       Inches(width), Inches(height))
    tbl = tbl_shape.table
    for i, w in enumerate(col_widths):
        tbl.columns[i].width = Inches(w)
    # Header row
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
            if j == 0:
                p.alignment = PP_ALIGN.LEFT
            elif j == 1:
                p.alignment = PP_ALIGN.LEFT
            elif j >= 2 and j <= 4:
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

    # Two side-by-side tables
    # Protocol A on the left
    header = ['Stage', 'What it does', 'p50 µs', 'mean µs', 'p99 µs', 'Note']
    col_w_a = [0.55, 2.5, 0.55, 0.65, 0.55, 1.6]
    # Highlight indices: dominant = W10 (idx 15), W1 (0) for outlier mean; lin = W9 (14)
    _make_table(s, A_WRITE_ROWS, header, left=0.3, top=1.2,
                width=6.4, height=4.3,
                col_widths=col_w_a, accent=A_DEEP,
                dominant_idx={15}, lin_idx={14},
                header_fs=10, body_fs=8)

    # Protocol C on right — total width 6.3 in (col widths sum)
    col_w_c = [0.65, 2.3, 0.55, 0.55, 0.55, 1.7]
    _make_table(s, C_WRITE_ROWS, header, left=6.9, top=1.2,
                width=6.3, height=2.0,
                col_widths=col_w_c, accent=C_DEEP,
                dominant_idx={0}, lin_idx={3},
                header_fs=10, body_fs=9)

    # Label the tables
    lbl_a = s.shapes.add_textbox(Inches(0.3), Inches(1.0), Inches(6.4), Inches(0.25))
    p = lbl_a.text_frame.paragraphs[0]; p.alignment = PP_ALIGN.LEFT
    r = p.add_run(); r.text = 'Protocol A — 17 distinct stages (W1-W12 + I1-I8 sub-path)'
    r.font.size = Pt(12); r.font.bold = True; r.font.color.rgb = A_TXT
    lbl_c = s.shapes.add_textbox(Inches(6.9), Inches(1.0), Inches(6.1), Inches(0.25))
    p = lbl_c.text_frame.paragraphs[0]; p.alignment = PP_ALIGN.LEFT
    r = p.add_run(); r.text = 'Protocol C — 6 stages total (Lock/Scan/Publish/Epoch/Unlock + Total)'
    r.font.size = Pt(12); r.font.bold = True; r.font.color.rgb = C_TXT

    # Bottom comparison summary
    sm = s.shapes.add_shape(MSO_SHAPE.ROUNDED_RECTANGLE,
                            Inches(6.9), Inches(3.4),
                            Inches(6.1), Inches(2.2))
    sm.fill.solid(); sm.fill.fore_color.rgb = GREY_L
    sm.line.color.rgb = GREY; sm.line.width = Pt(0.75)
    tf = sm.text_frame
    tf.margin_left = Pt(8); tf.margin_right = Pt(8)
    tf.margin_top = Pt(5); tf.margin_bottom = Pt(5)
    tf.word_wrap = True
    p = tf.paragraphs[0]; p.alignment = PP_ALIGN.LEFT
    r = p.add_run(); r.text = 'Mean per-op cost'
    r.font.size = Pt(11); r.font.bold = True; r.font.color.rgb = GREY_D
    p = tf.add_paragraph()
    r = p.add_run(); r.text = '\n  Protocol A (no remote sharer):   ~ 13 µs/op'
    r.font.size = Pt(10); r.font.color.rgb = A_TXT
    p = tf.add_paragraph()
    r = p.add_run(); r.text = '  Protocol A (with remote sharer):  ~ 19 µs/op  (+ inval path I1-I8)'
    r.font.size = Pt(10); r.font.color.rgb = A_TXT
    p = tf.add_paragraph()
    r = p.add_run(); r.text = '  Protocol C (always):              30.6 µs/op  (84% in lock alone)'
    r.font.size = Pt(10); r.font.color.rgb = C_TXT
    p = tf.add_paragraph()
    r = p.add_run(); r.text = '\nDominant stage shape:'
    r.font.size = Pt(11); r.font.bold = True; r.font.color.rgb = GREY_D
    p = tf.add_paragraph()
    r = p.add_run(); r.text = '\n  A: spread across 12 stages (W10 = 6.2 µs largest, MESI on shared L2 insert)'
    r.font.size = Pt(10); r.font.color.rgb = GREY_D
    p = tf.add_paragraph()
    r = p.add_run(); r.text = '  C: concentrated in 1 stage (LFM lock acquire under cross-host Zipf hot-bucket)'
    r.font.size = Pt(10); r.font.color.rgb = GREY_D


def slide_read_latency_table(prs):
    s = _blank_slide(
        prs,
        'Read path — measured per-stage latency (T=64 workload-A)',
        'A measured from iter-11A Phase 5; C inferred from primitives (no historical search-side decomp run for C)  '
        '•  Probabilities by outcome shown for A'
    )

    header = ['Stage', 'What it does', 'p50 µs', 'mean µs', 'p99 µs', 'Note']
    col_w_a = [0.85, 2.4, 0.55, 0.6, 0.55, 1.6]
    # Highlight: dominant = R3 (idx 4); lin = R0_tls_hit (0), R2hit (2)
    _make_table(s, A_READ_ROWS, header, left=0.3, top=1.2,
                width=6.55, height=2.2,
                col_widths=col_w_a, accent=A_DEEP,
                dominant_idx={4}, lin_idx={0, 2},
                header_fs=10, body_fs=9)

    col_w_c = [1.45, 1.95, 0.50, 0.55, 0.45, 1.45]  # sum = 6.35
    _make_table(s, C_READ_ROWS, header, left=6.95, top=1.2,
                width=6.35, height=2.0,
                col_widths=col_w_c, accent=C_DEEP,
                dominant_idx=set(), lin_idx={1, 4},
                header_fs=10, body_fs=9)

    # Outcome probability bars for A
    lbl = s.shapes.add_textbox(Inches(0.3), Inches(3.7), Inches(6.55), Inches(0.30))
    p = lbl.text_frame.paragraphs[0]; p.alignment = PP_ALIGN.LEFT
    r = p.add_run(); r.text = 'Protocol A read-path outcome distribution (T=64 wl-A)'
    r.font.size = Pt(12); r.font.bold = True; r.font.color.rgb = A_TXT

    # Inline outcome rows: outcome, p%, p50 latency, bar
    outcomes_a = [
        ('TLS L1 hit',                  '~18%', '~0.08 µs', 0.18),
        ('L2 cache_pool hit',           '~64%', '~7 µs',    0.64),
        ('Owner-self CXL fetch',        '~17%', '~10 µs',   0.17),
        ('Cross-host miss (R3+R4)',     '~0.2%', '~22 µs',  0.002),
    ]
    y = 4.10
    bar_x = 0.3
    bar_w_max = 6.0
    for nm, prob, lat, frac in outcomes_a:
        # name
        tb = s.shapes.add_textbox(Inches(bar_x), Inches(y), Inches(1.8), Inches(0.28))
        p = tb.text_frame.paragraphs[0]
        r = p.add_run(); r.text = nm
        r.font.size = Pt(10); r.font.color.rgb = GREY_D; r.font.bold = True
        # bar background
        bg = s.shapes.add_shape(MSO_SHAPE.RECTANGLE,
                                Inches(bar_x + 1.85), Inches(y + 0.05),
                                Inches(2.5), Inches(0.18))
        bg.fill.solid(); bg.fill.fore_color.rgb = GREY_L
        bg.line.fill.background()
        # bar fill (sqrt scale so the 0.2% is at least visible)
        w_in = max(0.04, 2.5 * (frac ** 0.5))
        fg = s.shapes.add_shape(MSO_SHAPE.RECTANGLE,
                                Inches(bar_x + 1.85), Inches(y + 0.05),
                                Inches(w_in), Inches(0.18))
        fg.fill.solid(); fg.fill.fore_color.rgb = A_DEEP
        fg.line.fill.background()
        # prob and lat labels
        tb = s.shapes.add_textbox(Inches(bar_x + 4.45), Inches(y), Inches(0.8),
                                  Inches(0.28))
        p = tb.text_frame.paragraphs[0]
        r = p.add_run(); r.text = prob
        r.font.size = Pt(9); r.font.color.rgb = GREY_D
        tb = s.shapes.add_textbox(Inches(bar_x + 5.25), Inches(y), Inches(1.4),
                                  Inches(0.28))
        p = tb.text_frame.paragraphs[0]
        r = p.add_run(); r.text = lat
        r.font.size = Pt(9); r.font.bold = True
        r.font.color.rgb = A_TXT
        y += 0.30

    # Side-by-side comparison summary
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
    p = tf.add_paragraph()
    r = p.add_run(); r.text = '  A (TLS L1 hit):      ~ 80 ns'
    r.font.size = Pt(10); r.font.color.rgb = A_TXT
    p = tf.add_paragraph()
    r = p.add_run(); r.text = '  C (DRAM cache hit): ~ 700 ns'
    r.font.size = Pt(10); r.font.color.rgb = C_TXT
    p = tf.add_paragraph()
    r = p.add_run(); r.text = '\nReader worst-case (cross-host miss)'
    r.font.size = Pt(11); r.font.bold = True; r.font.color.rgb = GREY_D
    p = tf.add_paragraph()
    r = p.add_run(); r.text = '  A: ~ 22 µs (R1+R2miss+R3+R4+R6)'
    r.font.size = Pt(10); r.font.color.rgb = A_TXT
    p = tf.add_paragraph()
    r = p.add_run(); r.text = '  C: ~ 1 µs  (every read is a local flush+scan)'
    r.font.size = Pt(10); r.font.color.rgb = C_TXT
    p = tf.add_paragraph()
    r = p.add_run(); r.text = '\nKey insight'
    r.font.size = Pt(11); r.font.bold = True; r.font.color.rgb = GREY_D
    p = tf.add_paragraph()
    r = p.add_run()
    r.text = ('  Only TLS L1 hits beat C. Every other A path '
              'pays for L2 seqlock or cross-host coordination. '
              'C readers are near-CXL-physics regardless of '
              'contention.')
    r.font.size = Pt(10); r.font.color.rgb = GREY_D


# ──────────────────────────────────────────────────────────────────────
# Main
# ──────────────────────────────────────────────────────────────────────
def main():
    out = os.path.join(os.path.dirname(__file__), '..',
                       'docs', 'protocol_a_vs_c_path_comparison.pptx')
    out = os.path.abspath(out)
    prs = _setup_prs()
    slide_title(prs)
    slide_write_topology(prs)
    slide_read_topology(prs)
    slide_write_latency_table(prs)
    slide_read_latency_table(prs)
    prs.save(out)
    print('wrote', out)


if __name__ == '__main__':
    main()
