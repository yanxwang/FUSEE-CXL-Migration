#!/usr/bin/env python3
"""Generate single-key flood trace for Exp 1.

Workflow:
  1. Hash user names with FNV-1a (matches tests/protocol_a_ycsb.cc::hash_str)
  2. Apply sharding: host = (sharding_hash(key) >> 63) & 1, where
     sharding_hash also FNV-1a but on uint64.
  3. Find first user names mapping to host 0 and host 1.
  4. Generate single-key flood traces:
     - bench_single_key_flood_h0.spec_load = INSERT both keys (so they exist)
     - bench_single_key_flood_h0.spec_trans = 5M lines of UPDATE on PEER's key
       (i.e., the key owned by host 1) — h0 forwards to h1
     - bench_single_key_flood_h1.spec_trans = 5M lines of UPDATE on h0's key
       — h1 forwards to h0
  5. Both hosts simultaneously forward to peer's hot key → max lock contention
"""
import os, sys

FNV_PRIME = 0x100000001b3
FNV_OFFSET = 0xcbf29ce484222325

def hash_str(s):
    h = FNV_OFFSET
    for c in s.encode():
        h ^= c
        h = (h * FNV_PRIME) & 0xffffffffffffffff
    if h == 0:
        h = 1
    return h

def sharding_hash_u64(key):
    h = FNV_OFFSET
    for i in range(8):
        h ^= (key >> (i * 8)) & 0xff
        h = (h * FNV_PRIME) & 0xffffffffffffffff
    return h

def host_of(key, num_hosts=2):
    # shift = 64 - log2(num_hosts) = 63 for num_hosts=2
    # mask = num_hosts - 1 = 1
    shift = 64 - num_hosts.bit_length() + 1
    mask = num_hosts - 1
    return (sharding_hash_u64(key) >> shift) & mask

# Find a key for each host
key_h0 = None
key_h1 = None
for i in range(1000):
    name = f"user{i}"
    k = hash_str(name)
    h = host_of(k)
    if key_h0 is None and h == 0:
        key_h0 = name
    if key_h1 is None and h == 1:
        key_h1 = name
    if key_h0 and key_h1:
        break

print(f"Hot key h0-owned: {key_h0}  (hash={hash_str(key_h0):#x}, host={host_of(hash_str(key_h0))})")
print(f"Hot key h1-owned: {key_h1}  (hash={hash_str(key_h1):#x}, host={host_of(hash_str(key_h1))})")

# Generate traces. Each host's trans = 5M UPDATEs on PEER key (max forwarding).
N_TRANS = 5_000_000

out_dir = "/home/yanwang/FUSEE/setup/iter17A_single_key_flood"
os.makedirs(out_dir, exist_ok=True)

# spec_load (same for both, INSERT both keys)
load_lines = [f"INSERT usertable {key_h0}\n", f"INSERT usertable {key_h1}\n"]
for h in ("h0", "h1"):
    with open(os.path.join(out_dir, f"bench_single_key_flood_{h}.spec_load"), "w") as f:
        f.writelines(load_lines)

# spec_trans: h0 UPDATEs key_h1 (forwards to h1); h1 UPDATEs key_h0 (forwards to h0)
def write_trans(host, peer_key):
    line = f"UPDATE usertable {peer_key}\n"
    path = os.path.join(out_dir, f"bench_single_key_flood_{host}.spec_trans")
    with open(path, "w") as f:
        for _ in range(N_TRANS):
            f.write(line)
    print(f"wrote {path} ({N_TRANS} lines, {os.path.getsize(path)} bytes)")

write_trans("h0", key_h1)  # h0 → forwards to h1
write_trans("h1", key_h0)  # h1 → forwards to h0
print(f"\nAll traces under {out_dir}")
