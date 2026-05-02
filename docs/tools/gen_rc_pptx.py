#!/usr/bin/env python3
"""
Release Consistency slides for CXL-FUSEE discussion (English).
Font: title=20pt, content=14pt.
"""

import os
from pptx import Presentation
from pptx.util import Inches, Pt
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN, MSO_ANCHOR
from pptx.enum.shapes import MSO_SHAPE
from pptx.oxml.ns import qn

os.makedirs("docs", exist_ok=True)

# ── Colors ──
WHITE = RGBColor(0xFF, 0xFF, 0xFF)
DG    = RGBColor(0x33, 0x33, 0x33)
GRAY  = RGBColor(0x9E, 0x9E, 0x9E)
LGRAY = RGBColor(0xF5, 0xF5, 0xF5)

A_C    = RGBColor(0x15, 0x65, 0xC0); A_L = RGBColor(0xBB, 0xDE, 0xFB)
B_C    = RGBColor(0xC6, 0x28, 0x28); B_L = RGBColor(0xFF, 0xCD, 0xD2)
C_C    = RGBColor(0x6A, 0x1B, 0x9A); C_L = RGBColor(0xE1, 0xBE, 0xE7)

GOOD   = RGBColor(0x1B, 0x5E, 0x20); GOOD_L = RGBColor(0xC8, 0xE6, 0xC9)
BAD    = RGBColor(0xB7, 0x1C, 0x1C); BAD_L  = RGBColor(0xFF, 0xCD, 0xD2)
HI     = RGBColor(0xE6, 0x51, 0x00); HI_L   = RGBColor(0xFF, 0xE0, 0xB2)
HDR    = RGBColor(0xE6, 0x51, 0x00)
VAR    = RGBColor(0xFF, 0xF9, 0xC4)

TITLE_FS = 20
BODY_FS = 14
SMALL_FS = 11

# ── Helpers ──
def _in(v): return Inches(v)

def box(slide, l, t, w, h, fc, ec=None, bw=Pt(1.5), text="", fs=BODY_FS,
        c=DG, bold=False, align=PP_ALIGN.CENTER):
    s = slide.shapes.add_shape(MSO_SHAPE.ROUNDED_RECTANGLE,
                               _in(l), _in(t), _in(w), _in(h))
    s.fill.solid(); s.fill.fore_color.rgb = fc
    if ec:
        s.line.color.rgb = ec; s.line.width = bw
    else:
        s.line.fill.background()
    if text:
        tf = s.text_frame; tf.word_wrap = True
        tf.margin_top = Pt(4); tf.margin_bottom = Pt(4)
        tf.margin_left = Pt(6); tf.margin_right = Pt(6)
        p = tf.paragraphs[0]; p.alignment = align
        p.space_before = Pt(0); p.space_after = Pt(0)
        run = p.add_run(); run.text = text
        run.font.size = Pt(fs); run.font.color.rgb = c; run.font.bold = bold
    return s

def text(slide, l, t, w, h, txt, fs=BODY_FS, c=DG, bold=False, align=PP_ALIGN.LEFT):
    tb = slide.shapes.add_textbox(_in(l), _in(t), _in(w), _in(h))
    tf = tb.text_frame; tf.word_wrap = True
    p = tf.paragraphs[0]; p.alignment = align
    run = p.add_run(); run.text = txt
    run.font.size = Pt(fs); run.font.color.rgb = c; run.font.bold = bold
    return tb

def arrow(slide, x0, y0, x1, y1, c=DG, w=Pt(2), dashed=False):
    conn = slide.shapes.add_connector(1, _in(x0), _in(y0), _in(x1), _in(y1))
    conn.line.color.rgb = c; conn.line.width = w
    if dashed: conn.line.dash_style = 2
    fmt = conn.line._ln
    tail = fmt.makeelement(qn('a:tailEnd'), {})
    tail.set('type', 'triangle'); tail.set('w', 'med'); tail.set('len', 'med')
    fmt.append(tail)

def title(slide, txt, color=DG):
    text(slide, 0.4, 0.2, 12.5, 0.5, txt, TITLE_FS, color, bold=True, align=PP_ALIGN.CENTER)

def cell(cell_obj, txt, fs=BODY_FS, bold=False, color=DG, fill=None,
         align=PP_ALIGN.CENTER):
    if fill is not None:
        cell_obj.fill.solid()
        cell_obj.fill.fore_color.rgb = fill
    cell_obj.vertical_anchor = MSO_ANCHOR.MIDDLE
    cell_obj.margin_left = Inches(0.08); cell_obj.margin_right = Inches(0.08)
    cell_obj.margin_top = Inches(0.05); cell_obj.margin_bottom = Inches(0.05)
    tf = cell_obj.text_frame; tf.word_wrap = True
    p = tf.paragraphs[0]; p.alignment = align
    for run in list(p.runs): run.text = ""
    run = p.add_run(); run.text = txt
    run.font.size = Pt(fs); run.font.bold = bold; run.font.color.rgb = color


prs = Presentation()
prs.slide_width = Inches(13.33)
prs.slide_height = Inches(7.5)


# ════════════════════════════════════════════════
#  SLIDE 1: RC basic concept
# ════════════════════════════════════════════════
slide = prs.slides.add_slide(prs.slide_layouts[6])
title(slide, "Release Consistency: Basic Concept")

text(slide, 0.5, 0.85, 12.3, 0.4,
     "Three nodes share variables x and y protected by lock L",
     BODY_FS, GRAY)

# Node A code
box(slide, 0.5, 1.45, 4.0, 2.3, A_L, A_C, Pt(2))
text(slide, 0.5, 1.5, 4.0, 0.4, "Node A (writer)", BODY_FS, A_C, bold=True,
     align=PP_ALIGN.CENTER)
text(slide, 0.8, 1.95, 3.5, 1.75,
     "acquire(L)\n  x = 1\n  y = 2\nrelease(L)",
     BODY_FS, DG, align=PP_ALIGN.LEFT)

# Node B code
box(slide, 4.7, 1.45, 4.0, 2.3, B_L, B_C, Pt(2))
text(slide, 4.7, 1.5, 4.0, 0.4, "Node B (reader)", BODY_FS, B_C, bold=True,
     align=PP_ALIGN.CENTER)
text(slide, 5.0, 1.95, 3.7, 1.75,
     "acquire(L)\n  read x   // expect 1\n  read y   // expect 2\nrelease(L)",
     BODY_FS, DG, align=PP_ALIGN.LEFT)

# Node C code
box(slide, 8.9, 1.45, 4.0, 2.3, C_L, C_C, Pt(2))
text(slide, 8.9, 1.5, 4.0, 0.4, "Node C (uninvolved)", BODY_FS, C_C, bold=True,
     align=PP_ALIGN.CENTER)
text(slide, 9.2, 1.95, 3.5, 1.75,
     "// never uses x, y, L",
     BODY_FS, GRAY, align=PP_ALIGN.LEFT)

# RC guarantee box
box(slide, 0.5, 4.1, 12.3, 1.0, HI_L, HI, Pt(2))
text(slide, 0.7, 4.15, 11.9, 0.35, "RC guarantee",
     BODY_FS, HI, bold=True, align=PP_ALIGN.LEFT)
text(slide, 0.7, 4.5, 11.9, 0.6,
     "After B's acquire(L) completes, B must see all writes A made before its release(L): x=1, y=2.",
     BODY_FS, DG, align=PP_ALIGN.LEFT)

# Key insight
box(slide, 0.5, 5.3, 12.3, 1.9, LGRAY, GRAY, Pt(1))
text(slide, 0.7, 5.4, 11.9, 0.4, "Key insight",
     BODY_FS, DG, bold=True, align=PP_ALIGN.LEFT)
text(slide, 0.7, 5.85, 11.9, 1.3,
     "- Ordinary load/store do NOT need to be immediately visible to other nodes\n"
     "- At release time, writes in the critical section MUST become visible to future acquirers of the same lock\n"
     "- A perfect fit for lock-protected critical sections",
     BODY_FS, DG, align=PP_ALIGN.LEFT)


# ════════════════════════════════════════════════
#  SLIDE 2: Eager RC
# ════════════════════════════════════════════════
slide = prs.slides.add_slide(prs.slide_layouts[6])
title(slide, "Eager Release Consistency: Writer Pushes")

# Timeline headers
text(slide, 0.5, 0.85, 6.0, 0.4, "Node A (writer)",
     BODY_FS, A_C, bold=True, align=PP_ALIGN.CENTER)
text(slide, 7.0, 0.85, 6.0, 0.4, "Node B (reader, later)",
     BODY_FS, B_C, bold=True, align=PP_ALIGN.CENTER)

# Node A timeline
steps_a = [
    ("acquire(L)", "take the lock", A_L, A_C),
    ("x = 1", "write local cache", A_L, A_C),
    ("y = 2", "write local cache", A_L, A_C),
    ("release(L)  begins",
     "1. broadcast x=1, y=2 to all replicas\n2. wait for ack\n3. finally release the lock",
     HI_L, HI),
]
y = 1.35
for i, (op, note, fc, ec) in enumerate(steps_a):
    box(slide, 0.5, y, 2.2, 0.55, fc, ec, Pt(2), op, BODY_FS, ec, bold=True)
    text(slide, 2.85, y + 0.05, 3.5, 0.5, note, SMALL_FS, DG, align=PP_ALIGN.LEFT)
    if i < len(steps_a) - 1:
        arrow(slide, 1.6, y + 0.55, 1.6, y + 0.85, GRAY, Pt(1.5))
    y += 0.95

# Network push
arrow(slide, 6.5, 2.2, 7.0, 2.2, HI, Pt(3))
text(slide, 6.2, 1.7, 1.4, 0.4, "push", SMALL_FS, HI, bold=True, align=PP_ALIGN.CENTER)

# Node B timeline
steps_b = [
    ("acquire(L)", "take lock (local cache already has x=1, y=2)", GOOD_L, GOOD),
    ("read x -> 1", "local cache hit", GOOD_L, GOOD),
    ("read y -> 2", "local cache hit", GOOD_L, GOOD),
    ("release(L)", "fast", GOOD_L, GOOD),
]
y = 1.35
for i, (op, note, fc, ec) in enumerate(steps_b):
    box(slide, 7.0, y, 2.2, 0.55, fc, ec, Pt(2), op, BODY_FS, ec, bold=True)
    text(slide, 9.35, y + 0.05, 3.5, 0.5, note, SMALL_FS, DG, align=PP_ALIGN.LEFT)
    if i < len(steps_b) - 1:
        arrow(slide, 8.1, y + 0.55, 8.1, y + 0.85, GRAY, Pt(1.5))
    y += 0.95

# Summary
box(slide, 0.5, 5.4, 12.3, 1.8, HI_L, HI, Pt(2))
text(slide, 0.7, 5.5, 11.9, 0.4, "Characteristics",
     BODY_FS, HI, bold=True, align=PP_ALIGN.LEFT)
text(slide, 0.7, 5.95, 11.9, 1.2,
     "- Writer pushes updates to all replicas at release time (including nodes that do not need them)\n"
     "- Reader can read immediately after acquire, no fetch needed\n"
     "- Classic implementation: Munin (Rice, 1991)",
     BODY_FS, DG, align=PP_ALIGN.LEFT)


# ════════════════════════════════════════════════
#  SLIDE 3: Lazy RC
# ════════════════════════════════════════════════
slide = prs.slides.add_slide(prs.slide_layouts[6])
title(slide, "Lazy Release Consistency: Reader Pulls")

text(slide, 0.5, 0.85, 6.0, 0.4, "Node A (writer)",
     BODY_FS, A_C, bold=True, align=PP_ALIGN.CENTER)
text(slide, 7.0, 0.85, 6.0, 0.4, "Node B (reader, later)",
     BODY_FS, B_C, bold=True, align=PP_ALIGN.CENTER)

# Node A timeline (lazy)
steps_a = [
    ("acquire(L)", "take the lock", A_L, A_C),
    ("x = 1", "write local cache", A_L, A_C),
    ("y = 2", "write local cache", A_L, A_C),
    ("release(L)",
     "1. locally record 'I wrote x, y at epoch=T'\n2. release the lock immediately\n(no data pushed)",
     GOOD_L, GOOD),
]
y = 1.35
for i, (op, note, fc, ec) in enumerate(steps_a):
    box(slide, 0.5, y, 2.2, 0.55, fc, ec, Pt(2), op, BODY_FS, ec, bold=True)
    text(slide, 2.85, y + 0.05, 3.5, 0.5, note, SMALL_FS, DG, align=PP_ALIGN.LEFT)
    if i < len(steps_a) - 1:
        arrow(slide, 1.6, y + 0.55, 1.6, y + 0.85, GRAY, Pt(1.5))
    y += 0.95

# Pull arrow (B <- A)
arrow(slide, 7.0, 2.2, 6.5, 2.2, C_C, Pt(3))
text(slide, 6.2, 1.7, 1.4, 0.4, "pull", SMALL_FS, C_C, bold=True, align=PP_ALIGN.CENTER)

# Node B timeline (lazy)
steps_b = [
    ("acquire(L)",
     "on acquire, check:\n'last sync @ epoch=T0,\nA wrote x, y at epoch=T'\n-> pull x=1, y=2 from A",
     C_L, C_C),
    ("read x -> 1", "now local cache has it -> hit", GOOD_L, GOOD),
    ("read y -> 2", "local cache hit", GOOD_L, GOOD),
    ("release(L)", "fast", GOOD_L, GOOD),
]
y = 1.35
for i, (op, note, fc, ec) in enumerate(steps_b):
    h = 0.9 if i == 0 else 0.55
    box(slide, 7.0, y, 2.2, h, fc, ec, Pt(2), op, BODY_FS, ec, bold=True)
    text(slide, 9.35, y + 0.05, 3.5, h, note, SMALL_FS, DG, align=PP_ALIGN.LEFT)
    y += h + 0.2

# Summary
box(slide, 0.5, 5.6, 12.3, 1.6, GOOD_L, GOOD, Pt(2))
text(slide, 0.7, 5.7, 11.9, 0.4, "Characteristics",
     BODY_FS, GOOD, bold=True, align=PP_ALIGN.LEFT)
text(slide, 0.7, 6.15, 11.9, 1.1,
     "- Writer's release is nearly free (just records metadata)\n"
     "- Reader pulls on demand at acquire time (targeted fetch)\n"
     "- Classic implementation: TreadMarks (Rice, 1994) - 2-5x faster than Eager in practice",
     BODY_FS, DG, align=PP_ALIGN.LEFT)


# ════════════════════════════════════════════════
#  SLIDE 4: Eager vs Lazy + why CXL-FUSEE prefers Lazy
# ════════════════════════════════════════════════
slide = prs.slides.add_slide(prs.slide_layouts[6])
title(slide, "Eager vs Lazy: Why CXL-FUSEE Chooses Lazy")

# Example scenario
text(slide, 0.5, 0.85, 12.3, 0.4,
     "Scenario: 3 nodes A, B, C. A and B share lock L; C never touches x, y, or L.",
     BODY_FS, DG, align=PP_ALIGN.LEFT)

# Two side-by-side scenarios
# Eager side
box(slide, 0.5, 1.5, 6.1, 3.0, HI_L, HI, Pt(2))
text(slide, 0.5, 1.55, 6.1, 0.4, "Eager RC",
     BODY_FS, HI, bold=True, align=PP_ALIGN.CENTER)
text(slide, 0.8, 2.0, 5.5, 2.5,
     "At A's release:\n"
     "  broadcast x=1 -> Node B\n"
     "  broadcast x=1 -> Node C  (wasted!)\n\n"
     "C receives a useless update\n\n"
     "Message count = writers x nodes  (scales with N)",
     BODY_FS, DG, align=PP_ALIGN.LEFT)

# Lazy side
box(slide, 6.8, 1.5, 6.1, 3.0, GOOD_L, GOOD, Pt(2))
text(slide, 6.8, 1.55, 6.1, 0.4, "Lazy RC",
     BODY_FS, GOOD, bold=True, align=PP_ALIGN.CENTER)
text(slide, 7.1, 2.0, 5.5, 2.5,
     "At A's release:\n"
     "  record 'I wrote x at epoch=T'\n\n"
     "B pulls x=1 when it acquires  (good)\n"
     "C does nothing  (good)\n\n"
     "Message count = actual accessors  (targeted)",
     BODY_FS, DG, align=PP_ALIGN.LEFT)

# Mapping to CXL-FUSEE
box(slide, 0.5, 4.75, 12.3, 2.4, A_L, A_C, Pt(2))
text(slide, 0.7, 4.85, 11.9, 0.4, "Mapping to CXL-FUSEE",
     BODY_FS, A_C, bold=True, align=PP_ALIGN.LEFT)
text(slide, 0.7, 5.3, 11.9, 1.8,
     "- CXL is essentially shared memory; other nodes see the latest value by loading CXL\n"
     "- Writer stores to CXL in Step 5 = publish (visible to anyone who loads CXL)\n"
     "- Whether to replicate into local DRAM is an optional optimization (local cache)\n"
     "- Lazy RC fits perfectly: writer does not push, reader pulls from CXL on demand",
     BODY_FS, DG, align=PP_ALIGN.LEFT)


# ════════════════════════════════════════════════
#  SLIDE 5: Detailed comparison of three options
# ════════════════════════════════════════════════
slide = prs.slides.add_slide(prs.slide_layouts[6])
title(slide, "Detailed Comparison of the Three Options")

rows = 6
cols = 5
left = Inches(0.4); top = Inches(1.05)
width = Inches(12.5); height = Inches(4.4)

tbl_shape = slide.shapes.add_table(rows, cols, left, top, width, height)
tbl = tbl_shape.table

tbl.columns[0].width = Inches(2.4)
for i in range(1, 5):
    tbl.columns[i].width = Inches((12.5 - 2.4) / 4)

tbl.rows[0].height = Inches(0.6)
for r in range(1, 6):
    tbl.rows[r].height = Inches((4.4 - 0.6) / 5)

# Header
cell(tbl.cell(0, 0), "", fs=BODY_FS, fill=WHITE)
cell(tbl.cell(0, 1), "Current\n(Async)",        fs=BODY_FS, bold=True, color=DG,   fill=LGRAY)
cell(tbl.cell(0, 2), "Option A\n(Sync Repl)",    fs=BODY_FS, bold=True, color=BAD,  fill=BAD_L)
cell(tbl.cell(0, 3), "Option B\n(Eager RC)",     fs=BODY_FS, bold=True, color=HI,   fill=HI_L)
cell(tbl.cell(0, 4), "Option C\n(Lazy RC) *",    fs=BODY_FS, bold=True, color=GOOD, fill=GOOD_L)

data = [
    ("Writer waits for replicators?",
     "no", "yes (all acks)", "no (send + go)", "no"),
    ("Commit point meaning",
     "I wrote CXL",
     "all nodes local-updated",
     "I wrote CXL + sent notify",
     "I wrote CXL + bumped epoch"),
    ("Write latency (estimate)",
     "~12 us", "~20-30 us", "~14 us", "~13 us"),
    ("Write throughput per bucket",
     "~83k/s", "~30-60k/s", "~70k/s", "~77k/s"),
    ("Extra metadata",
     "none", "ack_bitmap + epoch", "per-node invalidation list", "write_epoch per bucket"),
]

for ri, row in enumerate(data, start=1):
    cell(tbl.cell(ri, 0), row[0], fs=BODY_FS, bold=True, color=DG, fill=HI_L)
    for ci in range(1, 5):
        fill = None
        col = DG
        if ci == 4:
            fill = GOOD_L
        elif ci == 2:
            fill = BAD_L
        cell(tbl.cell(ri, ci), row[ci], fs=BODY_FS, color=col, fill=fill)

# Bottom recommendation
box(slide, 0.5, 5.6, 12.3, 1.5, GOOD_L, GOOD, Pt(2))
text(slide, 0.7, 5.7, 11.9, 0.4,
     "Choice (preference: latency/throughput > consistency)",
     BODY_FS, GOOD, bold=True, align=PP_ALIGN.LEFT)
text(slide, 0.7, 6.15, 11.9, 0.95,
     "- Option A excluded: 2-3x write latency, throughput cut in half\n"
     "- Option C (Lazy RC) is optimal: latency close to current design, clean RC semantics, fast reads",
     BODY_FS, DG, align=PP_ALIGN.LEFT)


# ════════════════════════════════════════════════
#  SLIDE 6: CXL hardware failure - what each option saves
# ════════════════════════════════════════════════
slide = prs.slides.add_slide(prs.slide_layouts[6])
title(slide, "CXL Hardware Failure: What Each Option Preserves")

text(slide, 0.5, 0.85, 12.3, 0.4,
     "Scenario: the CXL memory module hardware fails (unmap also fails)",
     BODY_FS, GRAY, align=PP_ALIGN.LEFT)

# All options lose these (in CXL)
box(slide, 0.5, 1.4, 12.3, 1.2, BAD_L, BAD, Pt(2))
text(slide, 0.7, 1.5, 11.9, 0.4, "All options lose (state that lived in CXL):",
     BODY_FS, BAD, bold=True, align=PP_ALIGN.LEFT)
text(slide, 0.7, 1.9, 11.9, 0.7,
     "- BucketLock table (authoritative hash index)\n"
     "- Staging Buffer (KV data being replicated)\n"
     "- OpLog (crash recovery log)",
     BODY_FS, DG, align=PP_ALIGN.LEFT)

# Local DRAM remaining table
rows = 5; cols = 2
left = Inches(0.5); top = Inches(2.85)
width = Inches(12.3); height = Inches(3.0)
tbl_shape = slide.shapes.add_table(rows, cols, left, top, width, height)
tbl = tbl_shape.table
tbl.columns[0].width = Inches(3.5)
tbl.columns[1].width = Inches(12.3 - 3.5)
tbl.rows[0].height = Inches(0.55)
for r in range(1, 5):
    tbl.rows[r].height = Inches((3.0 - 0.55) / 4)

cell(tbl.cell(0, 0), "Option", fs=BODY_FS, bold=True, color=WHITE, fill=HDR)
cell(tbl.cell(0, 1), "What each node's local DRAM retains",
     fs=BODY_FS, bold=True, color=WHITE, fill=HDR)

local_data = [
    ("Current (Async)",
     "only partial buckets + KVs the node happened to access (severely incomplete)",
     BAD_L, BAD),
    ("Option A (Sync Repl)",
     "all committed buckets + KVs (complete replica)",
     GOOD_L, GOOD),
    ("Option B (Eager RC)",
     "most buckets + KVs (most pushes succeeded)",
     HI_L, HI),
    ("Option C (Lazy RC)",
     "only what the node acquired the lock for (least complete)",
     BAD_L, BAD),
]
for ri, (name, desc, fc, ec) in enumerate(local_data, start=1):
    cell(tbl.cell(ri, 0), name, fs=BODY_FS, bold=True, color=ec, fill=fc)
    cell(tbl.cell(ri, 1), desc, fs=BODY_FS, color=DG, fill=LGRAY,
         align=PP_ALIGN.LEFT)

# Punchline
box(slide, 0.5, 6.1, 12.3, 1.0, HI_L, HI, Pt(2))
text(slide, 0.7, 6.2, 11.9, 0.8,
     "Only Option A allows the system to rebuild complete state from local DRAM alone after CXL loss.",
     BODY_FS, DG, bold=True, align=PP_ALIGN.LEFT)


# ════════════════════════════════════════════════
#  SLIDE 7: The tradeoff - which to pick
# ════════════════════════════════════════════════
slide = prs.slides.add_slide(prs.slide_layouts[6])
title(slide, "CXL Hardware Failure: The Fundamental Tradeoff")

# Fundamental tradeoff
box(slide, 0.5, 0.9, 12.3, 0.8, HI_L, HI, Pt(2))
text(slide, 0.7, 0.95, 11.9, 0.7,
     "CXL fault tolerance  <----------->  Write latency / throughput\n"
     "Option A highest                              Option C best",
     BODY_FS, DG, bold=True, align=PP_ALIGN.CENTER)

# Option 1
box(slide, 0.5, 2.0, 6.1, 3.0, GOOD_L, GOOD, Pt(2.5))
text(slide, 0.5, 2.05, 6.1, 0.45,
     "Choice 1: trust CXL hardware",
     BODY_FS, GOOD, bold=True, align=PP_ALIGN.CENTER)
text(slide, 0.8, 2.55, 5.5, 2.4,
     "- CXL failure = whole system failure (same as DRAM)\n"
     "- On restart rely on OpLog + BucketLock\n"
     "- Pick Option C (Lazy RC)\n"
     "- Write latency ~13 us\n"
     "- Write throughput ~77k/s per bucket",
     BODY_FS, DG, align=PP_ALIGN.LEFT)

# Option 2
box(slide, 6.8, 2.0, 6.1, 3.0, BAD_L, BAD, Pt(2.5))
text(slide, 6.8, 2.05, 6.1, 0.45,
     "Choice 2: treat CXL as unreliable",
     BODY_FS, BAD, bold=True, align=PP_ALIGN.CENTER)
text(slide, 7.1, 2.55, 5.5, 2.4,
     "- Local DRAM must hold complete replica\n"
     "- Pick Option A (Sync Repl)\n"
     "- Write latency ~20-30 us\n"
     "- Write throughput ~30-60k/s per bucket\n"
     "- System continues even if CXL is lost",
     BODY_FS, DG, align=PP_ALIGN.LEFT)

# Recommendation
box(slide, 0.5, 5.25, 12.3, 1.9, A_L, A_C, Pt(2.5))
text(slide, 0.7, 5.35, 11.9, 0.45,
     "Recommendation: Choice 1 (Option C / Lazy RC)",
     BODY_FS, A_C, bold=True, align=PP_ALIGN.LEFT)
text(slide, 0.7, 5.85, 11.9, 1.3,
     "- Reason 1: CXL is essentially a DRAM extension; reliability is on par; writing CXL is like writing memory\n"
     "- Reason 2: Choice 2's cost (2x latency, half throughput) is hard to justify in a paper\n"
     "- Reason 3: Real CXL fault tolerance requires cross-module redundancy, not software-layer emulation\n"
     "- Reason 4: the original FUSEE paper also does not handle 'all memory nodes dead simultaneously'",
     BODY_FS, DG, align=PP_ALIGN.LEFT)


# ── Save ──
out = "docs/rc_discussion.pptx"
prs.save(out)
print(f"Saved {out}")
print(f"Slides: {len(prs.slides)}")