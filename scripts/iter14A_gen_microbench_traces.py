#!/usr/bin/env python3
"""iter-14A Phase 3 — microbench trace generator.

Generates 8 trace pairs (4 scenarios × 2 hosts):
  bench_<scenario>_<keyDist>_h<0|1>.spec_load
  bench_<scenario>_<keyDist>_h<0|1>.spec_trans

scenarios:
  local_read   : trans is 100% READ, key range = host's own sharding partition
  xhost_read   : trans is 100% READ, key range = peer host's sharding partition
  local_write  : trans is 100% UPDATE, key range = host's own partition
  xhost_write  : trans is 100% UPDATE, key range = peer's partition

keyDist:
  uniform / zipf-0.99

Default: 2,000,000 unique load keys (INSERT); 1,000,000 trans ops per host.

Sharding: num_hosts=2 default; key→owner via FNV-1a top-bit slice
(matches `cxl_sharding.h::host_of`).

Usage:
  python3 iter14A_gen_microbench_traces.py <out_dir>
"""

import argparse
import os
import random
import sys


def fnv1a_u64(key: int) -> int:
    """FNV-1a over the 8 bytes of a uint64 (matches sharding_hash_u64
    in cxl_sharding.h)."""
    h = 0xCBF29CE484222325
    for i in range(8):
        h ^= (key >> (i * 8)) & 0xFF
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def hash_str_runtime(s: str) -> int:
    """FNV-1a over the byte sequence of a string. Matches
    tests/protocol_a_ycsb.cc::hash_str()."""
    h = 0xCBF29CE484222325
    for c in s.encode():
        h ^= c
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    if h == 0:
        h = 1
    return h


def runtime_key_for_int(int_k: int) -> int:
    """The trace file emits 'userN' lines; runtime converts that string
    to the uint64 key via hash_str(). This helper reproduces the full
    pipeline so the trace generator partitions by the SAME uint64 key
    the runtime will see."""
    return hash_str_runtime(f"user{int_k}")


def owner_host(int_k: int, num_hosts: int = 2) -> int:
    """Owner host of `int_k` (the integer used to form 'userN').
    Reproduces runtime pipeline:
        userN --hash_str--> uint64 K --sharding_hash_u64--> H
                                                (H >> 63) & 1 = owner
    """
    assert num_hosts in (1, 2, 4)
    shift = 64 - (num_hosts - 1).bit_length()  # num_hosts=2 → 63
    mask = num_hosts - 1
    K = runtime_key_for_int(int_k)
    return (fnv1a_u64(K) >> shift) & mask


def zipf_indices(n: int, count: int, theta: float, rng: random.Random) -> list[int]:
    """Sample `count` indices in [0, n) following Zipf(theta).

    Uses inverse CDF of generalized harmonic. For theta=0.99 + n=1M this
    is slow if done naïvely; use approximate method (Boltzmann shuffle)
    or use built-in. For trace generation (one-shot), use rng.choices
    with normalized weights — n=1M → 1M weights, manageable in memory.
    """
    # Generate Zipf weights once, then sample
    weights = [1.0 / (rank ** theta) for rank in range(1, n + 1)]
    return rng.choices(range(n), weights=weights, k=count)


def partition_keys(num_load_keys: int, num_hosts: int) -> dict[int, list[int]]:
    """Partition load key range [0, num_load_keys) by owner_host."""
    parts: dict[int, list[int]] = {h: [] for h in range(num_hosts)}
    for k in range(num_load_keys):
        h = owner_host(k, num_hosts)
        parts[h].append(k)
    return parts


def write_load_file(path: str, keys: list[int]) -> None:
    with open(path, "w") as f:
        for k in keys:
            f.write(f"INSERT usertable user{k}\n")


def write_trans_file(path: str, keys: list[int], op: str) -> None:
    with open(path, "w") as f:
        for k in keys:
            f.write(f"{op} usertable user{k}\n")


def gen_trans_keys(
    key_pool: list[int], num_ops: int, key_dist: str, seed: int
) -> list[int]:
    rng = random.Random(seed)
    n = len(key_pool)
    if key_dist == "uniform":
        return [key_pool[rng.randrange(n)] for _ in range(num_ops)]
    elif key_dist == "zipf":
        if n > 200000:
            # Zipf weight generation slow for huge n. Cap key pool to top 200k.
            sample_pool = rng.sample(key_pool, 200000)
        else:
            sample_pool = key_pool
        idxs = zipf_indices(len(sample_pool), num_ops, theta=0.99, rng=rng)
        return [sample_pool[i] for i in idxs]
    else:
        raise ValueError(f"unknown key_dist: {key_dist}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out_dir")
    ap.add_argument("--num-load", type=int, default=2_000_000)
    ap.add_argument("--num-trans", type=int, default=1_000_000,
                    help="trans ops PER HOST")
    ap.add_argument("--num-hosts", type=int, default=2)
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)

    print(f"[gen] partitioning {args.num_load:,} keys across "
          f"{args.num_hosts} hosts ...", flush=True)
    parts = partition_keys(args.num_load, args.num_hosts)
    for h in range(args.num_hosts):
        print(f"[gen]   host {h}: {len(parts[h]):,} owned keys", flush=True)

    # Shared load file (same 2M unique keys). Each scenario × keyDist gets
    # its own load file (identical content; named for symmetry).
    scenarios = ["local_read", "xhost_read", "local_write", "xhost_write"]
    key_dists = ["uniform", "zipf"]

    all_keys = list(range(args.num_load))
    for sc in scenarios:
        for kd in key_dists:
            for h in range(args.num_hosts):
                tag = f"bench_{sc}_{kd}_h{h}"
                load_path = os.path.join(args.out_dir, f"{tag}.spec_load")
                trans_path = os.path.join(args.out_dir, f"{tag}.spec_trans")

                # Load: shared across all variants; INSERT all 2M keys
                # in randomized order (different shuffle per file to
                # avoid identical sequences contaminating measurement).
                rng_load = random.Random(hash((sc, kd, h, "load")) & 0xFFFFFFFF)
                load_keys = all_keys[:]
                rng_load.shuffle(load_keys)
                write_load_file(load_path, load_keys)

                # Trans: filter pool by scenario, sample by keyDist.
                if sc == "local_read":
                    pool = parts[h]
                    op = "READ"
                elif sc == "xhost_read":
                    peer = (h + 1) % args.num_hosts
                    pool = parts[peer]
                    op = "READ"
                elif sc == "local_write":
                    pool = parts[h]
                    op = "UPDATE"
                elif sc == "xhost_write":
                    peer = (h + 1) % args.num_hosts
                    pool = parts[peer]
                    op = "UPDATE"

                rng_trans = random.Random(hash((sc, kd, h, "trans")) & 0xFFFFFFFF)
                trans_keys = gen_trans_keys(
                    pool, args.num_trans, kd, seed=hash((sc, kd, h)) & 0xFFFFFFFF
                )
                write_trans_file(trans_path, trans_keys, op)

                print(f"[gen] wrote {tag}: load={args.num_load:,} "
                      f"trans={args.num_trans:,} pool={len(pool):,}",
                      flush=True)

    print("[gen] done")


if __name__ == "__main__":
    sys.exit(main())
