#!/usr/bin/env python3
"""iter-14A P6 microbench analysis — produce a summary table from MICROBENCH.tsv.

Output:
  - Per-scenario × per-T median trans_agg_thpt + w_p50/p99 + r_p50/p99
  - Cold-vs-warm delta for read scenarios
  - Bimodal check (max/min across 3 reps per cell)
"""
import csv
import statistics
import sys
from collections import defaultdict


def main(tsv_path):
    rows = []
    with open(tsv_path) as f:
        r = csv.DictReader(f, delimiter='\t')
        for row in r:
            if row['trans_agg_thpt'] == 'FAIL':
                continue
            for k in ('T', 'rep'):
                row[k] = int(row[k])
            for k in ('trans_agg_thpt', 'w_p50', 'w_p99', 'r_p50', 'r_p99'):
                row[k] = int(row[k]) if row[k] else 0
            rows.append(row)

    cells = defaultdict(list)
    for row in rows:
        key = (row['scenario'], row['keydist'], row['T'], row['cache'])
        cells[key].append(row)

    print(f"\n=== iter-14A P6 microbench summary ({len(rows)} cells, {tsv_path}) ===\n")

    print(f"{'scenario':<14} {'kd':<8} {'T':>3} {'cstate':>6} | {'median':>10} {'min':>10} {'max':>10} {'spread%':>8} | {'w_p50':>7} {'w_p99':>7} {'r_p50':>7} {'r_p99':>7}")
    print("-" * 130)

    ordered_keys = sorted(cells.keys(),
                           key=lambda k: (k[0], k[1], k[2], k[3]))
    for key in ordered_keys:
        sc, kd, T, cs = key
        reps = cells[key]
        thpts = [r['trans_agg_thpt'] for r in reps]
        med = statistics.median(thpts)
        mn = min(thpts); mx = max(thpts)
        spread = 100.0 * (mx - mn) / med if med else 0
        w50 = statistics.median([r['w_p50'] for r in reps]) / 1000.0
        w99 = statistics.median([r['w_p99'] for r in reps]) / 1000.0
        r50 = statistics.median([r['r_p50'] for r in reps]) / 1000.0
        r99 = statistics.median([r['r_p99'] for r in reps]) / 1000.0
        print(f"{sc:<14} {kd:<8} {T:>3} {cs:>6} | {med:>10} {mn:>10} {mx:>10} {spread:>+7.1f}% | {w50:>7.2f} {w99:>7.2f} {r50:>7.2f} {r99:>7.2f}")

    print("\n=== cold vs warm delta (read scenarios) ===")
    print(f"{'scenario':<14} {'kd':<8} {'T':>3} | {'cold med':>12} {'warm med':>12} {'delta%':>8}")
    print("-" * 70)
    for sc in ['local_read', 'xhost_read']:
        for kd in ['uniform', 'zipf']:
            for T in [1, 4, 32, 64]:
                cold = cells.get((sc, kd, T, 'cold'))
                warm = cells.get((sc, kd, T, 'warm'))
                if not cold or not warm:
                    continue
                cm = statistics.median([r['trans_agg_thpt'] for r in cold])
                wm = statistics.median([r['trans_agg_thpt'] for r in warm])
                d = 100.0 * (wm - cm) / cm if cm else 0
                print(f"{sc:<14} {kd:<8} {T:>3} | {cm:>12} {wm:>12} {d:>+7.2f}%")

    print("\n=== uniform vs zipf delta (per scenario, cold) ===")
    print(f"{'scenario':<14} {'T':>3} {'cs':>6} | {'uniform med':>12} {'zipf med':>12} {'delta%':>8}")
    print("-" * 70)
    for sc in ['local_read', 'xhost_read', 'local_write', 'xhost_write']:
        for T in [1, 4, 32, 64]:
            for cs in ['cold', 'warm']:
                u = cells.get((sc, 'uniform', T, cs))
                z = cells.get((sc, 'zipf', T, cs))
                if not u or not z:
                    continue
                um = statistics.median([r['trans_agg_thpt'] for r in u])
                zm = statistics.median([r['trans_agg_thpt'] for r in z])
                d = 100.0 * (zm - um) / um if um else 0
                print(f"{sc:<14} {T:>3} {cs:>6} | {um:>12} {zm:>12} {d:>+7.2f}%")

    print("\n=== bimodal check (cells with spread > 20%) ===")
    bimodal = []
    for key, reps in cells.items():
        if len(reps) < 2: continue
        thpts = [r['trans_agg_thpt'] for r in reps]
        med = statistics.median(thpts); mn = min(thpts); mx = max(thpts)
        if med == 0: continue
        spread = 100.0 * (mx - mn) / med
        if spread > 20:
            bimodal.append((spread, key, mn, med, mx))
    for spread, key, mn, med, mx in sorted(bimodal, reverse=True):
        sc, kd, T, cs = key
        print(f"  {sc:<14} {kd:<8} T={T:<3} {cs:<6}  med={med:>10}  min={mn:>10}  max={mx:>10}  spread={spread:>5.1f}%")


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'MICROBENCH.tsv')
