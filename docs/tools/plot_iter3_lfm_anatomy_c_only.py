#!/usr/bin/env python3
"""Standalone re-render of panel (c) of iter3_lfm_anatomy_vs_T.png.

Just the p99 stacked bar with all 4 stages — title changed to
"LFM latency decompose with clients scaling".

Uses the ORIGINAL colors (not the styled greyscale-with-accent set).
"""
import matplotlib.pyplot as plt
import numpy as np

T = np.array([8, 16, 32, 64, 86])

# p99 (us) — same source data as the original panel (c)
p99 = {
    'localstore': np.array([ 7.43, 24.30, 40.23, 44.05, 58.78]),
    'peerscan':   np.array([ 4.31,  5.81, 10.68, 17.19, 23.51]),
    'contwait':   np.array([ 6.74, 27.24, 62.71, 68.26, 78.50]),
    'entercs':    np.array([ 2.18,  2.73,  4.96, 10.78, 14.44]),
}

colors = {
    'localstore': '#4c78a8',   # blue
    'peerscan':   '#54a24b',   # green
    'entercs':    '#f58518',   # orange
    'contwait':   '#e45756',   # red (queueing — the culprit)
}
labels = {
    'localstore': 'local_store (local publish)',
    'peerscan':   'peer_scan (CXL y-load, ~2.85 µs RTT)',
    'entercs':    'enter_cs (CXL x-load + y-store)',
    'contwait':   'cont_wait (queue / retry-drain)',
}
order = ['localstore', 'peerscan', 'entercs', 'contwait']

fig, ax = plt.subplots(figsize=(8, 6))

bot = np.zeros_like(T, dtype=float)
x = np.arange(len(T))
for k in order:
    ax.bar(x, p99[k], bottom=bot,
           color=colors[k], edgecolor='white', label=labels[k])
    bot += p99[k]
ax.set_xticks(x)
ax.set_xticklabels([str(t) for t in T])
ax.set_xlabel('Clients per host (T)')
ax.set_ylabel('p99 latency (µs)')
ax.set_title('LFM latency decompose with clients scaling')
ax.grid(axis='y', alpha=0.3)
ax.set_axisbelow(True)
max_y = float(bot.max())
if max_y > 0:
    ax.set_ylim(0, max_y * 1.20)
ax.legend(loc='upper left', fontsize=9)

plt.tight_layout()
out = 'docs/iter3_lfm_anatomy_c_only.png'
plt.savefig(out, dpi=140)
print(f'wrote {out}')
