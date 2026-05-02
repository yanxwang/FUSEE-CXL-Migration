#!/usr/bin/env python3
"""
Generate summary PPTX of real CXL experimental results for A/B/C protocols.
"""

import os
from pptx import Presentation
from pptx.util import Inches, Pt
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN, MSO_ANCHOR
from pptx.enum.shapes import MSO_SHAPE

os.makedirs("docs", exist_ok=True)

WHITE = RGBColor(0xFF, 0xFF, 0xFF)
DG = RGBColor(0x33, 0x33, 0x33)
GRAY = RGBColor(0x9E, 0x9E, 0x9E)
LGRAY = RGBColor(0xF5, 0xF5, 0xF5)
A_C = RGBColor(0xC6, 0x28, 0x28); A_L = RGBColor(0xFF, 0xCD, 0xD2)
B_C = RGBColor(0xE6, 0x51, 0x00); B_L = RGBColor(0xFF, 0xE0, 0xB2)
C_C = RGBColor(0x2E, 0x7D, 0x32); C_L = RGBColor(0xC8, 0xE6, 0xC9)
HI = RGBColor(0xFF, 0x8F, 0x00)
HI_BG = RGBColor(0xFF, 0xF8, 0xE1)

TITLE_FS = 22
BODY_FS = 14
CELL_FS = 12

def box(slide, l, t, w, h, fc, ec=None, bw=Pt(1.5), text="", fs=BODY_FS, c=DG, bold=False):
    s = slide.shapes.add_shape(MSO_SHAPE.ROUNDED_RECTANGLE,
                               Inches(l), Inches(t), Inches(w), Inches(h))
    s.fill.solid(); s.fill.fore_color.rgb = fc
    if ec:
        s.line.color.rgb = ec; s.line.width = bw
    else:
        s.line.fill.background()
    if text:
        tf = s.text_frame; tf.word_wrap = True
        p = tf.paragraphs[0]; p.alignment = PP_ALIGN.CENTER
        run = p.add_run(); run.text = text
        run.font.size = Pt(fs); run.font.color.rgb = c; run.font.bold = bold

def text(slide, l, t, w, h, txt, fs=BODY_FS, c=DG, bold=False, align=PP_ALIGN.LEFT):
    tb = slide.shapes.add_textbox(Inches(l), Inches(t), Inches(w), Inches(h))
    tf = tb.text_frame; tf.word_wrap = True
    p = tf.paragraphs[0]; p.alignment = align
    run = p.add_run(); run.text = txt
    run.font.size = Pt(fs); run.font.color.rgb = c; run.font.bold = bold

def title(slide, txt, color=DG):
    text(slide, 0.4, 0.2, 12.5, 0.6, txt, TITLE_FS, color, bold=True, align=PP_ALIGN.CENTER)

def cell(c, txt, fs=CELL_FS, bold=False, color=DG, fill=None, align=PP_ALIGN.CENTER):
    if fill is not None:
        c.fill.solid(); c.fill.fore_color.rgb = fill
    c.vertical_anchor = MSO_ANCHOR.MIDDLE
    c.margin_left = Inches(0.08); c.margin_right = Inches(0.08)
    c.margin_top = Inches(0.04); c.margin_bottom = Inches(0.04)
    tf = c.text_frame; tf.word_wrap = True
    p = tf.paragraphs[0]; p.alignment = align
    for r in list(p.runs): r.text = ""
    r = p.add_run(); r.text = txt
    r.font.size = Pt(fs); r.font.bold = bold; r.font.color.rgb = color

prs = Presentation()
prs.slide_width = Inches(13.33)
prs.slide_height = Inches(7.5)

# ───────────────────────────────────────────
# Slide 1: Title
# ───────────────────────────────────────────
slide = prs.slides.add_slide(prs.slide_layouts[6])
text(slide, 0, 2.5, 13.33, 1.0,
     "CXL-FUSEE A/B/C Protocol Comparison",
     32, DG, bold=True, align=PP_ALIGN.CENTER)
text(slide, 0, 3.5, 13.33, 0.5,
     "Real Hardware Experimental Results",
     20, GRAY, align=PP_ALIGN.CENTER)
text(slide, 0, 4.2, 13.33, 0.5,
     "Intel Xeon 6787P + XConn XC50256 CXL switch + CXL Type 3 Memory",
     16, GRAY, align=PP_ALIGN.CENTER)
text(slide, 0, 4.7, 13.33, 0.5,
     "2026-04-19",
     14, GRAY, align=PP_ALIGN.CENTER)

# ───────────────────────────────────────────
# Slide 2: Setup
# ───────────────────────────────────────────
slide = prs.slides.add_slide(prs.slide_layouts[6])
title(slide, "Experimental Setup")

text(slide, 0.5, 1.0, 12.3, 0.5, "Hardware", BODY_FS+2, DG, bold=True)
text(slide, 0.7, 1.6, 12.0, 2.0,
     "- 2 machines: g3, g4 (Intel Xeon 6787P, 800 GB DRAM)\n"
     "- CXL switch: XConn Technologies XC50256\n"
     "- CXL memory: 512 GiB, target_node=1\n"
     "- Devdax devices: /dev/dax0.0 (256 GB), /dev/dax0.1 (128 GB), /dev/dax0.2 (128 GB)\n"
     "- g3 dax0.0 and g4 dax0.0 map to the same physical address 0x4080000000\n"
     "  -> hardware supports cross-node shared memory via CXL switch",
     BODY_FS, DG)

text(slide, 0.5, 4.0, 12.3, 0.5, "Software", BODY_FS+2, DG, bold=True)
text(slide, 0.7, 4.6, 12.0, 2.0,
     "- cxl_shm_profiling library (LFM mutex on non-coherent CXL)\n"
     "- Custom bench: ycsb_abc_bench.c (~700 lines, 3 protocols compile-time switched)\n"
     "- Single-host multi-process setup (4 processes simulate 4 nodes)\n"
     "- Cross-node ultimately blocked: g4 dax0.0 needed reboot to recover",
     BODY_FS, DG)

# ───────────────────────────────────────────
# Slide 3: CXL latency baseline
# ───────────────────────────────────────────
slide = prs.slides.add_slide(prs.slide_layouts[6])
title(slide, "Baseline: CXL vs DRAM Latency (g3)")

rows, cols = 5, 4
left, top = Inches(1.5), Inches(1.3)
width, height = Inches(10), Inches(3.5)
ts = slide.shapes.add_table(rows, cols, left, top, width, height)
tbl = ts.table

headers = ["Operation", "DRAM (tmpfs)", "CXL (/dev/dax0.0)", "CXL / DRAM"]
for i, h in enumerate(headers):
    cell(tbl.cell(0, i), h, fs=BODY_FS, bold=True, color=WHITE, fill=DG)

rows_data = [
    ("plain store (cached)",     "4.3 ns",   "7.2 ns",   "1.7x"),
    ("plain load (cached)",      "4.2 ns",   "8.1 ns",   "1.9x"),
    ("CACHELINE_STORE +flush+sfence", "255.7 ns", "1558.7 ns", "6.1x"),
    ("CACHELINE_LOAD +flush+mfence",  "322.5 ns", "700.0 ns",  "2.2x"),
]
for ri, row in enumerate(rows_data, start=1):
    for ci, v in enumerate(row):
        cell(tbl.cell(ri, ci), v, fs=CELL_FS,
             bold=(ci == 0), fill=(LGRAY if ri%2==0 else WHITE))

box(slide, 0.5, 5.2, 12.3, 1.5, HI_BG, HI, Pt(1.5))
text(slide, 0.7, 5.3, 11.9, 0.4,
     "Key observation",
     BODY_FS, HI, bold=True)
text(slide, 0.7, 5.75, 11.9, 0.9,
     "CXL flush to memory is 6x slower than flush to DRAM (which goes to pagecache).\n"
     "This makes every cross-node visibility primitive roughly 2-6x more expensive.",
     BODY_FS, DG)

# ───────────────────────────────────────────
# Slide 4: YCSB A/C main comparison
# ───────────────────────────────────────────
slide = prs.slides.add_slide(prs.slide_layouts[6])
title(slide, "Main Results: YCSB A (50% write) & YCSB C (100% read)")

rows, cols = 7, 4
left, top = Inches(0.7), Inches(1.2)
width, height = Inches(12), Inches(3.6)
ts = slide.shapes.add_table(rows, cols, left, top, width, height)
tbl = ts.table

headers = ["Option", "Workload", "Throughput (ops/s)", "Avg Latency"]
for i, h in enumerate(headers):
    cell(tbl.cell(0, i), h, fs=BODY_FS, bold=True, color=WHITE, fill=DG)

data = [
    ("A", "A (50% write)", "20,493",       "w=774 us / r=0.4 us", A_L, A_C),
    ("A", "C (100% read)", "77.1M",        "r=0.08 us",           A_L, A_C),
    ("B", "A (50% write)", "1.00M",        "w=14.2 us / r=1.2 us", B_L, B_C),
    ("B", "C (100% read)", "13.77M",       "r=0.35 us",           B_L, B_C),
    ("C", "A (50% write)", "1.06M",        "w=10.3 us / r=4.2 us  *", C_L, C_C),
    ("C", "C (100% read)", "1.95M  *",     "r=3.78 us  *",        C_L, C_C),
]
for ri, (opt, wl, thpt, lat, fc, ec) in enumerate(data, start=1):
    cell(tbl.cell(ri, 0), opt, fs=BODY_FS, bold=True, color=ec, fill=fc)
    cell(tbl.cell(ri, 1), wl, fs=CELL_FS)
    cell(tbl.cell(ri, 2), thpt, fs=CELL_FS, bold=True)
    cell(tbl.cell(ri, 3), lat, fs=CELL_FS)

box(slide, 0.5, 5.2, 12.3, 1.8, HI_BG, HI, Pt(1.5))
text(slide, 0.7, 5.3, 11.9, 0.4, "* = Lazy RC strict-read overhead",
     BODY_FS, HI, bold=True)
text(slide, 0.7, 5.75, 11.9, 1.2,
     "Key ratios:\n"
     "  - Write throughput C/A = 51.5x  (C is 51x better than A on writes)\n"
     "  - Write latency A/C = 75x      (A is 75x slower than C on writes)\n"
     "  - C's strict-read cost: ~3.7 us per read (flushes bucket+KV from CXL)",
     BODY_FS, DG)

# ───────────────────────────────────────────
# Slide 5: Write-ratio scan
# ───────────────────────────────────────────
slide = prs.slides.add_slide(prs.slide_layouts[6])
title(slide, "Write Ratio Sensitivity")

# Insert the PNG
img_path = "/home/yanwang/cxl_shm_profiling/bench/wr_scan_cxl_g4.png"
if os.path.exists(img_path):
    slide.shapes.add_picture(img_path, Inches(0.3), Inches(1.1), Inches(9), Inches(5.6))

text(slide, 9.5, 1.3, 3.7, 5.5,
     "Takeaways:\n\n"
     "1. C is most stable\n"
     "   across all write ratios\n\n"
     "2. A is great at wr=0\n"
     "   (40M ops/s) but\n"
     "   collapses at wr=0.1\n\n"
     "3. B > C at wr <= 0.25\n"
     "   due to read cost\n\n"
     "4. C > B at wr >= 0.5\n"
     "   (write-heavy)",
     BODY_FS, DG)

# ───────────────────────────────────────────
# Slide 6: Thread scaling
# ───────────────────────────────────────────
slide = prs.slides.add_slide(prs.slide_layouts[6])
title(slide, "Thread Scaling")

img_path = "/home/yanwang/cxl_shm_profiling/bench/thread_scan_cxl_g4.png"
if os.path.exists(img_path):
    slide.shapes.add_picture(img_path, Inches(0.3), Inches(1.1), Inches(9), Inches(3.5))

text(slide, 9.5, 1.3, 3.7, 3.0,
     "Setup:\n"
     "- 2 nodes (procs)\n"
     "- YCSB A (wr=0.5)\n"
     "- Threads per node:\n"
     "  1, 2, 4, 8\n",
     BODY_FS, DG)

box(slide, 0.5, 5.0, 12.3, 2.0, HI_BG, HI, Pt(1.5))
text(slide, 0.7, 5.1, 11.9, 0.4, "Findings", BODY_FS, HI, bold=True)
text(slide, 0.7, 5.55, 11.9, 1.4,
     "1. All three protocols scale near-linearly with thread count (7x from 1->8 threads)\n"
     "2. Write latency essentially constant w.r.t. thread count (lock contention low per-bucket)\n"
     "3. B maintains ~20% lead over C at high thread count, due to C's strict-read cost\n"
     "   (YCSB A has 50% reads; C pays 2 us extra per read)\n"
     "4. A's throughput stays 100-250x lower than B/C at every thread count",
     BODY_FS, DG)

# ───────────────────────────────────────────
# Slide 7: Decision matrix
# ───────────────────────────────────────────
slide = prs.slides.add_slide(prs.slide_layouts[6])
title(slide, "Decision Matrix: Which Option to Use?")

rows, cols = 6, 2
left, top = Inches(1), Inches(1.3)
width, height = Inches(11), Inches(4.2)
ts = slide.shapes.add_table(rows, cols, left, top, width, height)
tbl = ts.table

headers = ["Workload / Requirement", "Recommended"]
for i, h in enumerate(headers):
    cell(tbl.cell(0, i), h, fs=BODY_FS, bold=True, color=WHITE, fill=DG)

decisions = [
    ("Read-heavy, low latency reads matter", "Option B", B_L, B_C),
    ("Write-heavy or balanced mixed", "Option C (Lazy RC)", C_L, C_C),
    ("Eventual-consistency reads acceptable", "Option C (fast path only)", C_L, C_C),
    ("Need CXL hardware fault tolerance", "Option A (heavy cost)", A_L, A_C),
    ("Default / production", "Option C (Lazy RC)", C_L, C_C),
]
for ri, (q, a, fc, ec) in enumerate(decisions, start=1):
    cell(tbl.cell(ri, 0), q, fs=CELL_FS)
    cell(tbl.cell(ri, 1), a, fs=BODY_FS, bold=True, color=ec, fill=fc)

box(slide, 0.5, 5.8, 12.3, 1.3, HI_BG, HI, Pt(1.5))
text(slide, 0.7, 5.9, 11.9, 0.4, "Overall recommendation", BODY_FS, HI, bold=True)
text(slide, 0.7, 6.35, 11.9, 0.7,
     "Option C (Lazy Release Consistency) is the default production recommendation.\n"
     "Provides the best write performance with clean RC semantics; read cost ~3.8 us is acceptable.",
     BODY_FS, DG)

# ───────────────────────────────────────────
# Slide 8: Open items
# ───────────────────────────────────────────
slide = prs.slides.add_slide(prs.slide_layouts[6])
title(slide, "Open Items and Next Steps")

text(slide, 0.5, 1.0, 12.3, 0.5, "Not Completed This Session", BODY_FS+2, A_C, bold=True)
text(slide, 0.7, 1.6, 12.0, 2.0,
     "1. Cross-node test on real CXL:\n"
     "   - g4's /dev/dax0.0 in broken state after mid-session reconfigure\n"
     "   - Requires reboot + ~/cxl_net/g4-setup.sh to recover\n"
     "   - g3 SSH blocked by pam_time during test window (will lift)\n"
     "   - Hardware supports sharing (both map PA 0x4080000000); once g4 recovers, trivial to run\n\n"
     "2. Full FUSEE source-tree integration (replace RDMA paths with CXL):\n"
     "   - Estimated 2-3 weeks of engineering work\n"
     "   - Detailed steps in cxl_implementation_guide.md",
     BODY_FS, DG)

text(slide, 0.5, 4.2, 12.3, 0.5, "Immediate Next Steps", BODY_FS+2, C_C, bold=True)
text(slide, 0.7, 4.8, 12.0, 2.5,
     "1. Reboot g4, run setup:\n"
     "     ssh g4 reboot\n"
     "     # after reboot:\n"
     "     cd ~/cxl_net && ./g4-setup.sh\n\n"
     "2. Cross-node test command pair:\n"
     "     # g3: ./bench/ycsb_abc_bench --opt=C --workload=A --nodes=2 --node-id=0 --path=/dev/dax0.0\n"
     "     # g4: ./bench/ycsb_abc_bench --opt=C --workload=A --nodes=2 --node-id=1 --path=/dev/dax0.0\n\n"
     "3. Start FUSEE integration Phase 1 (cxl_mm, cxl_bucket_lock)",
     BODY_FS, DG)

out = "docs/cxl_abc_results_summary.pptx"
prs.save(out)
print(f"Saved {out}")
print(f"Slides: {len(prs.slides)}")
