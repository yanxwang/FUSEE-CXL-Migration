#!/usr/bin/env python3
"""Generate evidence plots for each Phase 6 sub-phase ANALYSIS.md."""

import csv
import os
import re
import subprocess

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def latest_dir(prefix):
    out = subprocess.check_output(
        f"ls -dt /home/yanwang/FUSEE/docs/{prefix}*/ 2>/dev/null | head -1",
        shell=True, text=True).strip()
    return out if out else None


def plot_phase6_0(ph_dir, out_path):
    """Phase 6.0: distribution skew (z=0.99 vs z=1.5) on read + write.
    Show HITM stays flat while thpt drops → MESI not the cause."""
    cells = [
        ("local_read", "zipf-0.99"),
        ("local_read", "zipf-1.5"),
        ("local_write", "zipf-0.99"),
        ("local_write", "zipf-1.5"),
    ]
    data = []
    for sc, kd in cells:
        fn = f"{ph_dir}/raw/ph60_{sc}_{kd.replace('-','')}_c2c_report.txt"
        h0 = f"{ph_dir}/raw/ph60_{sc}_{kd.replace('-','')}_h0.out"
        with open(fn) as f:
            txt = f.read()
        with open(h0) as f:
            h0txt = f.read()
        def g(field, source):
            m = re.search(rf'{field}\s*:\s+(\d+)', source)
            return int(m.group(1)) if m else 0
        thpt = int(re.search(r'trans_agg_thpt=(\d+)', h0txt).group(1)) / 1e6
        hitm = g("Load Local HITM", txt)
        shared = g("Total Shared Cache Lines", txt)
        # Get top-1 hot cacheline percent
        idx = txt.find("Shared Data Cache Line Table")
        sect = txt[idx:idx+2000]
        m_top = re.search(r"^\s+0\s+0x\S+\s+\S+\s+\d+\s+(\d+\.\d+)%", sect, re.MULTILINE)
        top1_pct = float(m_top.group(1)) if m_top else 0
        data.append({
            'cell': f"{sc}\n{kd}",
            'sc': sc, 'kd': kd, 'thpt': thpt, 'hitm': hitm,
            'shared': shared, 'top1_pct': top1_pct,
        })

    fig, axes = plt.subplots(1, 3, figsize=(16, 5))
    x_labels = [d['cell'] for d in data]
    x = list(range(4))
    colors = ['#4a90e2', '#a8d5e8', '#e25a4a', '#f4a89d']

    # Plot 1: thpt
    axes[0].bar(x, [d['thpt'] for d in data], color=colors)
    axes[0].set_xticks(x); axes[0].set_xticklabels(x_labels, fontsize=8)
    axes[0].set_ylabel("thpt (Mops/s)")
    axes[0].set_title("Throughput (drops 3-6× at zipf-1.5)")
    for i, d in enumerate(data):
        axes[0].text(i, d['thpt']+1, f"{d['thpt']:.2f}", ha='center', fontsize=8)

    # Plot 2: HITM count
    axes[1].bar(x, [d['hitm'] for d in data], color=colors)
    axes[1].set_xticks(x); axes[1].set_xticklabels(x_labels, fontsize=8)
    axes[1].set_ylabel("Load Local HITM (count)")
    axes[1].set_title("HITM count (~CONSTANT — debunks MESI hypothesis)")
    axes[1].axhline(y=sum(d['hitm'] for d in data)/4, color='gray', linestyle='--', alpha=0.5)
    for i, d in enumerate(data):
        axes[1].text(i, d['hitm']+1000, f"{d['hitm']:,}", ha='center', fontsize=8)

    # Plot 3: top-1 cacheline %
    axes[2].bar(x, [d['top1_pct'] for d in data], color=colors)
    axes[2].set_xticks(x); axes[2].set_xticklabels(x_labels, fontsize=8)
    axes[2].set_ylabel("Top-1 cacheline % of total HITM")
    axes[2].set_title("90% on ONE line (framework, not workload)")
    for i, d in enumerate(data):
        axes[2].text(i, d['top1_pct']+1, f"{d['top1_pct']:.1f}%", ha='center', fontsize=8)
    axes[2].set_ylim(0, 105)

    fig.suptitle("Phase 6.0: distribution skew perf c2c — MESI is NOT the cause of zipf-1.5 collapse",
                 fontsize=13, y=1.02)
    fig.tight_layout()
    fig.savefig(out_path, dpi=120, bbox_inches="tight")
    plt.close(fig)
    print(f"[plot] wrote {out_path}")


def plot_phase6a(ph_dir, out_path):
    """Phase 6a: cache% sweep on local_read. Show thpt drops 2.9× while
    HITM stays flat (1.1×) — MESI not the cause; LLC pressure is."""
    with open(f"{ph_dir}/summary.csv") as f:
        rows = list(csv.DictReader(f))

    pcts = [int(r['cache_pct']) for r in rows]
    thpt = [float(r['thpt_Mops']) for r in rows]
    hitm = [int(r['LL_HITM']) for r in rows]
    shared = [int(r['total_shared_lines']) for r in rows]
    hitm_per_line = [h/s if s>0 else 0 for h, s in zip(hitm, shared)]

    # Normalize to cache%=1 baseline
    base = {'thpt': thpt[0], 'hitm': hitm[0], 'shared': shared[0], 'hpl': hitm_per_line[0]}
    norm = {
        'thpt': [t/base['thpt'] for t in thpt],
        'hitm': [h/base['hitm'] for h in hitm],
        'shared': [s/base['shared'] for s in shared],
        'hpl': [h/base['hpl'] for h in hitm_per_line],
    }

    fig, (ax_raw, ax_norm) = plt.subplots(1, 2, figsize=(15, 6))

    # Raw values, left axis = thpt+HITM/1000+shared (scaled), right=hitm_per_line
    ax_raw.plot(pcts, thpt, 'o-', label='thpt (Mops/s)', linewidth=2.5, markersize=10, color='#2c5aa0')
    ax_raw.plot(pcts, [h/1000 for h in hitm], 's-', label='HITM (×1000)', linewidth=2, color='#d62728')
    ax_raw.plot(pcts, [s/100 for s in shared], '^-', label='shared lines (×100)', linewidth=2, color='#2ca02c')
    ax_raw.plot(pcts, hitm_per_line, 'd-', label='HITM/line', linewidth=2, color='#ff7f0e')
    ax_raw.set_xscale('log')
    ax_raw.set_xticks(pcts); ax_raw.set_xticklabels([f"{p}%" for p in pcts])
    ax_raw.set_xlabel("Cache size (% of unique keys)")
    ax_raw.set_ylabel("Value (mixed units, see legend)")
    ax_raw.set_title("Raw values: thpt drops, HITM flat, shared lines grow")
    ax_raw.legend()
    ax_raw.grid(True, which='both', alpha=0.3)

    # Normalized to cache%=1 baseline
    ax_norm.plot(pcts, norm['thpt'], 'o-', label='thpt', linewidth=2.5, markersize=10, color='#2c5aa0')
    ax_norm.plot(pcts, norm['hitm'], 's-', label='HITM count', linewidth=2, color='#d62728')
    ax_norm.plot(pcts, norm['shared'], '^-', label='shared lines', linewidth=2, color='#2ca02c')
    ax_norm.plot(pcts, norm['hpl'], 'd-', label='HITM / line', linewidth=2, color='#ff7f0e')
    ax_norm.axhline(y=1.0, color='gray', linestyle='--', alpha=0.5)
    ax_norm.set_xscale('log')
    ax_norm.set_xticks(pcts); ax_norm.set_xticklabels([f"{p}%" for p in pcts])
    ax_norm.set_yscale('log')
    ax_norm.set_xlabel("Cache size (% of unique keys)")
    ax_norm.set_ylabel("Ratio to cache%=1 baseline (log)")
    ax_norm.set_title("Normalized: thpt and HITM-per-line DROP together;\nshared lines GROW (= bigger working set = LLC pressure)")
    ax_norm.legend()
    ax_norm.grid(True, which='both', alpha=0.3)

    fig.suptitle("Phase 6a: cache% sweep perf c2c — local_read drops 2.9× because of LLC pressure, NOT MESI",
                 fontsize=13, y=1.02)
    fig.tight_layout()
    fig.savefig(out_path, dpi=120, bbox_inches="tight")
    plt.close(fig)
    print(f"[plot] wrote {out_path}")


def plot_phase6_0b(ph_dir, out_path):
    """Phase 6.0b: perf stat IPC across 4 cells. If IPC is flat across cells
    but thpt varies, the perf stat scope (whole process incl LOAD) doesn't
    isolate the difference."""
    with open(f"{ph_dir}/summary.csv") as f:
        rows = list(csv.DictReader(f))
    if len(rows) < 4:
        print(f"[plot] phase6.0b only {len(rows)} rows yet, skipping")
        return
    cells = [f"{r['scenario']}\n{r['keydist']}" for r in rows]
    thpt = [float(r['thpt_Mops']) for r in rows]
    ipc = [float(r['IPC']) for r in rows]
    cyc = [int(r['cycles']) for r in rows]
    cs = [int(r['context_switches']) for r in rows]

    fig, axes = plt.subplots(1, 3, figsize=(15, 5))
    x = list(range(len(rows)))
    colors = ['#4a90e2', '#a8d5e8', '#e25a4a', '#f4a89d']

    axes[0].bar(x, thpt, color=colors)
    axes[0].set_xticks(x); axes[0].set_xticklabels(cells, fontsize=8)
    axes[0].set_ylabel("thpt (Mops/s)")
    axes[0].set_title("Throughput (3-6× drops at zipf-1.5)")
    for i, v in enumerate(thpt):
        axes[0].text(i, v+0.5, f"{v:.2f}", ha='center', fontsize=8)

    axes[1].bar(x, ipc, color=colors)
    axes[1].set_xticks(x); axes[1].set_xticklabels(cells, fontsize=8)
    axes[1].set_ylabel("IPC (instructions per cycle)")
    axes[1].set_title("IPC is ~CONSTANT (~0.17) across cells\n(perf-stat scope = whole process; LOAD dominates → can't isolate)")
    axes[1].axhline(y=sum(ipc)/len(ipc), color='gray', linestyle='--', alpha=0.5)
    for i, v in enumerate(ipc):
        axes[1].text(i, v+0.005, f"{v:.3f}", ha='center', fontsize=8)
    axes[1].set_ylim(0, max(ipc)*1.3)

    axes[2].bar(x, cs, color=colors)
    axes[2].set_xticks(x); axes[2].set_xticklabels(cells, fontsize=8)
    axes[2].set_ylabel("context switches")
    axes[2].set_title("Context switches (if elevated under skew →\nworkers blocked on locks)")
    for i, v in enumerate(cs):
        axes[2].text(i, v+10, f"{v}", ha='center', fontsize=8)

    fig.suptitle("Phase 6.0b: perf stat IPC — process-level scope can't isolate TRANS lock cost\n"
                 "Either need TRANS-only counters OR direct CAS-retry instrumentation",
                 fontsize=12, y=1.02)
    fig.tight_layout()
    fig.savefig(out_path, dpi=120, bbox_inches="tight")
    plt.close(fig)
    print(f"[plot] wrote {out_path}")


def main():
    p60 = latest_dir("iter15A_phase6_0_dist_c2c_")
    p6a = latest_dir("iter15A_phase6a_cachepct_c2c_")
    p60b = latest_dir("iter15A_phase6_0b_perfstat_")

    if p60:
        plot_phase6_0(p60, f"{p60}/phase6_0_evidence.png")
    if p6a:
        plot_phase6a(p6a, f"{p6a}/phase6a_evidence.png")
    if p60b:
        plot_phase6_0b(p60b, f"{p60b}/phase6_0b_evidence.png")


if __name__ == "__main__":
    main()
