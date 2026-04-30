#ifndef FUSEE_CXL_SHARDING_H_
#define FUSEE_CXL_SHARDING_H_

// Protocol A v2 — key-to-host sharding table.
//
// Spec: docs/design_goals.md §I2 (sharding rule), §III (DRAM layout),
// §XII O1 (default hash function), AP10 (no LFM for writer-writer).
//
// Sharding rule: every key K maps to a unique owner_host(K). The owner
// host is the only one that performs writes on K's slot/value bytes;
// requests from non-owner hosts are forwarded via N:1:1:N (see I11).
//
// Hash function: high-bit FNV-1a (top bit XOR low bit family) avoids
// the bucket-index correlation that low-bit modulo introduces — keys
// that share a bucket index can still split evenly across owners.

#include <cstdint>

namespace fusee {

constexpr int kMaxShardHosts = 4;   // matches kMaxPhysicalHosts in iter-3A

struct ShardingTable {
  uint32_t num_hosts;     // 1, 2, or 4 (must be power of 2 for the mask)
  uint32_t mask;          // num_hosts - 1
  uint32_t shift;         // 64 - log2(num_hosts)  (use top log2(H) bits)
  // remainder is reserved for future per-host weight or extension fields
  uint64_t _pad[5];
};
static_assert(sizeof(ShardingTable) <= 64,
              "ShardingTable must fit in one cacheline");

// FNV-1a 64-bit. Same hash family as cxl_kv_ops_A.cc bucket_idx().
inline uint64_t sharding_hash_u64(uint64_t key) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (int i = 0; i < 8; i++) {
    h ^= (key >> (i * 8)) & 0xff;
    h *= 0x100000001b3ULL;
  }
  return h;
}

// Initialize the sharding table from `num_hosts`. num_hosts must be a
// power of two in [1, kMaxShardHosts]. Returns -1 on bad input.
int sharding_init(ShardingTable *st, uint32_t num_hosts);

// Look up owner host for a key. Returns 0..num_hosts-1.
inline uint32_t host_of(const ShardingTable *st, uint64_t key) {
  // (fnv1a(key) >> shift) & mask  — high-bit slice, default per spec O1.
  return (uint32_t)((sharding_hash_u64(key) >> st->shift) & st->mask);
}

}  // namespace fusee

#endif  // FUSEE_CXL_SHARDING_H_
