#ifndef FUSEE_CXL_KV_OPS_A_V2_H_
#define FUSEE_CXL_KV_OPS_A_V2_H_

// Protocol A v2 (iter-4A target). Spec: docs/design_goals.md
// §Protocol A v2 (§I-XIII).
//
// Architectural changes vs iter-3A CxlKvStoreA:
//   - I1: CXL = unique authoritative; DRAM = cache only
//   - I2: sharding routes writes to owner host (no cross-host LFM)
//   - I3: same-host workers share KvCachePool via MAP_SHARED
//   - I5/I7/I8: per-slot SlotDirectory in DRAM, host-local spinlock
//   - I6: copy-on-write only (no in-place value update)
//   - I9: reader fast path = local cache + stale check; slow path =
//         register-then-fill via N:1:1:N
//   - I10: write commit = all sharer ACK + CXL durable
//   - I11: cross-host write via N:1:1:N forward (Phase 8 wires this;
//          Phase 6 only handles owner-self ops, returns -ENOTSUP for
//          cross-host until Phase 8 lands)
//
// Data layout:
//   CXL: BucketLockTable (NOT used by v2 normal path — frozen, see §VI),
//        CxlKvBucket array (slot.key + slot.value, 16 B per slot),
//        per-host SPSC rings + ack channels (Phase 8 wires; Phase 6 skips)
//   DRAM (MAP_SHARED, pre-fork): ShardingTable, SlotDirectory, KvCachePool,
//        BlockFreeList (per-host free list for CoW reuse — Phase 4)

#include "cxl_cache_pool.h"
#include "cxl_directory.h"
#include "cxl_hashtable.h"
#include "cxl_kv_blockpool_freelist.h"
#include "cxl_sharding.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fusee {

class CxlKvStoreA_v2 {
 public:
  // Wire up. CxlKvBucket array lives at `bucket_base`. Sharding/directory/
  // cache_pool are caller-owned (host primary mmap'd MAP_SHARED pre-fork).
  // freelist is host-local (DRAM, not in CXL).
  int attach(void *bucket_base, uint32_t num_buckets,
             int host_id, int num_hosts, bool init_region,
             ShardingTable *st, SlotDirectory *dir, KvCachePool *cache,
             BlockFreeList *freelist);

  int insert(uint64_t key, uint64_t value);
  int update(uint64_t key, uint64_t value);
  int remove(uint64_t key);
  int search(uint64_t key, uint64_t *out);

  uint32_t num_buckets() const { return num_buckets_; }
  int host_id() const { return host_id_; }

 private:
  // bucket_idx(key) and host_of(key) helpers.
  uint32_t bucket_idx(uint64_t key) const;
  uint32_t owner_host(uint64_t key) const;

  // Owner-self write path used by insert/update/remove. Acquires the
  // directory spinlock for (bucket_idx, slot_idx), executes the CoW
  // commit (slot publish + epoch bump + cache update), releases.
  int execute_write_local(uint64_t key, uint64_t new_value, int op_kind);

  CxlKvBucket *buckets_ = nullptr;
  uint32_t num_buckets_ = 0;
  int host_id_ = -1;
  int num_hosts_ = 1;

  ShardingTable *st_ = nullptr;
  SlotDirectory *dir_ = nullptr;
  KvCachePool *cache_ = nullptr;
  BlockFreeList *freelist_ = nullptr;
};

}  // namespace fusee

#endif  // FUSEE_CXL_KV_OPS_A_V2_H_
