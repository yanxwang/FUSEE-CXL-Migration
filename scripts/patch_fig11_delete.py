#!/usr/bin/env python3
"""Patch the DELETE column of an existing Fig 11 summary.csv with values
from a separate DELETE-only sweep.

Usage:
  python3 scripts/patch_fig11_delete.py <fig11_summary.csv> <delete_summary.csv>
"""
import csv, sys, os, shutil

if len(sys.argv) < 3:
    print(__doc__); sys.exit(1)
FIG11 = sys.argv[1]
DEL   = sys.argv[2]

# Backup original.
shutil.copy(FIG11, FIG11 + ".v2-pre-delete-patch.bak")

del_map = {}
with open(DEL) as f:
    for r in csv.DictReader(f):
        del_map[(r["host"], int(r["num_clients"]))] = int(r["delete_tpt"])

# Rewrite Fig 11 summary with patched delete_tpt.
rows = []
with open(FIG11) as f:
    rdr = csv.DictReader(f)
    fieldnames = rdr.fieldnames
    for r in rdr:
        key = (r["host"], int(r["num_clients"]))
        if key in del_map:
            old = r["delete_tpt"]
            r["delete_tpt"] = str(del_map[key])
            print(f"  {key}: delete {old} → {r['delete_tpt']}")
        rows.append(r)

with open(FIG11, "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=fieldnames)
    w.writeheader()
    w.writerows(rows)
print(f"\nPatched {FIG11}")
print(f"Backup at {FIG11}.v2-pre-delete-patch.bak")
