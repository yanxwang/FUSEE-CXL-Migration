#!/usr/bin/env python3
"""Plot FUSEE c1/c2 latency benchmark results.

Generates:
- cdf.png      : CDF of latency per op (like FUSEE paper Fig 10)
- cdf_clip.png : CDF zoomed into 0..50us (hide long tail)
- summary.png  : bar chart of p50/p95/p99 per op
- histogram.png: histogram per op (log y)
"""
import os
import sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "results")

OPS = [
    ("INSERT", "insert_lat-2rp.txt", "#1f77b4"),
    ("SEARCH", "search_lat-2rp.txt", "#2ca02c"),
    ("UPDATE", "update_lat-2rp.txt", "#ff7f0e"),
    ("DELETE", "delete_lat-2rp.txt", "#d62728"),
]

def load(fname):
    return np.loadtxt(os.path.join(RES, fname), dtype=np.int64)

def pct(arr, p):
    return float(np.percentile(arr, p))

data = {}
for name, fname, color in OPS:
    arr = load(fname)
    data[name] = (arr, color)
    print(f"{name:7s}: n={len(arr):>7d}  min={arr.min()}us  avg={arr.mean():.2f}us"
          f"  p50={pct(arr,50):.0f}  p95={pct(arr,95):.0f}  p99={pct(arr,99):.0f}"
          f"  p99.9={pct(arr,99.9):.0f}  max={arr.max()}us")

# ---------- CDF (full) ----------
fig, ax = plt.subplots(figsize=(7, 4.5), dpi=120)
for name, (arr, color) in data.items():
    xs = np.sort(arr)
    ys = np.arange(1, len(xs)+1) / len(xs)
    ax.plot(xs, ys, label=name, color=color, linewidth=1.6)
ax.set_xlabel("Latency (μs)")
ax.set_ylabel("CDF")
ax.set_title("FUSEE latency CDF  (c1/c2, BlueField-3 / EDR IB, 2 MN + 1 CN, num_rep=2)")
ax.set_xscale("log")
ax.grid(True, alpha=0.3)
ax.legend()
fig.tight_layout()
fig.savefig(os.path.join(HERE, "cdf.png"))
print("wrote cdf.png")

# ---------- CDF (zoomed 0..50μs, linear x) — matches FUSEE paper Fig 10 style ----------
fig, ax = plt.subplots(figsize=(7, 4.5), dpi=120)
for name, (arr, color) in data.items():
    xs = np.sort(arr)
    ys = np.arange(1, len(xs)+1) / len(xs)
    ax.plot(xs, ys, label=name, color=color, linewidth=1.8)
ax.set_xlabel("Latency (μs)")
ax.set_ylabel("CDF")
ax.set_title("FUSEE latency CDF — zoomed (0..50μs, linear)")
ax.set_xlim(0, 50)
ax.grid(True, alpha=0.3)
ax.legend(loc="lower right")
fig.tight_layout()
fig.savefig(os.path.join(HERE, "cdf_clip.png"))
print("wrote cdf_clip.png")

# ---------- percentile bar chart ----------
fig, ax = plt.subplots(figsize=(7, 4.5), dpi=120)
xs = np.arange(len(data))
w = 0.22
labels = list(data.keys())
p50 = [pct(data[n][0], 50)  for n in labels]
p95 = [pct(data[n][0], 95)  for n in labels]
p99 = [pct(data[n][0], 99)  for n in labels]
p999= [pct(data[n][0], 99.9)for n in labels]

ax.bar(xs-1.5*w, p50,  w, label="p50",   color="#1f77b4")
ax.bar(xs-0.5*w, p95,  w, label="p95",   color="#2ca02c")
ax.bar(xs+0.5*w, p99,  w, label="p99",   color="#ff7f0e")
ax.bar(xs+1.5*w, p999, w, label="p99.9", color="#d62728")
for i, n in enumerate(labels):
    ax.text(i-1.5*w, p50[i]+0.5,  f"{p50[i]:.0f}",  ha="center", fontsize=8)
    ax.text(i+1.5*w, p999[i]+0.5, f"{p999[i]:.0f}", ha="center", fontsize=8)
ax.set_xticks(xs)
ax.set_xticklabels(labels)
ax.set_ylabel("Latency (μs)")
ax.set_title("Tail-latency percentiles")
ax.grid(True, axis="y", alpha=0.3)
ax.legend()
fig.tight_layout()
fig.savefig(os.path.join(HERE, "summary.png"))
print("wrote summary.png")

# ---------- histograms (subplot grid) ----------
fig, axes = plt.subplots(2, 2, figsize=(10, 6.5), dpi=120, sharex=False)
axes = axes.flatten()
for ax, (name, (arr, color)) in zip(axes, data.items()):
    xmax = pct(arr, 99.5)
    clipped = arr[arr <= xmax]
    bins = np.linspace(arr.min()-0.5, xmax+0.5, 60)
    ax.hist(clipped, bins=bins, color=color, alpha=0.85, edgecolor="white", linewidth=0.3)
    ax.axvline(arr.mean(),         color="black", linestyle="--", linewidth=0.8, label=f"avg={arr.mean():.1f}")
    ax.axvline(pct(arr, 50),       color="darkred", linestyle=":",  linewidth=0.8, label=f"p50={pct(arr,50):.0f}")
    ax.set_title(f"{name}  (n={len(arr)}, max={arr.max()}μs)")
    ax.set_xlabel("Latency (μs)")
    ax.set_ylabel("Count (≤p99.5)")
    ax.legend(fontsize=8)
    ax.grid(True, alpha=0.3)
fig.suptitle("FUSEE per-op latency histograms — 2 MN + 1 CN on c1/c2", fontsize=12)
fig.tight_layout()
fig.savefig(os.path.join(HERE, "histogram.png"))
print("wrote histogram.png")
