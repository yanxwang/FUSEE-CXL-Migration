#include "cxl_fusee_index_cache.h"

namespace fusee {

CxlFuseeIndexCache::CxlFuseeIndexCache(uint32_t capacity,
                                      double   bypass_threshold,
                                      uint32_t bypass_min_samples)
    : capacity_(capacity),
      bypass_threshold_(bypass_threshold),
      bypass_min_samples_(bypass_min_samples) {
  // Per-shard reserve.
  for (auto &s : shards_) s.map.reserve(capacity / kNumShards + 1);
}

void CxlFuseeIndexCache::touch(ListIt it) {
  // Called under the shard mutex.  `it` is for the lookup target's shard
  // — caller has the right `lru` reference; we splice within that list.
  // We just splice in place; caller passes the right list (shard.lru).
  // (Kept for API symmetry — actually unused now; inlined into callers.)
  (void)it;
}

void CxlFuseeIndexCache::recompute_bypass(CxlFuseeIndexCacheEntry &e) {
  if (e.access_cnt < bypass_min_samples_) {
    e.bypass = false;
    return;
  }
  double ratio = static_cast<double>(e.invalid_cnt)
               / static_cast<double>(e.access_cnt);
  e.bypass = (ratio > bypass_threshold_);
}

bool CxlFuseeIndexCache::lookup(uint64_t key, CxlFuseeIndexCacheEntry *out_entry) {
  Shard &s = shards_[shard_idx(key)];
  std::lock_guard<std::mutex> g(s.mu);
  auto it = s.map.find(key);
  if (it == s.map.end()) return false;
  if (it->second != s.lru.begin()) s.lru.splice(s.lru.begin(), s.lru, it->second);
  if (out_entry) *out_entry = it->second->entry;
  return true;
}

void CxlFuseeIndexCache::insert(uint64_t key, uint64_t slot_addr,
                                uint64_t kvpair_addr) {
  Shard &s = shards_[shard_idx(key)];
  std::lock_guard<std::mutex> g(s.mu);
  auto it = s.map.find(key);
  if (it != s.map.end()) {
    auto &slot = *it->second;
    slot.entry.slot_addr   = slot_addr;
    slot.entry.kvpair_addr = kvpair_addr;
    if (it->second != s.lru.begin()) s.lru.splice(s.lru.begin(), s.lru, it->second);
    return;
  }
  const uint32_t shard_cap = capacity_ / kNumShards + 1;
  if (s.lru.size() >= shard_cap) {
    auto victim = std::prev(s.lru.end());
    s.map.erase(victim->key);
    s.lru.pop_back();
  }
  Slot ns;
  ns.key                  = key;
  ns.entry.slot_addr      = slot_addr;
  ns.entry.kvpair_addr    = kvpair_addr;
  ns.entry.access_cnt     = 0;
  ns.entry.invalid_cnt    = 0;
  ns.entry.bypass         = false;
  s.lru.push_front(std::move(ns));
  s.map[key] = s.lru.begin();
}

void CxlFuseeIndexCache::record_hit(uint64_t key) {
  Shard &s = shards_[shard_idx(key)];
  std::lock_guard<std::mutex> g(s.mu);
  auto it = s.map.find(key);
  if (it == s.map.end()) return;
  it->second->entry.access_cnt++;
  s.total_hits++;
  recompute_bypass(it->second->entry);
  if (it->second != s.lru.begin()) s.lru.splice(s.lru.begin(), s.lru, it->second);
}

void CxlFuseeIndexCache::record_miss(uint64_t key) {
  Shard &s = shards_[shard_idx(key)];
  std::lock_guard<std::mutex> g(s.mu);
  auto it = s.map.find(key);
  if (it == s.map.end()) return;
  it->second->entry.access_cnt++;
  it->second->entry.invalid_cnt++;
  s.total_misses++;
  recompute_bypass(it->second->entry);
  if (it->second != s.lru.begin()) s.lru.splice(s.lru.begin(), s.lru, it->second);
}

void CxlFuseeIndexCache::evict(uint64_t key) {
  Shard &s = shards_[shard_idx(key)];
  std::lock_guard<std::mutex> g(s.mu);
  auto it = s.map.find(key);
  if (it == s.map.end()) return;
  s.lru.erase(it->second);
  s.map.erase(it);
}

uint64_t CxlFuseeIndexCache::total_bypass_active() const {
  uint64_t n = 0;
  for (const auto &s : shards_) {
    std::lock_guard<std::mutex> g(s.mu);
    for (const auto &slot : s.lru) if (slot.entry.bypass) n++;
  }
  return n;
}

}  // namespace fusee
