#ifndef FUSEE_CXL_FUSEE_INDEX_CACHE_H_
#define FUSEE_CXL_FUSEE_INDEX_CACHE_H_

// DRAM-resident per-key index cache for Protocol F.  Mirrors FUSEE §4.6
// Adaptive Index Cache.
//
// For each cached key we keep:
//   - slot_addr   : CXL absolute address of the slot last observed to hold this key
//   - kvpair_addr : CXL absolute address (blk_off) of the KV pair pointed to by
//                   that slot at the time of the last successful read
//   - access_cnt  : total times this entry has been touched on a SEARCH
//   - invalid_cnt : times the cache hit's kvpair_addr did NOT match the
//                   current slot.packed.blk_off (i.e. UPDATE happened since
//                   the cache was populated)
//   - bypass      : adaptive bypass flag, set when invalid_cnt/access_cnt
//                   exceeds the configured threshold
//
// Entries are evicted using bounded-capacity LRU.  The cache is per-process
// DRAM; it is not shared across hosts, not on CXL.
//
// See docs/protocol_F_design_and_plan.md §2.3 + §2.5.1.

#include <cstdint>
#include <list>
#include <mutex>
#include <unordered_map>

namespace fusee {

struct CxlFuseeIndexCacheEntry {
  uint64_t slot_addr   = 0;
  uint64_t kvpair_addr = 0;
  uint64_t access_cnt  = 0;
  uint64_t invalid_cnt = 0;
  bool     bypass      = false;
};

class CxlFuseeIndexCache {
 public:
  // capacity: max number of entries before LRU eviction.
  // bypass_threshold: invalid_cnt/access_cnt > threshold triggers bypass.
  //                   Default 0.5 per docs/protocol_F_design_and_plan.md §2.3.
  // bypass_min_samples: don't compute the ratio until access_cnt reaches this
  //                     many samples (avoids early-trigger from tiny counters).
  explicit CxlFuseeIndexCache(uint32_t capacity = 131072,
                              double   bypass_threshold = 0.5,
                              uint32_t bypass_min_samples = 8);

  // Look up a key.
  //   - Returns false if not cached; *out_entry undefined.
  //   - Returns true otherwise; *out_entry filled with a COPY of the
  //     cache entry (caller must consult bypass).  Copy semantics keep
  //     the API thread-safe — a pointer return would dangle if a
  //     concurrent UPDATE/DELETE evicted the entry.
  // Lookup updates LRU order (moves entry to MRU) under the mutex.
  bool lookup(uint64_t key, CxlFuseeIndexCacheEntry *out_entry);

  // Insert or replace a cache entry.  If at capacity, the LRU entry is
  // evicted.  Resets access_cnt / invalid_cnt for a fresh entry; preserves
  // them on UPDATE of an existing entry's addrs (so the bypass state
  // adapts continuously without being cleared by every write).
  void insert(uint64_t key, uint64_t slot_addr, uint64_t kvpair_addr);

  // Record a successful cache hit (cache's kvpair_addr matched current
  // slot.packed.blk_off).  Increments access_cnt only.
  void record_hit(uint64_t key);

  // Record a cache stale event (cache's kvpair_addr did not match current
  // slot.packed.blk_off).  Increments both access_cnt and invalid_cnt;
  // re-evaluates the bypass flag against the threshold.
  void record_miss(uint64_t key);

  // Evict a specific key (e.g., on DELETE).  No-op if absent.
  void evict(uint64_t key);

  // Diagnostics.
  uint32_t size() const {
    uint32_t n = 0;
    for (const auto &s : shards_) {
      std::lock_guard<std::mutex> g(s.mu);
      n += static_cast<uint32_t>(s.map.size());
    }
    return n;
  }
  uint32_t capacity() const { return capacity_; }
  uint64_t total_hits()    const {
    uint64_t n = 0;
    for (const auto &s : shards_) { std::lock_guard<std::mutex> g(s.mu); n += s.total_hits; }
    return n;
  }
  uint64_t total_misses()  const {
    uint64_t n = 0;
    for (const auto &s : shards_) { std::lock_guard<std::mutex> g(s.mu); n += s.total_misses; }
    return n;
  }
  uint64_t total_bypass_active() const;

 private:
  struct Slot {
    uint64_t                  key;
    CxlFuseeIndexCacheEntry   entry;
  };
  using ListIt = std::list<Slot>::iterator;

  void touch(ListIt it);
  void recompute_bypass(CxlFuseeIndexCacheEntry &e);

  uint32_t  capacity_;
  double    bypass_threshold_;
  uint32_t  bypass_min_samples_;

  // Sharded by key hash.  N shards × (mutex + LRU + map + counters)
  // → ~N× higher concurrent cache throughput before contention.
  // Per shard capacity = capacity_ / kNumShards.
  static constexpr uint32_t kNumShards = 64;
  struct alignas(64) Shard {
    mutable std::mutex                    mu;
    std::list<Slot>                       lru;
    std::unordered_map<uint64_t, ListIt>  map;
    uint64_t                              total_hits   = 0;
    uint64_t                              total_misses = 0;
  };
  Shard shards_[kNumShards];

  uint32_t shard_idx(uint64_t key) const {
    // Mix high bits so consecutive keys spread across shards.
    uint64_t h = key * 0x9e3779b97f4a7c15ULL;
    return static_cast<uint32_t>((h >> 32) & (kNumShards - 1));
  }
};

}  // namespace fusee

#endif  // FUSEE_CXL_FUSEE_INDEX_CACHE_H_
