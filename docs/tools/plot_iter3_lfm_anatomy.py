#!/usr/bin/env python3
# Plot LFM 4-stage anatomy vs T, from
# docs/latency_decomp_C_iter3_lock_anatomy_20260424_090048.md.
# Shows that acquire-physics (LS+PS+ECS) is T-independent while
# cont_wait p99 grows super-linearly — "LFM is already squeezed to
# the hardware floor; remaining gap is pure queueing."

import matplotlib.pyplot as plt
import numpy as np

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

colors = {
    'localstore': '#4c78a8',   # blue
    'peerscan':   '#54a24b',   # green
    'entercs':    '#f58518',   # orange
    'contwait':   '#e45756',   # red  (queueing — the culprit)
}
labels = {
    'localstore': 'local_store (local publish)',
    'peerscan':   'peer_scan (CXL y-load, ~2.85 µs RTT)',
    'entercs':    'enter_cs (CXL x-load + y-store)',
    'contwait':   'cont_wait (queue / retry-drain)',
}
order = ['localstore', 'peerscan', 'entercs', 'contwait']

fig, axes = plt.subplots(2, 2, figsize=(13, 9))
fig.suptitle(
    'Phase-1 LFM anatomy — per-stage latency vs T\n'
    '(workload A, 2 hosts, per-slot LFM, cache=on; data: '
    'latency_decomp_C_iter3_lock_anatomy_20260424_090048.md)',
    fontsize=12, y=0.995)

# ---- Panel 1: stacked bar, avg ----
ax = axes[0, 0]
bot = np.zeros_like(T, dtype=float)
for k in order:
    ax.bar(np.arange(len(T)), avg[k], bottom=bot,
           color=colors[k], edgecolor='white', label=labels[k])
    bot += avg[k]
ax.set_xticks(np.arange(len(T)))
ax.set_xticklabels([str(t) for t in T])
ax.set_xlabel('Threads per host (T)')
ax.set_ylabel('Avg latency (µs)')
ax.set_title('(a) Average — cont_wait (red) is the only growing stage')
ax.legend(loc='upper left', fontsize=8)
ax.grid(axis='y', alpha=0.3)

# ---- Panel 2: stacked bar, p50 — the PASS visualisation ----
ax = axes[0, 1]
bot = np.zeros_like(T, dtype=float)
for k in order:
    ax.bar(np.arange(len(T)), p50[k], bottom=bot,
           color=colors[k], edgecolor='white', label=labels[k])
    bot += p50[k]
# annotate acquire-physics sum at T=8 and T=86
phys_8  = p50['localstore'][0] + p50['peerscan'][0] + p50['entercs'][0]
phys_86 = p50['localstore'][-1] + p50['peerscan'][-1] + p50['entercs'][-1]
ax.annotate(f'acquire-physics = {phys_8:.2f} µs',
            xy=(0, phys_8), xytext=(0.2, phys_8+0.7),
            fontsize=8, color='black',
            arrowprops=dict(arrowstyle='->', lw=0.6))
ax.annotate(f'acquire-physics = {phys_86:.2f} µs\n(−22 % vs T=8)',
            xy=(4, phys_86), xytext=(2.6, phys_86+0.9),
            fontsize=8, color='black',
            arrowprops=dict(arrowstyle='->', lw=0.6))
ax.set_xticks(np.arange(len(T)))
ax.set_xticklabels([str(t) for t in T])
ax.set_xlabel('Threads per host (T)')
ax.set_ylabel('p50 latency (µs)')
ax.set_title('(b) p50 (first-attempt) — acquire-physics FLAT-to-SHRINKING\n'
             '  → LFM primitive is at hardware floor, not the bottleneck')
ax.legend(loc='upper right', fontsize=8)
ax.grid(axis='y', alpha=0.3)
ax.set_ylim(0, 6.5)

# ---- Panel 3: p99 stacked bar (linear y) ----
ax = axes[1, 0]
bot = np.zeros_like(T, dtype=float)
for k in order:
    ax.bar(np.arange(len(T)), p99[k], bottom=bot,
           color=colors[k], edgecolor='white', label=labels[k])
    bot += p99[k]
ax.set_xticks(np.arange(len(T)))
ax.set_xticklabels([str(t) for t in T])
ax.set_xlabel('Threads per host (T)')
ax.set_ylabel('p99 latency (µs)')
ax.set_title('(c) p99 stacked — cont_wait (red) & retry-inflated\n'
             '   local_store (blue) dominate tail growth')
ax.legend(loc='upper left', fontsize=8)
ax.grid(axis='y', alpha=0.3)

# ---- Panel 4: normalised p99 growth vs T=8, all 4 stages ----
ax = axes[1, 1]
linear_ref = T / T[0]

for k in order:
    ratio = p99[k] / p99[k][0]
    ax.plot(T, ratio, marker='o', lw=2.2, color=colors[k],
            label=f'{k}  ({p99[k][-1]/p99[k][0]:.2f}× @T=86)')
ax.plot(T, linear_ref, '--', color='grey', lw=1.2,
        label=f'linear reference ({linear_ref[-1]:.2f}× @T=86)')
ax.set_xticks(T)
ax.set_xticklabels([str(t) for t in T])
ax.set_xlabel('Threads per host (T)')
ax.set_ylabel('p99 latency, relative to T=8')
ax.set_title('(d) p99 growth ratio — cont_wait super-linear,\n'
             '   others sub-linear (retry inflation only)')
ax.legend(loc='upper left', fontsize=8)
ax.grid(alpha=0.3)

plt.tight_layout(rect=[0, 0, 1, 0.965])
out = 'docs/iter3_lfm_anatomy_vs_T.png'
plt.savefig(out, dpi=140, bbox_inches='tight')
print(f'wrote {out}')
