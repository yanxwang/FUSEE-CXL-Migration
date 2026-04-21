#!/usr/bin/env python3
"""
Hashtable / Bucket / Slot architectural diagram + revised Step 3 (insert/update/delete).
"""

import os
from pptx import Presentation
from pptx.util import Inches, Pt, Emu
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

P_ROOT  = RGBColor(0x6A, 0x1B, 0x9A); P_ROOT_L = RGBColor(0xE1, 0xBE, 0xE7)
P_SUB   = RGBColor(0x15, 0x65, 0xC0); P_SUB_L  = RGBColor(0xBB, 0xDE, 0xFB)
P_BUCK  = RGBColor(0x2E, 0x7D, 0x32); P_BUCK_L = RGBColor(0xC8, 0xE6, 0xC9)
P_SLOT  = RGBColor(0xE6, 0x51, 0x00); P_SLOT_L = RGBColor(0xFF, 0xE0, 0xB2)
P_KV    = RGBColor(0xC6, 0x28, 0x28); P_KV_L   = RGBColor(0xFF, 0xCD, 0xD2)

P_OK    = RGBColor(0x1B, 0x5E, 0x20)
P_FAIL  = RGBColor(0xB7, 0x1C, 0x1C)
P_HI    = RGBColor(0xFF, 0x8F, 0x00)
P_HI_BG = RGBColor(0xC8, 0xE6, 0xC9)
P_VAR   = RGBColor(0xFF, 0xF9, 0xC4)


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
#  SLIDE 1: Architecture overview
# ════════════════════════════════════════════════
slide = prs.slides.add_slide(prs.slide_layouts[6])
_text(slide, 0, 0.2, 13.33, 0.6,
      "RACE Hash Table — Architectural Hierarchy", 18, P_DG, bold=True)

# ── Level 1: RaceHashRoot ──
_box(slide, 0.5, 1.2, 4.0, 1.6, P_ROOT_L, P_ROOT, Pt(2.5))
_text(slide, 0.5, 1.3, 4.0, 0.4, "RaceHashRoot", 12, P_ROOT, bold=True)
_text(slide, 0.6, 1.7, 3.8, 1.0,
      "global_depth = 5\n"
      "subtable_entry[32][replicas]\n"
      "  -> server_id + pointer\n"
      "(directory of subtables)", 9, P_DG, align=PP_ALIGN.LEFT)

# ── Level 2: Subtable ──
_box(slide, 5.0, 1.2, 4.0, 1.6, P_SUB_L, P_SUB, Pt(2.5))
_text(slide, 5.0, 1.3, 4.0, 0.4, "Subtable[i]", 12, P_SUB, bold=True)
_text(slide, 5.1, 1.7, 3.8, 1.0,
      "32 subtables total\n"
      "Each subtable holds\n"
      "  ~34000 RaceHashBuckets\n"
      "(array of buckets)", 9, P_DG, align=PP_ALIGN.LEFT)

# ── Level 3: Bucket ──
_box(slide, 9.5, 1.2, 3.5, 1.6, P_BUCK_L, P_BUCK, Pt(2.5))
_text(slide, 9.5, 1.3, 3.5, 0.4, "RaceHashBucket (64B)", 12, P_BUCK, bold=True)
_text(slide, 9.6, 1.7, 3.3, 1.0,
      "local_depth(4B)\n"
      "prefix(4B)\n"
      "slot[0..6]: 7 x 8B\n"
      "= 7 slots per bucket", 9, P_DG, align=PP_ALIGN.LEFT)

# Arrows between levels
_arrow(slide, 4.5, 2.0, 5.0, 2.0, P_DG, Pt(2))
_arrow(slide, 9.0, 2.0, 9.5, 2.0, P_DG, Pt(2))

# ── Level 4: Slot ──
_box(slide, 0.5, 3.4, 6.0, 1.5, P_SLOT_L, P_SLOT, Pt(2.5))
_text(slide, 0.5, 3.5, 6.0, 0.4, "RaceHashSlot (8 bytes)", 12, P_SLOT, bold=True)

# Slot layout - byte boxes
sx, sy = 0.7, 4.0
bw_byte = 0.65
bytes_layout = [
    ("fp", "1B", "fingerprint"),
    ("kv_len", "1B", "size in subblocks"),
    ("server_id", "1B", "which MN"),
    ("ptr[0]", "", ""),
    ("ptr[1]", "5B", "address (40-bit)"),
    ("ptr[2]", "", ""),
    ("ptr[3]", "", ""),
    ("ptr[4]", "", ""),
]
for i, (name, sz, desc) in enumerate(bytes_layout):
    _box(slide, sx + i*bw_byte, sy, bw_byte-0.05, 0.55, P_VAR, P_SLOT, Pt(1))
    _text(slide, sx + i*bw_byte, sy+0.1, bw_byte-0.05, 0.3, name, 7, P_DG, bold=True)
    if sz:
        _text(slide, sx + i*bw_byte, sy+0.32, bw_byte-0.05, 0.2, sz, 6, P_GRAY)

_text(slide, 0.5, 4.6, 6.0, 0.3,
      "fp=0 means EMPTY slot. ptr is encoded address on server_id.",
      8, P_GRAY, bold=False)

# ── Level 5: KV data ──
_box(slide, 7.0, 3.4, 6.0, 1.5, P_KV_L, P_KV, Pt(2.5))
_text(slide, 7.0, 3.5, 6.0, 0.4, "KV Data Block (variable)", 12, P_KV, bold=True)
_box(slide, 7.2, 4.0, 1.0, 0.55, P_VAR, P_KV, Pt(0.8), "Header\n(7B)", 7, P_DG)
_box(slide, 8.25, 4.0, 1.5, 0.55, P_VAR, P_KV, Pt(0.8), "Key\n(\"hello\")", 7, P_DG)
_box(slide, 9.8, 4.0, 1.5, 0.55, P_VAR, P_KV, Pt(0.8), "Value\n(\"world\")", 7, P_DG)
_box(slide, 11.35, 4.0, 1.5, 0.55, P_VAR, P_KV, Pt(0.8), "Tail\n(22B)", 7, P_DG)

# Arrow from slot ptr to KV
_arrow(slide, 6.5, 4.3, 7.0, 4.3, P_DG, Pt(1.5))

# ── Hierarchy summary ──
_box(slide, 0.5, 5.5, 12.3, 1.7, RGBColor(0xFF, 0xF8, 0xE1), P_HI, Pt(1.5))
_text(slide, 0.7, 5.6, 12.0, 0.3, "Lookup hierarchy:", 11, P_DG, bold=True, align=PP_ALIGN.LEFT)
_text(slide, 0.7, 5.95, 12.0, 1.2,
      "key='hello' --hash--> prefix(5 bits) --selects--> Subtable[i]\n"
      "                hash --bits--> bucket_idx --selects--> Bucket (one of 34000)\n"
      "                                          fp --matches--> Slot (one of 7)\n"
      "                                                          ptr --points to--> KV Data on Memory Node",
      9, P_DG, align=PP_ALIGN.LEFT)


# ════════════════════════════════════════════════
#  SLIDE 2: Concrete bucket example
# ════════════════════════════════════════════════
slide = prs.slides.add_slide(prs.slide_layouts[6])
_text(slide, 0, 0.2, 13.33, 0.6,
      "Concrete Example: One Bucket Holds 7 Different Keys", 18, P_DG, bold=True)

# Big bucket box
_box(slide, 1.0, 1.2, 11.3, 4.5, P_BUCK_L, P_BUCK, Pt(2.5))
_text(slide, 1.0, 1.3, 11.3, 0.4,
      "RaceHashBucket  (one bucket, 64 bytes total)", 13, P_BUCK, bold=True)

# Header bytes
_box(slide, 1.2, 1.85, 1.5, 0.5, P_VAR, P_BUCK, Pt(0.8),
     "local_depth\n(4B)", 8, P_DG)
_box(slide, 2.75, 1.85, 1.5, 0.5, P_VAR, P_BUCK, Pt(0.8),
     "prefix\n(4B)", 8, P_DG)

# 7 slots
slot_data = [
    ("0x3A", "4", "0", "0x10080000", "key='hello'", "occupied", P_OK),
    ("0x91", "2", "1", "0x10092000", "key='world'", "occupied", P_OK),
    ("0x4F", "8", "2", "0x100A4000", "key='foo'",   "occupied", P_OK),
    ("0x00", "0", "0", "0x00000000", "(empty)",     "EMPTY",   P_GRAY),
    ("0x7C", "4", "0", "0x100B0000", "key='bar'",   "occupied", P_OK),
    ("0x00", "0", "0", "0x00000000", "(empty)",     "EMPTY",   P_GRAY),
    ("0x00", "0", "0", "0x00000000", "(empty)",     "EMPTY",   P_GRAY),
]
for i, (fp, kvlen, sid, ptr, what, status, color) in enumerate(slot_data):
    sx = 1.2 + i*1.6
    sy = 2.5
    _box(slide, sx, sy, 1.5, 1.85, P_SLOT_L if status == "occupied" else P_LGRAY,
         color, Pt(2 if status == "occupied" else 0.8))
    _text(slide, sx, sy+0.08, 1.5, 0.3, f"slot[{i}]", 8, P_DG, bold=True)
    _text(slide, sx, sy+0.4, 1.5, 0.25, f"fp = {fp}", 7, P_DG, bold=False, align=PP_ALIGN.CENTER)
    _text(slide, sx, sy+0.65, 1.5, 0.25, f"kv_len = {kvlen}", 7, P_DG, align=PP_ALIGN.CENTER)
    _text(slide, sx, sy+0.9, 1.5, 0.25, f"server = {sid}", 7, P_DG, align=PP_ALIGN.CENTER)
    _text(slide, sx, sy+1.15, 1.5, 0.25, f"ptr={ptr[:6]}..", 6, P_GRAY, align=PP_ALIGN.CENTER)
    _text(slide, sx, sy+1.4, 1.5, 0.3, what, 8, color, bold=True)

# Note about bucket
_box(slide, 1.0, 6.0, 11.3, 1.2, RGBColor(0xFF, 0xF8, 0xE1), P_HI, Pt(1.5))
_text(slide, 1.2, 6.1, 11.0, 1.1,
      "Multiple different keys coexist in the same bucket. They were all hashed to this bucket.\n"
      "The 'fp' field is a 1-byte fingerprint used for fast match within the bucket (avoids reading every key).\n"
      "fp == 0 means EMPTY slot. INSERT looks for empty slot. UPDATE/SEARCH/DELETE looks for matching fp + verify key.",
      10, P_DG, bold=False, align=PP_ALIGN.LEFT)


# ════════════════════════════════════════════════
#  SLIDE 3: INSERT scans bucket for empty slot
# ════════════════════════════════════════════════
slide = prs.slides.add_slide(prs.slide_layouts[6])
_text(slide, 0, 0.2, 13.33, 0.6,
      "Step 3 (revised): INSERT scans bucket for EMPTY slot",
      18, P_OK, bold=True)

_text(slide, 0.5, 0.85, 12.3, 0.35,
      "Node 0 holds BucketLock, INSERT key='dragon' (hash matches this bucket)",
      11, P_DG, bold=True)

# Same bucket, but show INSERT scanning
_box(slide, 1.0, 1.4, 11.3, 3.5, P_BUCK_L, P_BUCK, Pt(2.5))
_text(slide, 1.0, 1.5, 11.3, 0.4,
      "RaceHashBucket  (locked by Node 0)", 12, P_BUCK, bold=True)

slot_data2 = [
    ("0x3A", "key='hello'", "occupied", P_GRAY, False),
    ("0x91", "key='world'", "occupied", P_GRAY, False),
    ("0x4F", "key='foo'",   "occupied", P_GRAY, False),
    ("0x00", "(empty)",     "FOUND!",  P_OK,   True),   # First empty - this is where we'll write
    ("0x7C", "key='bar'",   "occupied", P_GRAY, False),
    ("0x00", "(empty)",     "(skipped)", P_GRAY, False),
    ("0x00", "(empty)",     "(skipped)", P_GRAY, False),
]
for i, (fp, what, status, color, found) in enumerate(slot_data2):
    sx = 1.2 + i*1.6
    sy = 2.05
    bg = P_HI_BG if found else (P_SLOT_L if "key" in what else P_LGRAY)
    bw = Pt(3 if found else 0.8)
    _box(slide, sx, sy, 1.5, 2.25, bg, color, bw)
    _text(slide, sx, sy+0.1, 1.5, 0.3, f"slot[{i}]", 8, P_DG, bold=True)
    _text(slide, sx, sy+0.5, 1.5, 0.3, f"fp={fp}", 8, P_DG)
    _text(slide, sx, sy+0.85, 1.5, 0.3, what, 8, P_DG)
    _text(slide, sx, sy+1.45, 1.5, 0.4, status, 9, color, bold=True)
    if found:
        _text(slide, sx, sy+1.85, 1.5, 0.3, "<- WRITE\ndragon", 8, P_OK, bold=True)

# Logic box
_box(slide, 0.5, 5.3, 12.3, 1.8, RGBColor(0xFF, 0xF8, 0xE1), P_HI, Pt(1.5))
_text(slide, 0.7, 5.4, 12.0, 0.3, "INSERT logic (under BucketLock):", 11, P_OK, bold=True, align=PP_ALIGN.LEFT)
_text(slide, 0.7, 5.7, 12.0, 1.4,
      "1. Scan all 7 slots; if any has the same key as 'dragon' -> KEY_EXISTS, abort\n"
      "2. Find first slot with fp == 0 (empty)  -- here it's slot[3]\n"
      "3. Write new slot value: { fp=hash('dragon'), kv_len=4, server_id=0, ptr=&new_kv }\n"
      "4. If no empty slot in this bucket -> try next candidate bucket; if all 4 buckets full -> return TABLE_FULL",
      9, P_DG, align=PP_ALIGN.LEFT)


# ════════════════════════════════════════════════
#  SLIDE 4: UPDATE scans bucket for matching key
# ════════════════════════════════════════════════
slide = prs.slides.add_slide(prs.slide_layouts[6])
_text(slide, 0, 0.2, 13.33, 0.6,
      "Step 3 (revised): UPDATE scans bucket for MATCHING key",
      18, P_SUB, bold=True)

_text(slide, 0.5, 0.85, 12.3, 0.35,
      "Node 0 holds BucketLock, UPDATE key='foo' to new value",
      11, P_DG, bold=True)

_box(slide, 1.0, 1.4, 11.3, 3.5, P_BUCK_L, P_BUCK, Pt(2.5))
_text(slide, 1.0, 1.5, 11.3, 0.4,
      "RaceHashBucket  (locked by Node 0)", 12, P_BUCK, bold=True)

# fp for "foo" = 0x4F
slot_data3 = [
    ("0x3A", "key='hello'", "fp mismatch", P_GRAY, False),
    ("0x91", "key='world'", "fp mismatch", P_GRAY, False),
    ("0x4F", "key='foo'",   "MATCH!",      P_SUB,   True),
    ("0x00", "(empty)",     "(skipped)",   P_GRAY, False),
    ("0x7C", "key='bar'",   "fp mismatch", P_GRAY, False),
    ("0x00", "(empty)",     "(skipped)",   P_GRAY, False),
    ("0x00", "(empty)",     "(skipped)",   P_GRAY, False),
]
for i, (fp, what, status, color, found) in enumerate(slot_data3):
    sx = 1.2 + i*1.6
    sy = 2.05
    bg = P_HI_BG if found else (P_SLOT_L if "key" in what else P_LGRAY)
    bw = Pt(3 if found else 0.8)
    _box(slide, sx, sy, 1.5, 2.25, bg, color, bw)
    _text(slide, sx, sy+0.1, 1.5, 0.3, f"slot[{i}]", 8, P_DG, bold=True)
    _text(slide, sx, sy+0.5, 1.5, 0.3, f"fp={fp}", 8, P_DG)
    _text(slide, sx, sy+0.85, 1.5, 0.3, what, 8, P_DG)
    _text(slide, sx, sy+1.45, 1.5, 0.4, status, 9, color, bold=True)
    if found:
        _text(slide, sx, sy+1.85, 1.5, 0.3, "ptr -> new", 8, P_SUB, bold=True)

_box(slide, 0.5, 5.3, 12.3, 1.8, RGBColor(0xFF, 0xF8, 0xE1), P_HI, Pt(1.5))
_text(slide, 0.7, 5.4, 12.0, 0.3, "UPDATE logic (under BucketLock):", 11, P_SUB, bold=True, align=PP_ALIGN.LEFT)
_text(slide, 0.7, 5.7, 12.0, 1.4,
      "1. Compute fp = hash_fp('foo') = 0x4F\n"
      "2. Scan all 7 slots; for each slot with matching fp, verify the actual key (read KV data, compare)\n"
      "3. Found at slot[2] -> write new ptr: { fp=0x4F, kv_len=new, server_id=0, ptr=&new_kv }\n"
      "4. Mark old KV block for GC. If no matching key found in any of the 4 buckets -> return KEY_NOT_FOUND",
      9, P_DG, align=PP_ALIGN.LEFT)


# ════════════════════════════════════════════════
#  SLIDE 5: Comparison of FUSEE vs new design
# ════════════════════════════════════════════════
slide = prs.slides.add_slide(prs.slide_layouts[6])
_text(slide, 0, 0.2, 13.33, 0.6,
      "FUSEE (RDMA CAS) vs New CXL (Per-Bucket LFM)", 18, P_DG, bold=True)

# Two columns
col_w = 6.0
y0 = 1.2
left_x = 0.5
right_x = 6.8

_box(slide, left_x, y0, col_w, 0.5, P_KV_L, P_KV, Pt(2),
     "FUSEE (RDMA CAS, optimistic)", 12, P_KV)
_box(slide, right_x, y0, col_w, 0.5, P_BUCK_L, P_BUCK, Pt(2),
     "New CXL (Per-Bucket LFM, pessimistic)", 12, P_BUCK)

# INSERT
_box(slide, left_x, 1.85, col_w, 1.7, P_LGRAY, P_KV, Pt(0.8))
_text(slide, left_x+0.1, 1.9, col_w-0.2, 0.3, "INSERT", 10, P_KV, bold=True, align=PP_ALIGN.LEFT)
_text(slide, left_x+0.1, 2.2, col_w-0.2, 1.4,
      "1. RDMA READ 4 buckets\n"
      "2. Find empty slot (fp=0)\n"
      "3. RDMA CAS slot: 0x0 -> new\n"
      "4. If CAS fails -> retry with another empty slot\n"
      "5. If all slots full -> TABLE_FULL",
      9, P_DG, align=PP_ALIGN.LEFT)

_box(slide, right_x, 1.85, col_w, 1.7, P_LGRAY, P_BUCK, Pt(0.8))
_text(slide, right_x+0.1, 1.9, col_w-0.2, 0.3, "INSERT", 10, P_BUCK, bold=True, align=PP_ALIGN.LEFT)
_text(slide, right_x+0.1, 2.2, col_w-0.2, 1.4,
      "1. shm_mutex_lock(BucketLock[idx])\n"
      "2. Scan bucket for empty slot (fp=0)\n"
      "3. Write new slot value directly\n"
      "4. shm_mutex_unlock\n"
      "5. If no empty slot -> try next candidate bucket; "
      "if all full -> TABLE_FULL",
      9, P_DG, align=PP_ALIGN.LEFT)

# UPDATE
_box(slide, left_x, 3.7, col_w, 1.7, P_LGRAY, P_KV, Pt(0.8))
_text(slide, left_x+0.1, 3.75, col_w-0.2, 0.3, "UPDATE", 10, P_KV, bold=True, align=PP_ALIGN.LEFT)
_text(slide, left_x+0.1, 4.05, col_w-0.2, 1.4,
      "1. RDMA READ 4 buckets\n"
      "2. Find slot with matching fp + key\n"
      "3. RDMA CAS slot: old_ptr -> new_ptr\n"
      "4. If CAS fails (concurrent update) -> retry\n"
      "5. If key not found -> KEY_NOT_FOUND",
      9, P_DG, align=PP_ALIGN.LEFT)

_box(slide, right_x, 3.7, col_w, 1.7, P_LGRAY, P_BUCK, Pt(0.8))
_text(slide, right_x+0.1, 3.75, col_w-0.2, 0.3, "UPDATE", 10, P_BUCK, bold=True, align=PP_ALIGN.LEFT)
_text(slide, right_x+0.1, 4.05, col_w-0.2, 1.4,
      "1. shm_mutex_lock(BucketLock[idx])\n"
      "2. Scan bucket for matching fp + key\n"
      "3. Write new slot value directly\n"
      "4. shm_mutex_unlock\n"
      "5. If key not found -> KEY_NOT_FOUND",
      9, P_DG, align=PP_ALIGN.LEFT)

# Tradeoff
_box(slide, left_x, 5.55, col_w, 1.6, RGBColor(0xFF, 0xEB, 0xEE), P_KV, Pt(1.2))
_text(slide, left_x+0.1, 5.6, col_w-0.2, 0.3, "Tradeoffs", 10, P_KV, bold=True, align=PP_ALIGN.LEFT)
_text(slide, left_x+0.1, 5.9, col_w-0.2, 1.3,
      "+ No locks, no waiting\n"
      "- Multiple retries under contention\n"
      "- Hardware CAS atomicity required\n"
      "- Each operation may need 2-3 RDMA CAS",
      8, P_DG, align=PP_ALIGN.LEFT)

_box(slide, right_x, 5.55, col_w, 1.6, RGBColor(0xE8, 0xF5, 0xE9), P_BUCK, Pt(1.2))
_text(slide, right_x+0.1, 5.6, col_w-0.2, 0.3, "Tradeoffs", 10, P_BUCK, bold=True, align=PP_ALIGN.LEFT)
_text(slide, right_x+0.1, 5.9, col_w-0.2, 1.3,
      "+ No retries (lock holder makes progress)\n"
      "+ Works without hardware atomics\n"
      "+ Simpler reasoning (mutual exclusion)\n"
      "- Spin-waiting under contention",
      8, P_DG, align=PP_ALIGN.LEFT)


# ── Save ──
out = "docs/hashtable_architecture.pptx"
prs.save(out)
print(f"Saved {out}")
print(f"Slides: {len(prs.slides)}")