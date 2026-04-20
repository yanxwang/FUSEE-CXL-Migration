#!/usr/bin/env python3
"""Generate synthetic YCSB-A or YCSB-C spec files for cxl_ycsb_runner.

Usage:
  gen_ycsb_spec.py <workload A|C> <num_keys> <num_trans> <out_load> <out_trans>

Emits lines of the form:
  INSERT table user<key_id>
  READ   table user<key_id>
  UPDATE table user<key_id>

Key space is [0, num_keys); trans phase draws keys uniformly. Workload A =
50/50 read/update; Workload C = 100% read. Good enough for a first-pass port;
real YCSB uses a Zipf distribution — easy to swap in later.
"""
import random
import sys
from pathlib import Path

if len(sys.argv) != 6:
    print(__doc__, file=sys.stderr)
    sys.exit(2)

workload = sys.argv[1].upper()
num_keys = int(sys.argv[2])
num_trans = int(sys.argv[3])
out_load = Path(sys.argv[4])
out_trans = Path(sys.argv[5])

if workload not in ("A", "C"):
    print(f"unsupported workload '{workload}', use A or C", file=sys.stderr)
    sys.exit(2)

random.seed(42)

with out_load.open("w") as f:
    for i in range(num_keys):
        f.write(f"INSERT usertable user{i}\n")
print(f"wrote {out_load} ({num_keys} INSERT ops)")

if workload == "A":
    ops = ["READ", "UPDATE"]
    weights = [1, 1]
else:  # C
    ops = ["READ"]
    weights = [1]

with out_trans.open("w") as f:
    for _ in range(num_trans):
        op = random.choices(ops, weights)[0]
        kid = random.randrange(num_keys)
        f.write(f"{op} usertable user{kid}\n")
print(f"wrote {out_trans} ({num_trans} {workload}-mix ops)")
