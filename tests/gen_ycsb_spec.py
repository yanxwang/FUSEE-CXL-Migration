#!/usr/bin/env python3
"""Generate synthetic YCSB-A or YCSB-C spec files for cxl_ycsb_runner.

Usage:
  gen_ycsb_spec.py <workload A|C> <num_keys> <num_trans> <out_load> <out_trans>
                   [--dist uniform|zipf] [--zipf-theta 0.99]

Emits lines of the form:
  INSERT table user<key_id>
  READ   table user<key_id>
  UPDATE table user<key_id>

Key space is [0, num_keys). Default trans distribution is Zipf with
theta=0.99 (matches the official YCSB default). Pass "--dist uniform" for
the earlier uniform behavior. Workload A = 50/50 read/update; Workload C =
100% read.
"""
import argparse
import math
import random
import sys
from pathlib import Path


def build_zipf_cdf(n: int, theta: float) -> list:
    # Precompute CDF of the Zipf distribution: P(k) proportional to 1/(k+1)**theta
    # over k in [0, n). Returns a CDF list; inversion via bisect samples a draw.
    weights = [1.0 / ((k + 1) ** theta) for k in range(n)]
    total = sum(weights)
    cdf = []
    acc = 0.0
    for w in weights:
        acc += w / total
        cdf.append(acc)
    return cdf


def zipf_draw(cdf: list, rng: random.Random) -> int:
    # Binary search on the CDF. Hot keys concentrate at the low indices.
    u = rng.random()
    lo, hi = 0, len(cdf) - 1
    while lo < hi:
        mid = (lo + hi) // 2
        if cdf[mid] < u: lo = mid + 1
        else: hi = mid
    return lo


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("workload", choices=["A", "C", "a", "c"])
    ap.add_argument("num_keys", type=int)
    ap.add_argument("num_trans", type=int)
    ap.add_argument("out_load")
    ap.add_argument("out_trans")
    ap.add_argument("--dist", choices=["uniform", "zipf"], default="zipf")
    ap.add_argument("--zipf-theta", type=float, default=0.99)
    args = ap.parse_args(argv)

    workload = args.workload.upper()
    out_load = Path(args.out_load)
    out_trans = Path(args.out_trans)

    rng = random.Random(42)

    with out_load.open("w") as f:
        for i in range(args.num_keys):
            f.write(f"INSERT usertable user{i}\n")
    print(f"wrote {out_load} ({args.num_keys} INSERT ops)")

    if workload == "A":
        ops_and_weights = [("READ", 1), ("UPDATE", 1)]
    else:
        ops_and_weights = [("READ", 1)]
    ops, weights = zip(*ops_and_weights)

    if args.dist == "zipf":
        cdf = build_zipf_cdf(args.num_keys, args.zipf_theta)
        def pick():
            return zipf_draw(cdf, rng)
    else:
        def pick():
            return rng.randrange(args.num_keys)

    with out_trans.open("w") as f:
        for _ in range(args.num_trans):
            op = rng.choices(ops, weights)[0]
            kid = pick()
            f.write(f"{op} usertable user{kid}\n")
    print(f"wrote {out_trans} ({args.num_trans} {workload}-mix ops, dist={args.dist}"
          + (f", theta={args.zipf_theta})" if args.dist == "zipf" else ")"))


if __name__ == "__main__":
    main(sys.argv[1:])
