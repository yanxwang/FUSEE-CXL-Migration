#!/usr/bin/env python3
"""
CXL Staging Buffer and OpLog layout slides.
Focus: data structure layout + concurrent write handling.
"""

import os
from pptx import Presentation
from pptx.util import Inches, Pt
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN
from pptx.enum.shapes import MSO_SHAPE
from pptx.oxml.ns import qn

os.makedirs("docs", exist_ok=True)

# ── Colors ──
P_WHITE = RGBColor(0xFF, 0xFF, 0xFF)
P_DG    = RGBColor(0x33, 0x33, 0x33)
P_GRAY  = RGBColor(0x9E, 0x9E, 0x9E)
P_LGRAY = RGBColor(0xEE, 0xEE, 0xEE)

P_N0    = RGBColor(0x15, 0x65, 0xC0); P_N0_L = RGBColor(0xBB, 0xDE, 0xFB)
P_N1    = RGBColor(0xC6, 0x28, 0x28); P_N1_L = RGBColor(0xFF, 0xCD, 0xD2)
P_N2    = RGBColor(0x6A, 0x1B, 0x9A); P_N2_L = RGBColor(0xE1, 0xBE, 0xE7)

P_CXL   = RGBColor(0xE6, 0x51, 0x00); P_CXL_L = RGBColor(0xFF, 0xE0, 0xB2)
P_DATA  = RGBColor(0x15, 0x65, 0xC0); P_DATA_L = RGBColor(0xE3, 0xF2, 0xFD)
P_LOG   = RGBColor(0x6A, 0x1B, 0x9A); P_LOG_L = RGBColor(0xF3, 0xE5, 0xF5)
P_HDR   = RGBColor(0xFF, 0x6F, 0x00); P_HDR_L = RGBColor(0xFF, 0xE0, 0xB2)
P_VAR   = RGBColor(0xFF, 0xF9, 0xC4)

P_OK    = RGBColor(0x1B, 0x5E, 0x20)
P_FAIL  = RGBColor(0xB7, 0x1C, 0x1C)
P_HI    = RGBColor(0xFF, 0x8F, 0x00)


def _in(v):
    return Inches(v)

def _box(slide, l, t, w, h, fc, ec=None, bw=Pt(1.5), text="", fs=9, c=P_DG, bold=True,
         shape=MSO_SHAPE.ROUNDED_RECTANGLE):
    s = slide.shapes.add_shape(shape, _in(l), _in(t), _in(w), _in(h))
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
    return s

def _text(slide, l, t, w, h, text, fs=9, c=P_DG, bold=False, align=PP_ALIGN.CENTER):
    tb = slide.shapes.add_textbox(_in(l), _in(t), _in(w), _in(h))
    tf = tb.text_frame; tf.word_wrap = True
    p = tf.paragraphs[0]; p.alignment = align
    run = p.add_run(); run.text = text
    run.font.size = Pt(fs); run.font.color.rgb = c; run.font.bold = bold

def _arrow(slide, x0, y0, x1, y1, c=P_DG, w=Pt(2), dashed=False):
    conn = slide.shapes.add_connector(1, _in(x0), _in(y0), _in(x1), _in(y1))
    conn.line.color.rgb = c; conn.line.width = w
    if dashed:
        conn.line.dash_style = 2
    fmt = conn.line._ln
    tail = fmt.makeelement(qn('a:tailEnd'), {})
    tail.set('type', 'triangle'); tail.set('w', 'med'); tail.set('len', 'med')
    fmt.append(tail)


prs = Presentation()
prs.slide_width = Inches(13.33)
prs.slide_height = Inches(7.5)


# ════════════════════════════════════════════════
#  SLIDE 1: CXL Staging Buffer Layout
# ════════════════════════════════════════════════
slide = prs.slides.add_slide(prs.slide_layouts[6])
_text(slide, 0, 0.2, 13.33, 0.6,
      "CXL Staging Buffer — Layout and Concurrent Writes", 18, P_DG, bold=True)

# Subtitle
_text(slide, 0, 0.75, 13.33, 0.35,
      "Per-node single-producer ring buffers (no inter-node contention)",
      11, P_GRAY, bold=False)

# ── Three nodes (top) ──
node_w, node_h = 2.6, 0.8
ny = 1.25
node_specs = [
    (1.5,  "Node 0 (writer)", P_N0, P_N0_L),
    (5.4,  "Node 1 (writer)", P_N1, P_N1_L),
    (9.3,  "Node 2 (writer)", P_N2, P_N2_L),
]
for nx, t, c, cl in node_specs:
    _box(slide, nx, ny, node_w, node_h, cl, c, Pt(2), t, 11, c)

# ── CXL Staging area (bottom) ──
cy = 2.9
cw = 12.3
ch = 3.2
_box(slide, 0.5, cy, cw, ch, P_CXL_L, P_CXL, Pt(2.5))
_text(slide, 0.5, cy+0.05, cw, 0.35,
      "CXL Shared Memory: DataStagingArea", 12, P_CXL, bold=True)

# Per-node staging regions
region_w = 4.0
region_h = 2.55
region_y = cy + 0.45
gaps = [(0.6, P_N0_L, P_N0, "Node 0's region", "8a", "8a", "8a"),
        (4.65, P_N1_L, P_N1, "Node 1's region", "0",  "0",  "0"),
        (8.7,  P_N2_L, P_N2, "Node 2's region", "12", "8",  "0")]

for rx, fc, ec, title, head, tail, _ in gaps:
    # outer
    _box(slide, rx, region_y, region_w, region_h, fc, ec, Pt(2))
    _text(slide, rx, region_y+0.05, region_w, 0.3, title, 10, ec, bold=True)
    # header (head/tail/capacity)
    hdr_y = region_y + 0.4
    _box(slide, rx+0.1, hdr_y, region_w-0.2, 0.45, P_HDR_L, P_HDR, Pt(1),
         f"head={head}   tail={tail}   capacity=64MB", 8, P_DG)
    # entries
    _text(slide, rx+0.15, hdr_y+0.5, region_w-0.3, 0.25,
          "StagingEntry[]:", 8, P_DG, bold=True, align=PP_ALIGN.LEFT)

# Node 0 entries (full)
e_y = region_y + 1.2
e_w = 3.7
for i, (eid, op) in enumerate([(8, "hello:world"), (9, "foo:bar")]):
    _box(slide, 0.7, e_y + i*0.42, e_w, 0.38, P_VAR, P_N0, Pt(0.8),
         f"id={eid}  size={len(op)+30}  data=\"{op}\"", 7, P_DG)

# Node 1 entries (empty)
_text(slide, 4.75, e_y+0.5, region_w-0.2, 0.3, "(no pending writes)", 8, P_GRAY, align=PP_ALIGN.CENTER)

# Node 2 entries
_box(slide, 8.8, e_y, e_w, 0.38, P_VAR, P_N2, Pt(0.8),
     "id=8  size=42  data=\"key:val\"", 7, P_DG)

# ── Arrows ──
# Each node writes ONLY to its own region
_arrow(slide, 1.5+node_w/2, ny+node_h, 0.6+region_w/2, region_y, P_N0, Pt(2.5))
_arrow(slide, 5.4+node_w/2, ny+node_h, 4.65+region_w/2, region_y, P_N1, Pt(2.5))
_arrow(slide, 9.3+node_w/2, ny+node_h, 8.7+region_w/2, region_y, P_N2, Pt(2.5))

# Cross-region read arrows (other nodes read each other's regions)
# Node 1 reads Node 0's region (dashed)
_arrow(slide, 0.6+region_w/2, region_y, 5.4+node_w/2-0.3, ny+node_h, P_N1, Pt(1), dashed=True)
# Node 2 reads Node 0's region
_arrow(slide, 0.6+region_w/2+0.3, region_y, 9.3+node_w/2-0.3, ny+node_h, P_N2, Pt(1), dashed=True)

# Bottom note
_box(slide, 0.5, 6.3, 12.3, 0.95, RGBColor(0xFF, 0xF8, 0xE1), P_HI, Pt(1.5))
_text(slide, 0.7, 6.35, 12.0, 0.3,
      "Concurrent write handling — partition by writer:",
      10, P_DG, bold=True, align=PP_ALIGN.LEFT)
_text(slide, 0.7, 6.65, 12.0, 0.6,
      "* Each node owns one ring buffer region. Only the owner writes (single producer) -> no contention, no lock needed.\n"
      "* Owner advances 'tail' after each store + sfence. Other nodes are read-only consumers (track per-consumer 'head').",
      9, P_DG, bold=False, align=PP_ALIGN.LEFT)


# ════════════════════════════════════════════════
#  SLIDE 2: Staging Entry Format + Write Protocol
# ════════════════════════════════════════════════
slide = prs.slides.add_slide(prs.slide_layouts[6])
_text(slide, 0, 0.2, 13.33, 0.6,
      "CXL Staging Buffer — Entry Format and Write Protocol",
      18, P_DG, bold=True)

# ── StagingHeader ──
_box(slide, 0.5, 1.1, 12.3, 0.95, P_HDR_L, P_HDR, Pt(2))
_text(slide, 0.5, 1.15, 12.3, 0.3, "StagingHeader (cache-line aligned, 64B)",
      11, P_HDR, bold=True)

hdr_fields = [("head_off", "uint64_t", "8B", "consumer cursor"),
              ("tail_off", "uint64_t", "8B", "producer cursor"),
              ("capacity", "uint64_t", "8B", "ring size"),
              ("entry_seq", "uint64_t", "8B", "monotonic id")]
fx = 0.7
fw = 3.0
for i, (name, ty, sz, desc) in enumerate(hdr_fields):
    _box(slide, fx + i*fw, 1.45, fw-0.1, 0.5, P_VAR, P_HDR, Pt(0.8),
         f"{name}\n{ty}  ({sz})", 8, P_DG)

# ── StagingEntry ──
_box(slide, 0.5, 2.4, 12.3, 1.6, P_DATA_L, P_DATA, Pt(2))
_text(slide, 0.5, 2.45, 12.3, 0.3, "StagingEntry (variable size)",
      11, P_DATA, bold=True)

entry_fields = [
    ("entry_id", "8B", "matches OpLog id"),
    ("size", "4B", "payload bytes"),
    ("flags", "4B", "READY / IN_USE"),
    ("crc", "8B", "checksum"),
    ("payload[]", "var", "actual KV data"),
]
fx = 0.7
fw = 2.4
for i, (name, sz, desc) in enumerate(entry_fields):
    _box(slide, fx + i*fw, 2.85, fw-0.1, 0.55, P_VAR, P_DATA, Pt(0.8),
         f"{name}\n({sz})", 8, P_DG)
    _text(slide, fx + i*fw, 3.45, fw-0.1, 0.4, desc, 7, P_GRAY)

# ── Write protocol ──
_box(slide, 0.5, 4.3, 6.0, 2.7, P_DATA_L, P_DATA, Pt(2))
_text(slide, 0.5, 4.35, 6.0, 0.3,
      "Producer (owner node) write protocol", 11, P_DATA, bold=True)
_text(slide, 0.65, 4.7, 5.7, 2.2,
      "1. Reserve slot: e_off = tail_off\n"
      "2. Compute size needed; check space\n"
      "3. Store entry header:\n"
      "     entry_id, size, crc\n"
      "     flags = IN_USE\n"
      "4. Store payload (memcpy + flush_region)\n"
      "5. sfence\n"
      "6. Set flags = READY (single store)\n"
      "7. Advance tail_off += entry_size\n"
      "8. sfence\n"
      "Reader waits until flags == READY",
      9, P_DG, align=PP_ALIGN.LEFT)

# ── Read protocol ──
_box(slide, 6.8, 4.3, 6.0, 2.7, P_LGRAY, P_GRAY, Pt(1.5))
_text(slide, 6.8, 4.35, 6.0, 0.3,
      "Consumer (other nodes) read protocol", 11, P_GRAY, bold=True)
_text(slide, 6.95, 4.7, 5.7, 2.2,
      "1. Each consumer keeps own local_head\n"
      "2. Periodically poll: load tail_off\n"
      "3. While local_head < tail_off:\n"
      "     load entry header at local_head\n"
      "     wait until flags == READY\n"
      "     verify crc\n"
      "     copy payload to local DRAM\n"
      "     advance local_head\n"
      "4. Owner can reclaim space when ALL\n"
      "   consumers' local_head >= tail (GC)",
      9, P_DG, align=PP_ALIGN.LEFT)


# ════════════════════════════════════════════════
#  SLIDE 3: CXL OpLog Layout
# ════════════════════════════════════════════════
slide = prs.slides.add_slide(prs.slide_layouts[6])
_text(slide, 0, 0.2, 13.33, 0.6,
      "CXL OpLog — Layout and Concurrent Writes", 18, P_DG, bold=True)
_text(slide, 0, 0.75, 13.33, 0.35,
      "Per-node single-producer log (for crash recovery)",
      11, P_GRAY, bold=False)

# ── Three nodes ──
ny = 1.25
for nx, t, c, cl in node_specs:
    _box(slide, nx, ny, node_w, node_h, cl, c, Pt(2), t, 11, c)

# ── CXL OpLog area ──
cy = 2.9
cw = 12.3
ch = 3.3
_box(slide, 0.5, cy, cw, ch, P_LOG_L, P_LOG, Pt(2.5))
_text(slide, 0.5, cy+0.05, cw, 0.35,
      "CXL Shared Memory: OperationLog", 12, P_LOG, bold=True)

# Per-node OpLog regions
region_w = 4.0
region_h = 2.65
region_y = cy + 0.45

oplog_specs = [
    (0.6, P_N0_L, P_N0, "Node 0's OpLog", [
        ("epoch=42", "INSERT", "COMMITTED"),
        ("epoch=43", "INSERT", "IN_PROGRESS"),
    ]),
    (4.65, P_N1_L, P_N1, "Node 1's OpLog", [
        ("epoch=10", "UPDATE", "COMMITTED"),
    ]),
    (8.7, P_N2_L, P_N2, "Node 2's OpLog", [
        ("epoch=88", "DELETE", "COMMITTED"),
        ("epoch=89", "INSERT", "COMMITTED"),
    ]),
]

for rx, fc, ec, title, entries in oplog_specs:
    _box(slide, rx, region_y, region_w, region_h, fc, ec, Pt(2))
    _text(slide, rx, region_y+0.05, region_w, 0.3, title, 10, ec, bold=True)
    # header
    hdr_y = region_y + 0.4
    _box(slide, rx+0.1, hdr_y, region_w-0.2, 0.4, P_HDR_L, P_HDR, Pt(1),
         f"head=10  tail=12  cap=80MB", 8, P_DG)
    # entries
    e_y = region_y + 0.95
    for i, (epoch, op, status) in enumerate(entries):
        col = P_OK if status == "COMMITTED" else P_HI
        _box(slide, rx+0.15, e_y + i*0.55, region_w-0.3, 0.5,
             P_VAR, col, Pt(1.2 if status == "IN_PROGRESS" else 0.7),
             f"{epoch}  op={op}\nstatus={status}", 7, P_DG)

# ── Arrows ──
_arrow(slide, 1.5+node_w/2, ny+node_h, 0.6+region_w/2, region_y, P_N0, Pt(2.5))
_arrow(slide, 5.4+node_w/2, ny+node_h, 4.65+region_w/2, region_y, P_N1, Pt(2.5))
_arrow(slide, 9.3+node_w/2, ny+node_h, 8.7+region_w/2, region_y, P_N2, Pt(2.5))

# Recovery read (e.g., Node 0 crashes, Node 1 reads its OpLog)
_arrow(slide, 0.6+region_w/2+0.5, region_y, 5.4+node_w/2-0.3, ny+node_h,
       P_N1, Pt(1), dashed=True)
_text(slide, 3.0, 2.4, 3.0, 0.3,
      "(recovery: read others' OpLogs)", 8, P_GRAY, bold=False)

# Bottom note
_box(slide, 0.5, 6.4, 12.3, 0.85, RGBColor(0xFF, 0xF8, 0xE1), P_HI, Pt(1.5))
_text(slide, 0.7, 6.45, 12.0, 0.3,
      "Concurrent writes — same trick: each node owns one log",
      10, P_DG, bold=True, align=PP_ALIGN.LEFT)
_text(slide, 0.7, 6.75, 12.0, 0.5,
      "* No two nodes write to the same OpLog -> no lock, no contention.\n"
      "* On crash: surviving nodes read the failed node's OpLog (read-only) to redo/abort its in-progress operations.",
      9, P_DG, bold=False, align=PP_ALIGN.LEFT)


# ════════════════════════════════════════════════
#  SLIDE 4: OpLog Entry Format
# ════════════════════════════════════════════════
slide = prs.slides.add_slide(prs.slide_layouts[6])
_text(slide, 0, 0.2, 13.33, 0.6,
      "CXL OpLog — Entry Format and Lifecycle",
      18, P_DG, bold=True)

# ── Entry struct ──
_box(slide, 0.5, 1.1, 12.3, 3.4, P_LOG_L, P_LOG, Pt(2))
_text(slide, 0.5, 1.15, 12.3, 0.3,
      "OpLogEntry (fixed 128B, cache-line aligned)", 11, P_LOG, bold=True)

entry_struct = [
    [("epoch", "uint64_t", "8B", "monotonic per-node sequence"),
     ("op_type", "uint8_t", "1B", "INSERT / UPDATE / DELETE"),
     ("status", "uint8_t", "1B", "IN_PROGRESS / COMMITTED / DONE"),
     ("node_id", "uint8_t", "1B", "owner node id"),
     ("pad", "-", "5B", "alignment")],
    [("key_hash", "uint64_t", "8B", "hash(key) for fast lookup"),
     ("bucket_idx", "uint64_t", "8B", "which bucket"),
     ("slot_idx", "uint16_t", "2B", "which slot in bucket"),
     ("pad", "-", "6B", "alignment")],
    [("old_slot_value", "uint64_t", "8B", "previous slot (for verify)"),
     ("new_slot_value", "uint64_t", "8B", "target slot value"),
     ("staging_offset", "uint64_t", "8B", "ptr into Staging Buffer"),
     ("payload_size", "uint32_t", "4B", "KV size"),
     ("crc", "uint32_t", "4B", "integrity check")],
]

for row, fields in enumerate(entry_struct):
    fx = 0.7
    fw = 2.4
    fy = 1.55 + row * 0.95
    for i, (name, ty, sz, desc) in enumerate(fields):
        _box(slide, fx + i*fw, fy, fw-0.1, 0.78, P_VAR, P_LOG, Pt(0.8))
        _text(slide, fx + i*fw, fy+0.05, fw-0.1, 0.25, name, 8, P_DG, bold=True)
        _text(slide, fx + i*fw, fy+0.3, fw-0.1, 0.25, f"{ty}  ({sz})", 7, P_GRAY)
        _text(slide, fx + i*fw, fy+0.55, fw-0.1, 0.25, desc, 6, P_GRAY)

# ── Lifecycle ──
_box(slide, 0.5, 4.7, 12.3, 2.5, RGBColor(0xFF, 0xF8, 0xE1), P_HI, Pt(1.5))
_text(slide, 0.7, 4.75, 12.0, 0.3,
      "Entry lifecycle (matches consensus Steps 4-7)", 11, P_HI, bold=True, align=PP_ALIGN.LEFT)

# 4 lifecycle states
states = [
    ("(empty)", P_LGRAY, P_GRAY, "Step 1-3:\nbefore write"),
    ("IN_PROGRESS", RGBColor(0xFF,0xE0,0xB2), P_HI,
     "Step 4: writer\nstores full entry\nflush + sfence"),
    ("COMMITTED", RGBColor(0xC8,0xE6,0xC9), P_OK,
     "Step 5: after\nslot value written\n(commit point)"),
    ("DONE", P_LGRAY, P_GRAY,
     "Step 7: after\nall replicas applied\n(GC eligible)"),
]
sx = 0.8
sw = 2.95
sy = 5.15
for i, (name, fc, ec, desc) in enumerate(states):
    _box(slide, sx + i*(sw+0.1), sy, sw, 0.6, fc, ec, Pt(2),
         name, 11, ec, bold=True)
    _text(slide, sx + i*(sw+0.1), sy+0.65, sw, 1.3, desc, 8, P_DG, align=PP_ALIGN.CENTER)
    if i < len(states) - 1:
        ax_x = sx + (i+1)*(sw+0.1) - 0.05
        _arrow(slide, ax_x, sy+0.3, ax_x+0.15, sy+0.3, P_DG, Pt(1.5))


# ── Save ──
out = "docs/cxl_staging_oplog.pptx"
prs.save(out)
print(f"Saved {out}")
print(f"Slides: {len(prs.slides)}")
