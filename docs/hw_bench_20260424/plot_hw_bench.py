#!/usr/bin/env python3
# Plot DRAM vs CXL bandwidth + latency vs thread count on g3.
# Data from docs/hw_bench_20260424/g3_cxl_dram_sweep.log

import matplotlib.pyplot as plt
import numpy as np

T = np.array([1, 4, 8, 16, 32, 64, 86])

dram = {
    'seq_read':  np.array([ 9820, 32836, 60447,106081,158128,217071,238881]),
    'seq_write': np.array([ 7257, 29580, 51010, 75058,101452,182817,209114]),
    'rand_read': np.array([ 5640, 22051, 55664, 84578,156506,261670,328121]),
    'lat_idle':  np.array([121, 129, 124, 129, 123, 123, 131]),
    'lat_flush': np.array([194, 252, 217, 255, 217, 207, 246]),
}
cxl = {
    'seq_read':  np.array([ 6625, 18589, 21073, 22559, 23451, 24003, 24405]),
    'seq_write': np.array([ 7873, 16469, 14565, 17685, 21750, 14770, 14128]),
    'rand_read': np.array([ 2055,  8691, 16458, 25672, 26282, 26109, 26354]),
    'lat_idle':  np.array([590, 621, 588, 629, 606, 608, 555]),
    'lat_flush': np.array([1102,1147, 727,1150,1150,1149,1120]),
}

fig, axes = plt.subplots(1, 3, figsize=(16, 5))

# --- (a) Bandwidth vs T ---
ax = axes[0]
ax.plot(T, dram['seq_read'] / 1024, 'o-', color='#1f77b4', lw=2, label='DRAM seq_read')
ax.plot(T, dram['seq_write']/ 1024, 's-', color='#1f77b4', lw=2, ls='--', label='DRAM seq_write')
ax.plot(T, dram['rand_read']/ 1024, '^-', color='#1f77b4', lw=2, ls=':', label='DRAM rand_read')
ax.plot(T, cxl['seq_read']  / 1024, 'o-', color='#d62728', lw=2, label='CXL seq_read')
ax.plot(T, cxl['seq_write'] / 1024, 's-', color='#d62728', lw=2, ls='--', label='CXL seq_write')
ax.plot(T, cxl['rand_read'] / 1024, '^-', color='#d62728', lw=2, ls=':', label='CXL rand_read')
ax.set_xlabel('Threads')
ax.set_ylabel('Bandwidth (GB/s)')
ax.set_title('(a) DRAM vs CXL bandwidth\n(single-host, 8 GiB region)')
ax.set_xscale('log', base=2)
ax.set_xticks(T)
ax.set_xticklabels([str(t) for t in T])
ax.legend(fontsize=8, loc='upper left')
ax.grid(alpha=0.3)

# --- (b) Latency vs T ---
ax = axes[1]
ax.plot(T, dram['lat_idle'],  'o-', color='#1f77b4', lw=2, label='DRAM random load (warm, L3 miss)')
ax.plot(T, dram['lat_flush'], 's-', color='#1f77b4', lw=2, ls='--', label='DRAM clflushopt+mfence+load')
ax.plot(T, cxl['lat_idle'],   'o-', color='#d62728', lw=2, label='CXL random load (single-host)')
ax.plot(T, cxl['lat_flush'],  's-', color='#d62728', lw=2, ls='--', label='CXL clflushopt+mfence+load (1 host)')
ax.axhline(2820, color='#8c564b', ls='-.', lw=1.5,
           label='CXL peer_scan (cross-host, Phase-1)')
ax.axhline(4370, color='#9467bd', ls=':', lw=1.5,
           label='LFM acquire (2×RTT, Phase-1)')
ax.set_xlabel('Threads')
ax.set_ylabel('Latency per access (ns)')
ax.set_title('(b) DRAM vs CXL access latency\n(single-thread pointer chase, 200 k iter)')
ax.set_xscale('log', base=2)
ax.set_xticks(T)
ax.set_xticklabels([str(t) for t in T])
ax.legend(fontsize=7, loc='center right')
ax.grid(alpha=0.3)

# --- (c) DRAM/CXL ratio vs T ---
ax = axes[2]
ratio_seq_read  = dram['seq_read']  / cxl['seq_read']
ratio_seq_write = dram['seq_write'] / cxl['seq_write']
ratio_rand_read = dram['rand_read'] / cxl['rand_read']
ratio_lat_flush = cxl['lat_flush']  / dram['lat_flush']
ax.plot(T, ratio_seq_read,  'o-', color='#2ca02c', lw=2, label='seq_read BW ratio')
ax.plot(T, ratio_seq_write, 's-', color='#2ca02c', lw=2, ls='--', label='seq_write BW ratio')
ax.plot(T, ratio_rand_read, '^-', color='#2ca02c', lw=2, ls=':', label='rand_read BW ratio')
ax.plot(T, ratio_lat_flush, 'o-', color='#ff7f0e', lw=2, label='lat_flush ratio (CXL/DRAM)')
ax.set_xlabel('Threads')
ax.set_ylabel('Ratio (DRAM/CXL for BW, CXL/DRAM for latency)')
ax.set_title('(c) Gap: DRAM outperforms CXL by ~5–12×\n(BW gap widens with T; latency gap flat)')
ax.set_xscale('log', base=2)
ax.set_xticks(T)
ax.set_xticklabels([str(t) for t in T])
ax.legend(fontsize=8, loc='upper left')
ax.grid(alpha=0.3)

plt.tight_layout()
out = 'docs/hw_bench_20260424/dram_vs_cxl.png'
plt.savefig(out, dpi=140, bbox_inches='tight')
print(f'wrote {out}')
