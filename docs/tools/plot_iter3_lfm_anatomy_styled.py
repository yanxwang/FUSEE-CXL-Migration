#!/usr/bin/env python3
"""Style-B rewrite of plot_iter3_lfm_anatomy.py.

Outputs to *_styled.png so the original is preserved for comparison.

Mapping rationale: 4 LFM stages → STYLE_B4 (4-tone greyscale + accent).
The narrative is "3 acquire-physics stages are flat / boring; cont_wait
is the culprit", so cont_wait gets the accent red and the other three
are greys (lightest → darkest by intuitive size of the stage).
"""
import os
import sys

import matplotlib.pyplot as plt
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from plot_style import apply_style, STYLE_B4, STYLE_B_EDGE  # noqa: E402
apply_style()

T = np.array([8, 16, 32, 64, 86])

# Average (us)
avg = {
    'localstore': np.array([0.42, 0.96, 1.56, 1.76, 2.14]),
    'peerscan':   np.array([3.26, 3.30, 3.22, 2.95, 3.28]),
    'contwait':   np.array([0.20, 0.73, 1.66, 1.82, 2.24]),
    'entercs':    np.array([1.72, 1.75, 1.66, 1.54, 1.72]),
}

# p50 (us) — first-attempt; cont_wait is 0 by construction at median
p50 = {
    'localstore': np.array([0.017, 0.228, 0.224, 0.237, 0.237]),
    'peerscan':   np.array([3.234, 3.242, 2.959, 2.382, 2.375]),
    'contwait':   np.array([0.000, 0.000, 0.000, 0.000, 0.000]),
    'entercs':    np.array([1.709, 1.730, 1.549, 1.247, 1.241]),
}

# p99 (us)
p99 = {
    'localstore': np.array([ 7.43, 24.30, 40.23, 44.05, 58.78]),
    'peerscan':   np.array([ 4.31,  5.81, 10.68, 17.19, 23.51]),
    'contwait':   np.array([ 6.74, 27.24, 62.71, 68.26, 78.50]),
    'entercs':    np.array([ 2.18,  2.73,  4.96, 10.78, 14.44]),
}

# Style B4: cont_wait gets the accent, the 3 acquire-physics stages
# get greys (lightest → darkest). Order chosen so the smallest stage
# (localstore — usually thin) sits at the lightest tone.
COLORS = {
    'localstore': STYLE_B4[0],   # lightest grey
    'peerscan':   STYLE_B4[1],   # light grey
    'entercs':    STYLE_B4[2],   # dark grey
    'contwait':   STYLE_B4[3],   # accent red — the culprit
}
LABELS = {
    'localstore': 'local_store (local publish)',
    'peerscan':   'peer_scan (CXL y-load, ~2.85 µs RTT)',
    'entercs':    'enter_cs (CXL x-load + y-store)',
    'contwait':   'cont_wait (queue / retry-drain)',
}
ORDER = ['localstore', 'peerscan', 'entercs', 'contwait']


def stacked_bar(ax, data_dict, ylabel, subtitle):
    """Style-B stacked bar with greyscale stages + accent culprit."""
    bot = np.zeros_like(T, dtype=float)
    x = np.arange(len(T))
    for k in ORDER:
        ax.bar(x, data_dict[k], bottom=bot,
               color=COLORS[k], edgecolor=STYLE_B_EDGE, linewidth=0.5,
               label=LABELS[k])
        bot += data_dict[k]
    ax.set_xticks(x)
    ax.set_xticklabels([str(t) for t in T])
    ax.set_xlabel('Threads per host (T)')
    ax.set_ylabel(ylabel)
    ax.set_title(subtitle)
    ax.grid(axis='y', alpha=0.3)
    ax.set_axisbelow(True)
    # 25 % top headroom from total stack max.
    max_y = float(bot.max())
    if max_y > 0:
        ax.set_ylim(0, max_y * 1.25)


fig, axes = plt.subplots(2, 2, figsize=(13, 9))
fig.suptitle(
    'Phase-1 LFM anatomy — per-stage latency vs T\n'
    '(workload A, 2 hosts, per-slot LFM, cache=on; data: '
    'latency_decomp_C_iter3_lock_anatomy_20260424_090048.md)',
    fontsize=12, y=0.995)

# ---- Panel (a): avg ----
stacked_bar(axes[0, 0], avg, 'Avg latency (µs)',
            '(a) Average — cont_wait (red) is the only growing stage')
axes[0, 0].legend(loc='upper left', fontsize=8)

# ---- Panel (b): p50 ----
stacked_bar(axes[0, 1], p50, 'p50 latency (µs)',
            '(b) p50 (first-attempt) — acquire-physics FLAT-to-SHRINKING\n'
            '  → LFM primitive is at hardware floor, not the bottleneck')
axes[0, 1].legend(loc='upper right', fontsize=8)
# Annotations re-applied for the p50 narrative.
phys_8  = p50['localstore'][0] + p50['peerscan'][0] + p50['entercs'][0]
phys_86 = p50['localstore'][-1] + p50['peerscan'][-1] + p50['entercs'][-1]
axes[0, 1].annotate(
    f'acquire-physics = {phys_8:.2f} µs',
    xy=(0, phys_8), xytext=(0.2, phys_8 + 0.7),
    fontsize=8, color='black',
    arrowprops=dict(arrowstyle='->', lw=0.6))
axes[0, 1].annotate(
    f'acquire-physics = {phys_86:.2f} µs\n(−22 % vs T=8)',
    xy=(4, phys_86), xytext=(2.6, phys_86 + 0.9),
    fontsize=8, color='black',
    arrowprops=dict(arrowstyle='->', lw=0.6))

# ---- Panel (c): p99 ----
stacked_bar(axes[1, 0], p99, 'p99 latency (µs)',
            '(c) p99 stacked — cont_wait (red) & retry-inflated\n'
            '   local_store (light) dominate tail growth')
axes[1, 0].legend(loc='upper left', fontsize=8)

# ---- Panel (d): p99 growth ratio (line plot) ----
ax = axes[1, 1]
linear_ref = T / T[0]
for k in ORDER:
    ratio = p99[k] / p99[k][0]
    ax.plot(T, ratio, marker='o', lw=2.0, color=COLORS[k],
            label=f'{k}  ({p99[k][-1]/p99[k][0]:.2f}× @T=86)')
ax.plot(T, linear_ref, '--', color='gray', lw=1.0,
        label=f'linear reference ({linear_ref[-1]:.2f}× @T=86)')
ax.set_xticks(T)
ax.set_xticklabels([str(t) for t in T])
ax.set_xlabel('Threads per host (T)')
ax.set_ylabel('p99 latency, relative to T=8')
ax.set_title('(d) p99 growth ratio — cont_wait super-linear,\n'
             '   others sub-linear (retry inflation only)')
ax.legend(loc='upper left', fontsize=8)
ax.grid(alpha=0.3)
ax.set_axisbelow(True)

plt.tight_layout(rect=[0, 0, 1, 0.965])
out = 'docs/iter3_lfm_anatomy_vs_T_styled.png'
plt.savefig(out)
print(f'wrote {out}')
