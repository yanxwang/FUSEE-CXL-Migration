#!/usr/bin/env python3
# Per-workload throughput + p50 latency bar charts, for the two
# iter-3 Phase-3 sweeps:
#   - 20260424_052400: 200 k ops (phase-3 micro-batch final)
#   - 20260424_091433: 2 M ops (2 M-ops steady-state validation)
#
# For each sweep:
#   (i)  throughput vs workload, grouped bars T ∈ {32, 64, 86}
#   (ii) p50 latency vs workload, grouped bars T ∈ {32, 64, 86}
#
# p50 is max(r_p50_ns, w_p50_ns) — the slower op governs the per-op
# critical path; for read-only C this is r_p50.
# Only cache=1 (cache-on) lines are plotted.

import os
import re
import matplotlib.pyplot as plt
import numpy as np

SWEEPS = [
    ('logs/g34_scaling_sweep_C_only_microbatch_20260424_052400/SUMMARY.log',
     '20260424_052400 — Phase-3 micro-batch (200 k ops)',
     'sweep_052400_200k'),
    ('logs/g34_scaling_sweep_C_only_2M_20260424_091433/SUMMARY.log',
     '20260424_091433 — Phase-3 micro-batch (2 M ops, steady-state)',
     'sweep_091433_2M'),
]

WORKLOADS = ['a', 'b', 'c', 'd', 'f']
WORKLOAD_LABEL = {'a': 'A (50R/50U)', 'b': 'B (95R/5U)',
                  'c': 'C (100R)',   'd': 'D (95R/5INS)',
                  'f': 'F (50R/50RMW)'}
T_LIST = [32, 64, 86]
T_COLOR = {32: '#6baed6', 64: '#3182bd', 86: '#08519c'}

LINE_RE = re.compile(
    r'YCSB\s+opt=C\s+cache=(\d).*?threads_eff=(\d+).*?'
    r'trans_agg_thpt=(\d+).*?w_p50_ns=(\d+).*?r_p50_ns=(\d+).*?'
    r'#\s+workload([a-z])_')


def parse(path):
    """dict: {(workload, T): (thpt_mops, p50_us_max_of_rw)}"""
    data = {}
    with open(path) as f:
        for line in f:
            m = LINE_RE.search(line)
            if not m:
                continue
            cache = int(m.group(1))
            if cache != 1:
                continue
            T = int(m.group(2))
            thpt = int(m.group(3))
            w_p50 = int(m.group(4))
            r_p50 = int(m.group(5))
            wl = m.group(6)
            if wl not in WORKLOADS or T not in T_LIST:
                continue
            p50_ns = max(w_p50, r_p50)
            data[(wl, T)] = (thpt / 1e6, p50_ns / 1000.0)
    return data


def grouped_bars(ax, data, idx, title, ylabel, fmt):
    width = 0.26
    x = np.arange(len(WORKLOADS))
    for i, T in enumerate(T_LIST):
        ys = []
        for wl in WORKLOADS:
            v = data.get((wl, T), (0.0, 0.0))[idx]
            ys.append(v)
        offset = (i - 1) * width
        bars = ax.bar(x + offset, ys, width,
                      color=T_COLOR[T], edgecolor='white',
                      label=f'T={T}')
        for bar, v in zip(bars, ys):
            if v > 0:
                ax.annotate(fmt.format(v),
                            xy=(bar.get_x() + bar.get_width() / 2,
                                bar.get_height()),
                            xytext=(0, 2), textcoords='offset points',
                            ha='center', va='bottom', fontsize=7)
    ax.set_xticks(x)
    ax.set_xticklabels([WORKLOAD_LABEL[w] for w in WORKLOADS])
    ax.set_xlabel('YCSB workload')
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.legend(loc='best', fontsize=9)
    ax.grid(axis='y', alpha=0.3)


for summary_path, sweep_title, slug in SWEEPS:
    if not os.path.exists(summary_path):
        print(f'skip (missing): {summary_path}')
        continue
    data = parse(summary_path)

    # --- Throughput plot ---
    fig, ax = plt.subplots(figsize=(9.5, 5))
    grouped_bars(ax, data, idx=0,
                 title=f'Throughput — {sweep_title}\n'
                       '(protocol C, cache=on, T ∈ {32, 64, 86})',
                 ylabel='Aggregate throughput (Mops/s)',
                 fmt='{:.1f}')
    ax.axhline(20, color='red', linestyle='--', lw=1.2, alpha=0.6,
               label='20 Mops/s target')
    ax.legend(loc='best', fontsize=9)
    out = f'docs/iter3_{slug}_thpt.png'
    plt.tight_layout()
    plt.savefig(out, dpi=140, bbox_inches='tight')
    plt.close(fig)
    print(f'wrote {out}')

    # --- p50 latency plot ---
    fig, ax = plt.subplots(figsize=(9.5, 5))
    grouped_bars(ax, data, idx=1,
                 title=f'p50 latency (max of r_p50, w_p50) — '
                       f'{sweep_title}\n'
                       '(protocol C, cache=on, T ∈ {32, 64, 86})',
                 ylabel='p50 op latency (µs, slower of read/write)',
                 fmt='{:.2f}')
    out = f'docs/iter3_{slug}_p50.png'
    plt.tight_layout()
    plt.savefig(out, dpi=140, bbox_inches='tight')
    plt.close(fig)
    print(f'wrote {out}')

print('done')
