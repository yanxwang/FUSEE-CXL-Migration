#!/usr/bin/env python3
"""
Detailed sub-steps of shm_mutex_lock (LFM fast path) for CXL consensus Step 2.
One slide per sub-step.
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
P_COMP  = RGBColor(0x15, 0x65, 0xC0); P_COMP_L = RGBColor(0xBB, 0xDE, 0xFB)
P_CXL   = RGBColor(0xE6, 0x51, 0x00); P_CXL_L = RGBColor(0xFF, 0xE0, 0xB2)
P_VAR   = RGBColor(0xFF, 0xF9, 0xC4)
P_HI    = RGBColor(0xFF, 0x8F, 0x00)
P_HI_BG = RGBColor(0xC8, 0xE6, 0xC9)
P_GRAY  = RGBColor(0x9E, 0x9E, 0x9E)
P_LGRAY = RGBColor(0xEE, 0xEE, 0xEE)
P_DG    = RGBColor(0x33, 0x33, 0x33)
P_OK    = RGBColor(0x1B, 0x5E, 0x20)
P_FAIL  = RGBColor(0xB7, 0x1C, 0x1C)
P_LOCK  = RGBColor(0xFF, 0xCC, 0xBC)


def _in(v):
    return Inches(v)

def _box(slide, l, t, w, h, fc, ec=None, bw=Pt(1.5), text="", fs=9, c=P_DG, bold=True):
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

def _text(slide, l, t, w, h, text, fs=9, c=P_DG, bold=False, align=PP_ALIGN.CENTER):
    tb = slide.shapes.add_textbox(_in(l), _in(t), _in(w), _in(h))
    tf = tb.text_frame; tf.word_wrap = True
    p = tf.paragraphs[0]; p.alignment = align
    run = p.add_run(); run.text = text
    run.font.size = Pt(fs); run.font.color.rgb = c; run.font.bold = bold

def _arrow(slide, x0, y0, x1, y1, c=P_COMP, w=Pt(2.5), dashed=False):
    conn = slide.shapes.add_connector(1, _in(x0), _in(y0), _in(x1), _in(y1))
    conn.line.color.rgb = c; conn.line.width = w
    if dashed:
        conn.line.dash_style = 2
    fmt = conn.line._ln
    tail = fmt.makeelement(qn('a:tailEnd'), {})
    tail.set('type', 'triangle'); tail.set('w', 'med'); tail.set('len', 'med')
    fmt.append(tail)

def _badge(slide, n, color=P_COMP):
    """Step number circle in upper-left."""
    circle = slide.shapes.add_shape(MSO_SHAPE.OVAL, _in(0.3), _in(0.4), _in(0.7), _in(0.7))
    circle.fill.solid(); circle.fill.fore_color.rgb = color
    circle.line.color.rgb = P_WHITE; circle.line.width = Pt(2)
    tf = circle.text_frame
    p = tf.paragraphs[0]; p.alignment = PP_ALIGN.CENTER
    run = p.add_run(); run.text = f"2.{n}"
    run.font.size = Pt(12); run.font.color.rgb = P_WHITE; run.font.bold = True


def _draw_node(slide, action_text):
    """Node 0 box on the left."""
    _box(slide, 0.8, 1.5, 4.5, 1.4, P_COMP_L, P_COMP, Pt(2.5))
    _text(slide, 0.8, 1.6, 4.5, 0.4, "Node 0 (id=0)", 12, P_COMP, bold=True)
    _text(slide, 0.8, 2.05, 4.5, 0.85, action_text, 10, P_DG, bold=False)


def _draw_mutex(slide, b0, b1, b2, x_, y_, hi=None, b0_note=None, x_note=None, y_note=None):
    """LFM mutex state in CXL memory.
       hi = set of variable names to highlight: {'b[0]', 'b[1]', 'b[2]', 'x', 'y'}"""
    hi = hi or set()
    # Outer box
    _box(slide, 6.8, 1.0, 6.0, 4.5, P_CXL_L, P_CXL, Pt(2.5))
    _text(slide, 6.8, 1.1, 6.0, 0.4, "CXL: shm_mutex_t (LFM)", 12, P_CXL, bold=True)

    # Variables
    rows = [
        ("b[0]", b0, "Node 0 wants in",  "b[0]" in hi, b0_note),
        ("b[1]", b1, "Node 1 wants in",  "b[1]" in hi, None),
        ("b[2]", b2, "Node 2 wants in",  "b[2]" in hi, None),
        ("x",    x_, "last to write x",  "x"    in hi, x_note),
        ("y",    y_, "current owner",    "y"    in hi, y_note),
    ]
    for i, (name, val, desc, is_hi, note) in enumerate(rows):
        ry = 1.6 + i * 0.65
        fc = P_HI_BG if is_hi else P_VAR
        ec = P_HI if is_hi else P_GRAY
        bw = Pt(2.5) if is_hi else Pt(0.8)
        _box(slide, 7.0, ry, 5.6, 0.55, fc, ec, bw)
        # Variable name
        _text(slide, 7.1, ry+0.02, 1.0, 0.5, name, 11, P_DG, bold=True, align=PP_ALIGN.LEFT)
        # Value
        _text(slide, 8.2, ry+0.02, 1.5, 0.5, f"= {val}", 11,
              P_OK if is_hi else P_DG, bold=is_hi, align=PP_ALIGN.LEFT)
        # Description
        _text(slide, 9.8, ry+0.02, 2.7, 0.5, f"// {desc}", 9, P_GRAY, align=PP_ALIGN.LEFT)
        # Optional inline note
        if note:
            _text(slide, 9.8, ry+0.28, 2.7, 0.3, note, 8, P_OK, bold=True, align=PP_ALIGN.LEFT)


def _add_slide(title, color=P_COMP):
    slide = prs.slides.add_slide(prs.slide_layouts[6])
    _text(slide, 1.2, 0.4, 12.0, 0.6, title, 18, color, bold=True)
    return slide


# ════════════════════════════════════════════════
prs = Presentation()
prs.slide_width = Inches(13.33)
prs.slide_height = Inches(7.5)

# Common context box at bottom
def _ctx(slide, text, color=P_DG):
    _box(slide, 0.8, 5.9, 12.0, 1.3, RGBColor(0xFF, 0xF8, 0xE1), P_HI, Pt(1.2))
    _text(slide, 0.9, 6.0, 11.8, 1.1, text, 10, color, bold=False, align=PP_ALIGN.LEFT)


# ─── Sub-step 0: Initial state ───
slide = _add_slide("Step 2.0 — Initial state: lock is FREE")
_badge(slide, "0")
_draw_node(slide, "Node 0 wants to acquire\nSlotLock[12345].lock\n(LFM mutex on CXL)")
_draw_mutex(slide, "0", "0", "0", "0", "0",
            b0_note="(no one wants in)",
            x_note="(0 = uninitialized)",
            y_note="(0 = no owner)")
_ctx(slide,
     "LFM convention: id=0 is encoded as 1, id=1 as 2, etc. So \"value 0\" means \"no node\".\n"
     "All variables on separate cache lines. Initially: nobody is competing, nobody owns the lock.")


# ─── Sub-step 1: b[0] = 1 ───
slide = _add_slide("Step 2.1 — b[0] := 1  (announce intent)")
_badge(slide, "1")
_draw_node(slide,
           "CACHELINE_STORE(&m->b[0], 1)\n\n"
           "  // I (Node 0) want to enter")
_draw_mutex(slide, "1", "0", "0", "0", "0",
            hi={"b[0]"},
            b0_note="<- just set",
            x_note="(0 = uninitialized)",
            y_note="(0 = no owner)")
_arrow(slide, 5.3, 2.2, 7.0, 1.85, P_COMP, Pt(2.5))
_ctx(slide,
     "CACHELINE_STORE = write + clflushopt + sfence (~1 us on CXL).\n"
     "Setting b[0]=1 announces \"Node 0 is competing for this lock\". This is visible to other nodes.\n"
     "Why? In the contention path, others read b[] to know who is currently competing.")


# ─── Sub-step 2: x = 1 ───
slide = _add_slide("Step 2.2 — x := 1  (write my id+1)")
_badge(slide, "2")
_draw_node(slide,
           "CACHELINE_STORE(&m->x, 0+1)\n\n"
           "  // Register myself in x\n"
           "  // (id+1 because 0 means none)")
_draw_mutex(slide, "1", "0", "0", "1", "0",
            hi={"x"},
            b0_note="(I am competing)",
            x_note="<- just set to 1 (= my id+1)",
            y_note="(still no owner)")
_arrow(slide, 5.3, 2.2, 7.0, 3.85, P_COMP, Pt(2.5))
_ctx(slide,
     "x serves as a \"who is the current writer of x\" marker. Multiple nodes may write x;\n"
     "the last writer wins. Used in step 2.4 to detect races.")


# ─── Sub-step 3: load y, check == 0 ───
slide = _add_slide("Step 2.3 — load y, check y == 0  (is anyone holding lock?)")
_badge(slide, "3")
_draw_node(slide,
           "if (CACHELINE_LOAD(&m->y) != 0)\n"
           "  // give up + retry\n\n"
           "  loaded y -> 0\n"
           "  -> proceed (no current owner)")
_draw_mutex(slide, "1", "0", "0", "1", "0",
            hi={"y"},
            x_note="(my id+1)",
            y_note="<- read: 0 (FREE!)")
_arrow(slide, 7.0, 4.6, 5.3, 2.4, P_COMP, Pt(2.5), dashed=True)
_ctx(slide,
     "CACHELINE_LOAD = clflushopt + mfence + read (~1 us). Forces re-fetch from CXL.\n"
     "y is the \"current lock owner\" field. y==0 means no one currently holds it.\n"
     "If y were non-zero, we would back off (clear b[0]) and spin-wait for y to become 0.")


# ─── Sub-step 4: y = 1 ───
slide = _add_slide("Step 2.4 — y := 1  (claim ownership)")
_badge(slide, "4")
_draw_node(slide,
           "CACHELINE_STORE(&m->y, 0+1)\n\n"
           "  // Set y to my id+1\n"
           "  // \"I claim ownership\"")
_draw_mutex(slide, "1", "0", "0", "1", "1",
            hi={"y"},
            x_note="(my id+1)",
            y_note="<- just set to 1 (= my id+1)")
_arrow(slide, 5.3, 2.2, 7.0, 4.2, P_COMP, Pt(2.5))
_ctx(slide,
     "Now y = 1 means \"Node 0 claims ownership\". But this isn't yet final --\n"
     "another node might have written y simultaneously. We need to verify next.")


# ─── Sub-step 5: load x, check == 1 ───
slide = _add_slide("Step 2.5 — load x, check x == 1  (verify nobody raced me)")
_badge(slide, "5")
_draw_node(slide,
           "if (CACHELINE_LOAD(&m->x) != 0+1)\n"
           "  // contention - take slow path\n\n"
           "  loaded x -> 1\n"
           "  -> matches my id+1\n"
           "  -> FAST PATH SUCCESS!")
_draw_mutex(slide, "1", "0", "0", "1", "1",
            hi={"x"},
            x_note="<- read: 1 (still my value!)",
            y_note="(I claimed ownership)")
_arrow(slide, 7.0, 3.85, 5.3, 2.4, P_COMP, Pt(2.5), dashed=True)
_ctx(slide,
     "Critical check: if x == my_id+1, no one else wrote x after me -> nobody raced.\n"
     "If x != my_id+1, another node wrote x in between (between my step 2.2 and now).\n"
     "Then we'd take the slow path: clear b[0], wait for all b[]==0, recheck y. Skipped here.")


# ─── Sub-step 6: LOCKED ───
slide = _add_slide("Step 2.6 — LOCK ACQUIRED  (uncontended fast path complete)", P_OK)
_badge(slide, "6", P_OK)
_draw_node(slide,
           "return  // hold the lock\n\n"
           "Total CXL ops:\n"
           "  3 stores + 2 loads = 5 ops\n"
           "  ~5 us on real CXL hardware")
_draw_mutex(slide, "1", "0", "0", "1", "1",
            x_note="(my id+1)",
            y_note="(I am the owner)")
# big "LOCKED" badge
_box(slide, 5.4, 4.0, 1.4, 0.8, P_HI_BG, P_OK, Pt(2.5),
     "LOCKED!", 14, P_OK)
_ctx(slide,
     "Final state: b[0]=1, x=1, y=1. Node 0 now holds the lock for SlotLock[12345].\n"
     "Total cost: 5 CACHELINE operations (~5 us). No spin-waiting, no retry.\n"
     "Now Node 0 proceeds to consensus Step 3 (read slot value) ... Step 5 (commit) ... Step 6 (unlock).")


# ─── Bonus slide: contention case ───
slide = _add_slide("Bonus: What if Node 1 raced us at the same time?", P_FAIL)
_badge(slide, "?", P_FAIL)
_draw_node(slide,
           "Race scenario:\n"
           "  Node 0 stores x:=1\n"
           "  Node 1 stores x:=2  (overwrites!)\n"
           "  Node 0's check x==1 fails")
_draw_mutex(slide, "1", "1", "0", "2", "?",
            hi={"x", "b[1]"},
            x_note="<- Node 1 overwrote (now =2)",
            y_note="(undecided)")
_ctx(slide,
     "If x check fails, the contention path runs:\n"
     "  1. clear b[0]\n"
     "  2. wait for all b[j]==0 (let other contenders finish)\n"
     "  3. re-check y: if y == my_id+1, I won; else wait y==0 and retry from start.\n"
     "Result: only ONE node ever holds the lock. No two nodes can both \"think\" they got it.\n"
     "(This is exactly what fixes the race in our old Propose-Vote-Commit Claim phase!)")


# ── Save ──
out = "docs/lfm_substeps.pptx"
prs.save(out)
print(f"Saved {out}")
print(f"Slides: {len(prs.slides)}")