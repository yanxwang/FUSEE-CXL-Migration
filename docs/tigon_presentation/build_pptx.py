"""Build a ~1-hour Tigon (OSDI'25) presentation using the UTA template.

Run:
    python3 build_pptx.py

Output:
    tigon_osdi25.pptx  (in the same directory as this script)

The script:
  1. Opens UTA_PPT.pptx (to inherit its theme/master/layouts).
  2. Removes all template-provided example slides.
  3. Programmatically builds ~45 slides organized as:
        Title -> Outline -> Background -> Research Problem ->
        Existing Solutions (why not) -> Design -> Evaluation -> Conclusion
  4. Inserts figures cropped from the paper PDF (in ./figures).
     Where a figure is not available, a caption placeholder box is left
     so the presenter can paste a screenshot.

Layouts used from UTA template:
    0 = UTA Title Slide   (title page)
    4 = Section Slide     (section dividers)
    5 = Content Slide     (normal content / bullets / with picture)
    6 = Two Content Slide (side-by-side content + picture)
"""

from __future__ import annotations

import copy
import os
from pathlib import Path
from xml.etree import ElementTree as ET

from pptx import Presentation
from pptx.util import Inches, Pt, Emu
from pptx.dml.color import RGBColor
from pptx.enum.shapes import MSO_SHAPE
from pptx.enum.text import PP_ALIGN

# -------------------------------------------------------------------------
# Paths
# -------------------------------------------------------------------------
HERE      = Path(__file__).parent
TEMPLATE  = Path("/home/yanwang/paper/UTA_PPT.pptx")
FIGDIR    = HERE / "figures"
OUTFILE   = HERE / "tigon_osdi25.pptx"

# -------------------------------------------------------------------------
# Layout indices (UTA template)
# -------------------------------------------------------------------------
LAY_TITLE    = 0
LAY_SECTION  = 4
LAY_CONTENT  = 5
LAY_TWOCOL   = 6

# -------------------------------------------------------------------------
# Helpers
# -------------------------------------------------------------------------
def clear_template_slides(prs: Presentation) -> None:
    """Remove all slides that came with the template."""
    xml_slides = prs.slides._sldIdLst  # noqa: SLF001
    slides = list(xml_slides)
    for sld in slides:
        xml_slides.remove(sld)
    # Also drop from the package parts so the file stays clean
    for rid in list(prs.part.rels):
        rel = prs.part.rels[rid]
        if rel.reltype.endswith("/slide"):
            prs.part.drop_rel(rid)


def set_placeholder_text(slide, idx, text, font_size=None, bold=None):
    """Set text of placeholder by idx. Returns the placeholder or None."""
    for ph in slide.placeholders:
        if ph.placeholder_format.idx == idx:
            tf = ph.text_frame
            tf.text = text
            if font_size or bold is not None:
                for p in tf.paragraphs:
                    for r in p.runs:
                        if font_size:
                            r.font.size = Pt(font_size)
                        if bold is not None:
                            r.font.bold = bold
            return ph
    return None


def fill_body_bullets(slide, bullets, body_idx=1, font_size=18):
    """Put bullets (list[str] or list[(text,level)]) into body placeholder."""
    ph = None
    for p in slide.placeholders:
        if p.placeholder_format.idx == body_idx:
            ph = p
            break
    if ph is None:
        return None
    tf = ph.text_frame
    tf.word_wrap = True
    tf.clear()
    for i, b in enumerate(bullets):
        if isinstance(b, tuple):
            text, level = b
        else:
            text, level = b, 0
        p = tf.paragraphs[0] if i == 0 else tf.add_paragraph()
        p.text = text
        p.level = level
        for r in p.runs:
            r.font.size = Pt(font_size - 2 * level)
    return ph


def remove_placeholder(slide, idx):
    """Remove a placeholder from a slide by idx (used to free space for an image)."""
    for ph in slide.placeholders:
        if ph.placeholder_format.idx == idx:
            sp = ph._element
            sp.getparent().remove(sp)
            return


def add_picture_right(slide, img_path, left_in=5.0, top_in=1.4, width_in=4.6):
    if not Path(img_path).exists():
        return add_placeholder_box(slide, left_in, top_in, width_in, 3.0,
                                   f"[Figure placeholder: {Path(img_path).name}]")
    return slide.shapes.add_picture(
        str(img_path), Inches(left_in), Inches(top_in), width=Inches(width_in)
    )


def add_picture_centered(slide, img_path, top_in=1.3, max_w_in=8.5, max_h_in=4.0):
    if not Path(img_path).exists():
        return add_placeholder_box(slide, 1.0, top_in, max_w_in, max_h_in,
                                   f"[Figure placeholder: {Path(img_path).name}]")
    # We don't know the image aspect without opening it; just set width and
    # let PowerPoint preserve the aspect ratio.
    from PIL import Image
    try:
        w, h = Image.open(img_path).size
        aspect = w / h
    except Exception:
        aspect = max_w_in / max_h_in
    if max_w_in / max_h_in > aspect:
        disp_h = max_h_in
        disp_w = disp_h * aspect
    else:
        disp_w = max_w_in
        disp_h = disp_w / aspect
    left_in = (10.0 - disp_w) / 2
    return slide.shapes.add_picture(
        str(img_path), Inches(left_in), Inches(top_in),
        width=Inches(disp_w), height=Inches(disp_h)
    )


def add_placeholder_box(slide, left, top, width, height, caption):
    """Rectangle with a label so the user knows where to paste a figure."""
    shape = slide.shapes.add_shape(
        MSO_SHAPE.RECTANGLE,
        Inches(left), Inches(top), Inches(width), Inches(height),
    )
    shape.fill.solid()
    shape.fill.fore_color.rgb = RGBColor(0xEE, 0xEE, 0xEE)
    shape.line.color.rgb = RGBColor(0xAA, 0xAA, 0xAA)
    tf = shape.text_frame
    tf.word_wrap = True
    tf.text = caption
    for p in tf.paragraphs:
        p.alignment = PP_ALIGN.CENTER
        for r in p.runs:
            r.font.size = Pt(14)
            r.font.italic = True
            r.font.color.rgb = RGBColor(0x77, 0x77, 0x77)
    return shape


def add_speaker_notes(slide, notes):
    nt = slide.notes_slide
    nt.notes_text_frame.text = notes


def add_textbox(slide, left, top, width, height, text, size=16, bold=False,
                color=None, align=None):
    tb = slide.shapes.add_textbox(
        Inches(left), Inches(top), Inches(width), Inches(height)
    )
    tf = tb.text_frame
    tf.word_wrap = True
    tf.text = text
    for p in tf.paragraphs:
        if align is not None:
            p.alignment = align
        for r in p.runs:
            r.font.size = Pt(size)
            r.font.bold = bold
            if color:
                r.font.color.rgb = color
    return tb


# -------------------------------------------------------------------------
# Build
# -------------------------------------------------------------------------
def build():
    prs = Presentation(str(TEMPLATE))
    clear_template_slides(prs)

    L_TITLE   = prs.slide_layouts[LAY_TITLE]
    L_SECTION = prs.slide_layouts[LAY_SECTION]
    L_CONTENT = prs.slide_layouts[LAY_CONTENT]
    L_TWOCOL  = prs.slide_layouts[LAY_TWOCOL]

    fig = lambda name: FIGDIR / name  # noqa: E731

    # =====================================================================
    # 1. Title slide
    # =====================================================================
    s = prs.slides.add_slide(L_TITLE)
    set_placeholder_text(s, 0,
        "Tigon: A Distributed Database for a CXL Pod")
    set_placeholder_text(s, 10,
        "Yibo Huang, Haowei Chen, Newton Ni, Yan Sun, "
        "Vijay Chidambaram, Dixin Tang, Emmett Witchel")
    set_placeholder_text(s, 11, "OSDI 2025  |  UT Austin & UIUC")
    set_placeholder_text(s, 12, "Presented by: Yan Wang")
    add_speaker_notes(s,
        "Welcome. Today I'll present Tigon, an OSDI'25 paper from UT Austin "
        "that builds the first distributed in-memory database on a CXL pod. "
        "Plan: ~60 minutes. Please interrupt with questions at any point.")

    # =====================================================================
    # 2. Outline
    # =====================================================================
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Outline")
    set_placeholder_text(s, 10, "~60 minutes presentation")
    fill_body_bullets(s, [
        "1. Background: CXL, CXL pods, and distributed transactional databases",
        "2. Research problem: Cross-host synchronization is expensive",
        "3. Existing solutions and why they fall short",
        "4. Tigon's design",
        "    Data organization & software cache coherence",
        ("Concurrency control (2PL + next-key locking)", 1),
        ("Logging & recovery without 2PC", 1),
        "5. Evaluation: TPC-C, YCSB, scalability, sensitivity studies",
        "6. Discussion & takeaways",
    ], font_size=18)

    # =====================================================================
    # SECTION 1. BACKGROUND
    # =====================================================================
    s = prs.slides.add_slide(L_SECTION)
    set_placeholder_text(s, 0, "1. Background")
    set_placeholder_text(s, 10,
        "Distributed transactions, RDMA, and the arrival of CXL")

    # 1.1 Why distributed transactions are hard
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Distributed Transactional Databases")
    set_placeholder_text(s, 10, "Decades of research, still a bottleneck")
    fill_body_bullets(s, [
        "In-memory OLTP databases scale horizontally by partitioning data",
        "Transactions that touch a single partition are cheap and local",
        "Multi-partition transactions require cross-host coordination:",
        ("Many message exchanges during execution", 1),
        ("Two-phase commit (2PC) at the end for atomicity", 1),
        "Network latency dominates: one RTT ≈ microseconds",
        ("= hundreds of DRAM access times", 1),
        "Performance collapses as multi-partition ratio grows",
    ])
    add_speaker_notes(s,
        "Key message: distributed transactional DBs have been studied for 40 "
        "years, but the fundamental problem is that coordination across hosts "
        "is expensive. Shared-nothing systems work great for single-partition "
        "transactions and fall apart otherwise.")

    # 1.2 RDMA line of work
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Accelerating Distributed Transactions with RDMA")
    set_placeholder_text(s, 10, "One-sided RDMA and disaggregated shared memory")
    fill_body_bullets(s, [
        "RDMA bypasses the kernel: direct remote memory access",
        "Two popular directions:",
        ("Optimize partition-based DBs with RDMA (FaRM, FaSST, DrTM, ...)", 1),
        ("Shared disaggregated memory DBs (FORD, Motor, ...)", 1),
        "Still: RDMA latency is 1–2 orders of magnitude higher than local DRAM",
        "Every synchronization is a network round-trip in microseconds",
        "→ Even with RDMA, cross-host sync is the bottleneck",
    ])

    # 1.3 CXL intro
    s = prs.slides.add_slide(L_TWOCOL)
    set_placeholder_text(s, 0, "Compute Express Link (CXL)")
    set_placeholder_text(s, 10, "A new, load/store-accessible memory fabric")
    fill_body_bullets(s, [
        "Open-standard interconnect over PCIe 5.0 / 6.0",
        "CXL.mem: CPUs access CXL memory via normal LD/ST",
        "CXL 1.1: one host per device (memory expansion)",
        "CXL 3.0 / 3.2: cacheline-granularity sharing across hosts with HW cache coherence",
        "Hardware prototypes exist: SK Hynix Niagara 2.0 (8 hosts), Microsoft (2 hosts)",
    ], body_idx=1, font_size=16)
    # Right column: describe/diagram placeholder
    add_picture_right(s, fig("figure1_architecture.png"),
                      left_in=5.1, top_in=1.4, width_in=4.6)
    # Also remove the right-column text placeholder (idx 2) if the image overlaps
    remove_placeholder(s, 2)

    # 1.4 CXL pod
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "CXL Pod")
    set_placeholder_text(s, 10,
        "An intermediate between SMP and distributed systems")
    fill_body_bullets(s, [
        "A small set of hosts (8–16) directly connected to a shared CXL memory",
        "Via a multi-headed device (MHD) — no switch required → low latency",
        "Each host has its own local DRAM (hardware cache coherent)",
        "All hosts see a shared CXL memory region with limited HW cache coherence",
        "Not quite SMP (inter-host coherence is expensive),",
        "Not quite distributed (memory is load/store-addressable)",
        "Tigon navigates this new tradeoff space",
    ])

    # 1.5 CXL performance numbers
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "CXL Performance: The Reality")
    set_placeholder_text(s, 10, "Good — but not as good as local DRAM")
    fill_body_bullets(s, [
        "Latency:  214–394 ns (CXL)  vs.  111–117 ns (local DRAM)",
        "Bandwidth:  18–52 GB/s (CXL)  vs.  218–246 GB/s (local DRAM, read-only)",
        "(Still 1–2 orders of magnitude better than RDMA for latency)",
        "Takeaway #1:  Cannot naively put all data on CXL memory",
        "Takeaway #2:  Must lean heavily on local DRAM for the hot path",
        "Takeaway #3:  Bandwidth is precious — minimize data movement",
    ])
    add_speaker_notes(s,
        "These numbers are from Sun et al.'s recent measurement study. The "
        "takeaways frame Tigon's whole design — we cannot put everything on CXL, "
        "we must keep frequent accesses local, and we must be stingy with bandwidth.")

    # 1.6 Limited HWcc
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Why Hardware Cache Coherence is Limited")
    set_placeholder_text(s, 10, "A practical, not theoretical, constraint")
    fill_body_bullets(s, [
        "CXL 3.0+ supports inter-host HW cache coherence via back-invalidations",
        "But: coherence requires a snoop filter that tracks every cacheable line",
        "Snoop filter is expensive silicon — limited by area budget on the CXL controller",
        "AMD report (ISCA'24): only tens to hundreds of MBs can be kept HW-coherent",
        "Extreme example: Intel Granite Rapids 6980P would need 7.9 GB of tags for 16×504 MB",
        "Practical devices: ≤ a few hundred MB of HWcc memory",
        "→ Database synchronization structures must fit into a small HWcc budget",
    ])

    # =====================================================================
    # SECTION 2. RESEARCH PROBLEM
    # =====================================================================
    s = prs.slides.add_slide(L_SECTION)
    set_placeholder_text(s, 0, "2. Research Problem")
    set_placeholder_text(s, 10,
        "Build an efficient distributed transactional DB on a CXL pod")

    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "The Core Question")
    set_placeholder_text(s, 10, "Synchronize through memory, not through the network")
    fill_body_bullets(s, [
        "Existing distributed DBs: synchronize concurrent cross-host accesses via messages",
        "Proposal: synchronize directly through shared memory using atomic ops on CXL",
        "Why now?  CXL 3.x makes load/store-addressable, cache-coherent shared memory real",
        "Why not trivial?  CXL has three hardware limitations we must confront:",
        ("Higher latency than local DRAM", 1),
        ("Lower bandwidth than local DRAM", 1),
        ("Very limited HW-cache-coherent region", 1),
        "Can we convert message exchanges into data-structure operations?",
    ])

    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Design Goals")
    set_placeholder_text(s, 10, "What a good CXL-pod database must achieve")
    fill_body_bullets(s, [
        "G1. Match or beat single-partition performance of shared-nothing DBs when no sharing",
        "G2. Scale gracefully as the fraction of multi-partition transactions grows",
        "G3. Obey HWcc memory budget (tens–hundreds of MB) without killing performance",
        "G4. Use CXL bandwidth sparingly (bandwidth is the real bottleneck)",
        "G5. Preserve standard transaction semantics (serializability, durability, recovery)",
        "G6. Avoid 2PC and its hidden latency tax",
    ])

    # =====================================================================
    # SECTION 3. EXISTING SOLUTIONS (AND WHY NOT)
    # =====================================================================
    s = prs.slides.add_slide(L_SECTION)
    set_placeholder_text(s, 0, "3. Existing Solutions — and Why They Fall Short")
    set_placeholder_text(s, 10, "Three strawmen we must do better than")

    # 3.1 Shared-nothing + 2PC
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Approach 1: Shared-Nothing + 2PC")
    set_placeholder_text(s, 10, "Sundial, DS2PL, H-Store, Spanner, …")
    fill_body_bullets(s, [
        "Partition data across hosts. Each host handles its partition.",
        "Multi-partition transactions: coordinator gathers remote ops, runs 2PC",
        "Why it falls short:",
        ("Heavy messaging: read/write RPCs + prepare + commit (2 extra RTTs)", 1),
        ("Paper measures 3.3 – 4.1 messages/transaction for TPC-C 60/90", 1),
        ("Throughput drops sharply as multi-partition % grows", 1),
        "Even with RDMA or CXL used only as a transport, performance remains limited",
    ])

    # 3.2 RDMA disaggregated
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Approach 2: RDMA-based Shared Disaggregated Memory")
    set_placeholder_text(s, 10, "FORD, Motor, XSTORE, …")
    fill_body_bullets(s, [
        "Compute and memory are separated; compute nodes read/write remote tuples via RDMA",
        "Avoids partitioning → no multi-partition problem, no 2PC",
        "Why it falls short:",
        ("One-sided RDMA latency is in the microseconds — 10–100× local DRAM", 1),
        ("Every tuple access can be a network round-trip", 1),
        ("Motor’s published peak: ~100K TPC-C txn/s; our testbed: ~30K/s (bandwidth-bound)", 1),
        "CXL memory is load/store-addressable and much faster — RDMA leaves perf on the table",
    ])

    # 3.3 Use CXL as a faster network transport
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Approach 3: CXL as a Faster Network")
    set_placeholder_text(s, 10, "HydraRPC-style:  reuse CXL only for message passing")
    fill_body_bullets(s, [
        "Keep shared-nothing architecture, just replace the network with CXL queues",
        "Paper’s improved baselines Sundial-CXL / DS2PL-CXL do exactly this",
        "Result:  2× improvement for TPC-C 60/90 (message RTT shrinks)",
        "Why it still falls short:",
        ("You still exchange messages — just faster ones", 1),
        ("You still run 2PC", 1),
        ("You ignore the unique capability of CXL: shared memory with atomic ops", 1),
        "Tigon asks: what if we eliminate messages on the critical path instead?",
    ])

    # 3.4 Naive: all data on CXL
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Approach 4: Put Everything on CXL Memory")
    set_placeholder_text(s, 10, "The obvious strawman")
    fill_body_bullets(s, [
        "Every tuple lives in shared CXL memory; every host reads/writes directly",
        "Why it falls short:",
        ("CXL is 1.6–3× slower than local DRAM (latency) → hot path suffers", 1),
        ("CXL bandwidth is ~13% of local DRAM → cores starve", 1),
        ("HW cache coherence only covers ~hundreds of MB → won’t fit the database", 1),
        "→ Data placement must be careful: hot data local, shared data on CXL",
    ])

    # 3.5 Summary table
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Summary of Prior Art")
    set_placeholder_text(s, 10, "Tigon occupies a new point in the design space")
    fill_body_bullets(s, [
        "Partition + 2PC:  messaging bound, 2PC overhead",
        "RDMA disaggregated:  μs-scale per-access latency",
        "CXL as fast network:  still messaging, still 2PC",
        "Naive all-on-CXL:  violates HWcc budget, bandwidth starvation",
        "Insight behind Tigon:",
        ("Only a tiny set of tuples is concurrently shared at any instant (the CAT)", 1),
        ("Keep the CAT in CXL memory; everything else local", 1),
        ("Convert cross-host messages into local atomic ops on shared structures", 1),
    ])

    # =====================================================================
    # SECTION 4. DESIGN
    # =====================================================================
    s = prs.slides.add_slide(L_SECTION)
    set_placeholder_text(s, 0, "4. Tigon Design")
    set_placeholder_text(s, 10, "The first distributed DB designed for a CXL pod")

    # 4.1 Key insight: CAT
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Key Insight: the Cross-host Active Tuple (CAT)")
    set_placeholder_text(s, 10, "Working set of shared data is tiny")
    fill_body_bullets(s, [
        "Databases can be huge, but at any instant only a few tuples are actively shared",
        "Why?  In-memory OLTP has ~#cores concurrent transactions, each touching a few tuples",
        "Back-of-envelope for TPC-C:  1000 cores × 39 tuples × ~190 B = ~7 MB",
        "This is exactly the size regime where HWcc memory can help",
        "Tigon’s north star:  keep the CAT in CXL memory, use atomic ops on it for sync",
    ])

    # 4.2 Architecture figure
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Tigon Architecture at a Glance")
    set_placeholder_text(s, 10, "Partitioned local DRAM + shared CXL memory")
    remove_placeholder(s, 1)
    add_picture_centered(s, fig("figure1_architecture.png"),
                         top_in=1.3, max_w_in=9.0, max_h_in=3.9)
    add_speaker_notes(s,
        "Each host owns a data partition in local DRAM. Shared CXL memory is "
        "split into a small HWcc region (for latches, index, metadata) and a "
        "larger SWcc region (for tuple bodies and other metadata). Tigon "
        "dynamically moves tuples into CXL memory when they become part of the CAT.")

    # 4.3 Example workflow
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Example Transaction Workflow")
    set_placeholder_text(s, 10, "From the paper's Figure 2")
    remove_placeholder(s, 1)
    add_picture_centered(s, fig("figure2_workflow.png"),
                         top_in=1.3, max_w_in=9.0, max_h_in=3.9)
    add_speaker_notes(s,
        "Transaction 1 on Host 1: reads A (local, take read lock), then wants "
        "to write C (owned by Host 2). It sends a move request; Host 2 places "
        "C in CXL memory. T1 then grabs the write lock on the CXL-resident C, "
        "updates it, and commits locally — no 2PC. Transaction 2 on Host 2 "
        "conflicts on C, observes the lock, aborts under NO_WAIT.")

    # 4.4 Data organization
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Data Organization in Tigon")
    set_placeholder_text(s, 10, "HWcc record vs. SWcc row")
    remove_placeholder(s, 1)
    add_picture_centered(s, fig("figure3_data_org.png"),
                         top_in=1.3, max_w_in=9.3, max_h_in=4.0)

    # 4.5 HWcc record fields
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "The 8-Byte HWcc Record")
    set_placeholder_text(s, 10, "Minimize HWcc footprint — everything frequently synced fits here")
    fill_body_bullets(s, [
        "1 bit   HWcc-latch (mutual exclusion for the record)",
        "8 bit   2pl-lock (read count + write bit)",
        "1 bit   has-next-key (needed for correct next-key locking across hosts)",
        "1 bit   is-dirty (tuple modified since moved to CXL)",
        "1 bit   clock-bit (for CLOCK eviction)",
        "16 bit  SWcc-bitmap (per-host cacheability for SWcc row)",
        "36 bit  SWcc-row-ptr (offset pointer to the SWcc row)",
        "Design point: only latch + lock + tiny metadata need HW coherence",
    ])

    # 4.6 Shortcut pointer
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Indexes and the Shortcut Pointer")
    set_placeholder_text(s, 10, "Fast path for owners, general path for non-owners")
    fill_body_bullets(s, [
        "Each host has a local index in DRAM over its own partition",
        "An additional CXL index lives in HWcc memory for tuples in CXL memory",
        "For an owned tuple that has been moved to CXL, the local row caches a shortcut-ptr",
        "Owner hosts skip the CXL index using the shortcut pointer — saves CXL lookup",
        "Non-owners look up in the CXL index (synchronized via HWcc latches)",
        "Key challenge:  shortcut-ptr correctness under concurrent data movement",
        ("Solution: local-latch on the local row serializes owner-side operations", 1),
        ("Non-owners can only drive moves via request messages, never directly", 1),
    ])

    # 4.7 SWcc protocol
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Software Cache Coherence (SWcc)")
    set_placeholder_text(s, 10, "Make the large non-HWcc region usable for tuple data")
    fill_body_bullets(s, [
        "Problem:  most of CXL memory is NOT hardware cache coherent",
        "If hosts cache SWcc rows, stale data is possible",
        "Insight:  the database already has latches (HWcc-latch) gating every access",
        "Co-design the SW coherence protocol with the latch:",
        ("Attach a 16-bit SWcc-bitmap to each HWcc record: one bit per host", 1),
        ("Reader: if my bit is set → cacheable load; else flush+reload, set my bit", 1),
        ("Writer: invalidate (unset) all other hosts' bits on write", 1),
        "Coherence granularity = tuple (larger than a cacheline) → metadata cheap",
        "Result: database can use lots of SWcc memory with cacheable reads",
    ])

    # 4.8 Data movement policy (CLOCK)
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Data Movement Policy: CLOCK")
    set_placeholder_text(s, 10, "Cheap eviction that respects the HWcc budget")
    fill_body_bullets(s, [
        "Non-owner access → request owner to move tuple to CXL memory",
        "HWcc memory fills up → evict least-likely-reused tuples back to local DRAM",
        "LRU is ideal but needs a linked list + metadata updates on every access",
        "Tigon uses CLOCK: a single reference bit per tuple",
        ("Circular cursor scans tuples; bit set → clear and skip; bit clear → evict", 1),
        "Evaluation: CLOCK uses ~25% less HWcc than LRU; 2.4× faster under budget pressure",
    ])

    # 4.9 Concurrency control 1
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Concurrency Control: 2PL with NO_WAIT")
    set_placeholder_text(s, 10, "Single-host CC extended to shared CXL data")
    fill_body_bullets(s, [
        "Tigon uses strong strict 2PL (SS2PL) — acquire during exec, release on commit",
        "Read/write locks live in the 2pl-lock byte inside the HWcc record",
        "Acquired via atomic ops on HWcc memory — no messages needed",
        "NO_WAIT deadlock prevention: abort on lock conflict (prior work shows it scales best)",
        "Standard OCC/MVCC left as future work (they add complexity under CXL pod model)",
    ])

    # 4.10 Next-key locking
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Avoiding Phantoms: Enhanced Next-Key Locking")
    set_placeholder_text(s, 10, "Serializable ranges across hosts, with a partial CXL index")
    fill_body_bullets(s, [
        "Classic next-key lock: lock the next key in the ordered index on insert/delete/scan",
        "Challenge: CXL index is a subset of the host's local index",
        ("The 'next key' in CXL index may not be the true next key!", 1),
        "Solution: add has-next-key bit per CXL-index entry",
        ("Set if next-in-CXL-index == next-in-local-index", 1),
        ("Maintained on insert/delete/move events", 1),
        "If the bit isn’t set, ask the remote host to move the real next key to CXL",
        "Paper: phantom-avoidance costs 10–12% throughput (reasonable)",
    ])

    # 4.11 Logging without 2PC (big idea)
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Logging & Recovery Without 2PC")
    set_placeholder_text(s, 10, "Adapt SiloR’s epoch-group-commit to 2PL + CAT")
    fill_body_bullets(s, [
        "Observation 1:  by keeping the CAT in CXL memory, a single host can complete and log all tuple modifications of its transaction",
        "Observation 2:  indexes can be reconstructed from tuples → no need to log index changes",
        "→ One host logs all effects → no 2PC!",
        "Borrowed from SiloR [Zheng'14]:",
        ("Epoch-based group commit (transactions batch into 10 ms epochs)", 1),
        ("Parallel value logging: each worker writes its own log buffer to local SSD", 1),
        "Each tuple carries (epoch, version); tuple with highest (epoch, version) wins on recovery",
        "A global epoch counter lives in HWcc memory — synchronization is ~negligible",
    ])

    # 4.12 Implementation
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Implementation")
    set_placeholder_text(s, 10, "Open source:  github.com/ut-datasys/tigon")
    fill_body_bullets(s, [
        "C++ on top of Lotus (≈18K LoC); ≈5K new LoC for Tigon",
        "API:  read, write, insert, delete, range query + parameterized transactions",
        "B+-tree with optimistic crabbing (extended with next-key locking)",
        "Offset pointers → CXL-resident structures are position-independent",
        "CXL memory exposed as a CPU-less NUMA node; modified mimalloc allocator",
        "CXL transport:  lock-free MPSC ring buffers (metadata in HWcc, payload in non-HWcc)",
        "Epoch-based reclamation (EBR) via per-worker epoch counters in HWcc",
    ])

    # =====================================================================
    # SECTION 5. EVALUATION
    # =====================================================================
    s = prs.slides.add_slide(L_SECTION)
    set_placeholder_text(s, 0, "5. Evaluation")
    set_placeholder_text(s, 10, "TPC-C, YCSB, scalability, sensitivity, ablations")

    # 5.1 Setup
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Experimental Setup")
    set_placeholder_text(s, 10, "Emulated CXL pod on a single Intel server")
    fill_body_bullets(s, [
        "Hardware:  Intel Xeon Platinum 8568Y+, 512 GB DRAM, 128 GB CXL 1.1 (PCIe 5.0 x8)",
        "Measured:  CXL latency 259 ns vs DRAM 159 ns; CXL BW 31.8 GB/s vs DRAM 238 GB/s",
        "Emulation:  8 VMs share the CXL device; inter-VM coherence ≈ inter-host coherence",
        "HWcc budget:  capped to 200 MB (varied in sensitivity study)",
        "Baselines:",
        ("Sundial+, DS2PL+:  optimized shared-nothing DBs with CXL transport", 1),
        ("Motor:  RDMA-based shared disaggregated memory DB", 1),
        "Workloads:  full TPC-C (24 warehouses);  YCSB-like (2.4M keys, Zipf 0.7/0.99)",
    ])

    # 5.2 Baseline improvements story
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Why 'Improved' Baselines?")
    set_placeholder_text(s, 10, "Make the comparison genuinely fair")
    fill_body_bullets(s, [
        "Baselines originally use TCP/RDMA as transport — unfair vs CXL-native Tigon",
        "Enhancements applied to Sundial and DS2PL:",
        ("Replace network transport with CXL message queues (Sundial-CXL, DS2PL-CXL) — +2× at 60/90", 1),
        ("Convert the freed I/O thread into an extra worker thread (Sundial+, DS2PL+)", 1),
        ("Added logging, phantom avoidance, and full TPC-C coverage (5 txns)", 1),
        "Total: baselines are up to 4.2× faster than their original forms",
        "All comparisons below are Tigon vs. these improved baselines",
    ])

    # 5.3 TPC-C headline
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "End-to-End: TPC-C Throughput")
    set_placeholder_text(s, 10, "Varying % multi-partition transactions")
    remove_placeholder(s, 1)
    add_picture_centered(s, fig("figure4_tpcc_perf.png"),
                         top_in=1.3, max_w_in=9.3, max_h_in=4.0)
    add_speaker_notes(s,
        "At 0/0 (no multi-partition): Sundial+ is 37% faster (OCC + no phantom handling) — "
        "a fair-but-expected loss for Tigon. At 60/90: Tigon is 75% faster than Sundial+ "
        "and 2.5× faster than DS2PL+. Motor: 15.9× – 18.5× slower (bandwidth-bound).")

    # 5.4 TPC-C analysis
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "TPC-C: Where Tigon Wins")
    set_placeholder_text(s, 10, "No messages on the critical path")
    fill_body_bullets(s, [
        "At 60/90 multi-partition ratio:",
        ("Sundial+ / DS2PL+ send 3.3 and 4.1 messages per txn", 1),
        ("Tigon sends 0 on the critical path — sync via atomic ops on HWcc", 1),
        "CUSTOMER and STOCK tables get hot → Tigon moves 720K+2.4M tuples into CXL during warmup",
        ("176 MB HWcc + 1.6 GB CXL total", 1),
        "No data moves back during the run → CLOCK policy never triggers",
        "Vs. Motor: bandwidth of 25 Gbps NIC is the bottleneck; peak ≈ 30 K/s",
        ("Even with a better NIC, Motor paper reports ≤100 K/s", 1),
    ])

    # 5.5 YCSB
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "End-to-End: YCSB Throughput")
    set_placeholder_text(s, 10, "4 read/write mixes × varying multi-partition %")
    remove_placeholder(s, 1)
    add_picture_centered(s, fig("figure5_ycsb.png"),
                         top_in=1.3, max_w_in=8.5, max_h_in=4.0)

    # 5.6 YCSB commentary
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "YCSB: Findings")
    set_placeholder_text(s, 10, "Larger wins on higher contention")
    fill_body_bullets(s, [
        "At 0% multi-partition:  all systems within 3.3% (no cross-host sync)",
        "At 100% multi-partition:",
        ("Read-only:  Tigon 2.0×–2.3× over Sundial+", 1),
        ("50R/50W:  Tigon 2.7× over Sundial+, 3.5× over DS2PL+ (Zipf 0.99)", 1),
        ("Motor:  5.4×–14.3× slower than Tigon", 1),
        "Tigon moves all 2.4M tuples to CXL (112 MB HWcc + 2.6 GB CXL) — fits in budget",
        "Large win on writes:  atomic-op writes beat message-based sync",
    ])

    # 5.7 Scalability
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Scalability: 1 → 8 Hosts")
    set_placeholder_text(s, 10, "TPC-C 60/90 and YCSB 95R/5W, 100% multi-partition")
    remove_placeholder(s, 1)
    add_picture_centered(s, fig("figure6_scalability.png"),
                         top_in=1.3, max_w_in=8.0, max_h_in=4.0)
    add_speaker_notes(s,
        "Tigon: 5.7× on TPC-C, 3.5× on YCSB going from 1 → 8 hosts. "
        "Slope: Tigon 55.1 Ktx/s/host vs Sundial+ 29.9, DS2PL+ 21.6. "
        "Scaling ceiling unclear: limited by atomic-op contention, HW coherence, or HWcc size.")

    # 5.8 Sensitivity: HWcc budget
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Sensitivity: HWcc Memory Budget")
    set_placeholder_text(s, 10, "Even 50 MB works for TPC-C")
    remove_placeholder(s, 1)
    add_picture_centered(s, fig("figure7_hwcc_budget.png"),
                         top_in=1.3, max_w_in=8.0, max_h_in=4.0)
    add_speaker_notes(s,
        "TPC-C runs well with as little as 50 MB HWcc — only 5.8% slower than unlimited. "
        "At 10 MB: data thrash (16K TPC-C / 110K YCSB moves per second). "
        "But bandwidth is barely used (1.1% / 9.1% of CXL BW) → latency of movement is the cost.")

    # 5.9 Inter-host coherence cost
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Under Realistic Inter-Host Coherence Cost")
    set_placeholder_text(s, 10, "Conservative estimate of back-invalidation penalty")
    fill_body_bullets(s, [
        "Testbed uses intra-socket coherence — optimistic vs real CXL back-invalidations",
        "Paper can only count software invalidations (no HW counter): 12 M / 82 M = 14.5% of accesses",
        "Assume back-invalidations are 4× slower than our intra-socket measurements",
        "Net effect:  Tigon throughput –41.4%",
        "Even then: Tigon still beats Sundial+ (+2.8%), DS2PL+ (+45%), Motor (9.6×)",
        "Tigon’s design principle (small HWcc region) protects it from HW coherence costs",
    ])

    # 5.10 SW cache coherence ablation
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Does Software Cache Coherence Matter?")
    set_placeholder_text(s, 10, "Four configurations compared")
    remove_placeholder(s, 1)
    add_picture_centered(s, fig("figure8_sw_coherence.png"),
                         top_in=1.3, max_w_in=8.0, max_h_in=4.0)
    add_speaker_notes(s,
        "NoSWcc (HWcc only): limited-size region → lots of data movement. "
        "Up to 4.3× slower on YCSB 100% multi-partition. "
        "NonTemporal (bypass cache): 4.5–5.1% slower on TPC-C, 11–20% slower on YCSB. "
        "NoSharedReader (single reader): 15% slower on YCSB — SWcc-bitmap flips cost flushes. "
        "Full Tigon wins across the board — and specifically on shared read-heavy workloads.")

    # 5.11 Logging latency
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Logging Epoch vs. Throughput/Latency")
    set_placeholder_text(s, 10, "Group commit is the right tradeoff")
    remove_placeholder(s, 1)
    add_picture_centered(s, fig("table1_logging_latency.png"),
                         top_in=1.3, max_w_in=8.5, max_h_in=3.8)
    add_speaker_notes(s,
        "10 ms epoch: only 2.8% slower than 50 ms but 48% lower p50 latency. "
        "Without logging Tigon is ~6% faster — logging is cheap. "
        "Motor uses 3-way replication, so p50 in hundreds of μs — Tigon prioritizes throughput via group commit.")

    # 5.12 Optimization ablations
    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Optimization Ablations")
    set_placeholder_text(s, 10, "Each piece pulls its weight")
    fill_body_bullets(s, [
        "CLOCK vs. LRU:",
        ("LRU needs 33% more HWcc; 2.4× slower under HWcc pressure", 1),
        ("Under unlimited HWcc, LRU still 17% slower (lock contention on LRU list)", 1),
        "Shortcut pointer (owner-side fast path):",
        ("+16% on TPC-C, +8–24% on YCSB 95R/5W", 1),
        "is-dirty (read clean tuples from local DRAM):",
        ("+60% YCSB read-only at 10% multi-partition; +27% at 100%", 1),
        "Together these are what make Tigon frugal with CXL bandwidth",
    ])

    # =====================================================================
    # SECTION 6. DISCUSSION / CONCLUSION
    # =====================================================================
    s = prs.slides.add_slide(L_SECTION)
    set_placeholder_text(s, 0, "6. Discussion & Takeaways")
    set_placeholder_text(s, 10, "What did we learn? What's next?")

    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Discussion Points")
    set_placeholder_text(s, 10, "Questions worth debating")
    fill_body_bullets(s, [
        "Is the CAT assumption robust to analytical / long-running transactions?",
        "What happens at 32+ hosts?  SWcc-bitmap is 16-bit; HWcc size is fixed by silicon",
        "Co-design or decouple:  SW coherence requires DB-side changes — reusable by other apps?",
        "Real CXL 3.x hardware doesn’t exist yet with HW coherence — evaluation extrapolates",
        "2PL + NO_WAIT — abort rates under very high contention?",
        "Failure model: single-machine correlated failure undermines the pod (shared memory device)",
        "Compare against OCC/MVCC-based CXL designs as they emerge",
    ])

    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Takeaways")
    set_placeholder_text(s, 10, "Why this paper matters")
    fill_body_bullets(s, [
        "CXL pods are a new architectural point — not SMP, not fully distributed",
        "The right abstraction for databases is NOT 'CXL as a faster network'",
        "Turn messages into atomic ops on shared structures — eliminate 2PC",
        "Be stingy with HWcc memory:  co-design a software coherence protocol for the bulk",
        "Result: up to 2.5× over optimized shared-nothing baselines, 18.5× over RDMA DBs",
        "Opens a research agenda: OCC/MVCC on CXL pods, larger pods, real HW experiments",
    ])

    s = prs.slides.add_slide(L_CONTENT)
    set_placeholder_text(s, 0, "Questions?")
    set_placeholder_text(s, 10, "Thank you!")
    fill_body_bullets(s, [
        "Paper:  https://www.usenix.org/conference/osdi25/presentation/huang-yibo",
        "Code:   https://github.com/ut-datasys/tigon",
        "Related papers worth reading:",
        ("Pasha (CIDR'25) — CXL-pod DB architecture (same authors)", 1),
        ("SiloR (SOSP'13) — the logging protocol Tigon adapts", 1),
        ("Octopus (2025) — CXL memory pooling, relevant failure models", 1),
        ("Motor (OSDI'24) — the RDMA-disaggregated baseline", 1),
    ])

    # Save
    prs.save(str(OUTFILE))
    print(f"Wrote {OUTFILE}  ({OUTFILE.stat().st_size/1024:.1f} KB)")
    print(f"Slides: {len(prs.slides)}")


if __name__ == "__main__":
    build()
