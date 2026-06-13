#ifndef FUSEE_CXL_KV_OPS_F_H_
#define FUSEE_CXL_KV_OPS_F_H_

// Protocol F: FUSEE-on-CXL baseline KV store.
//
// Mirrors FUSEE's RACE-hashing data layout and read/write protocols as
// closely as the CXL 2.0 fabric permits.  See
// docs/protocol_F_design_and_plan.md for the full design rationale and the
// FUSEE↔Protocol F structural correspondence.
//
// Stage 3.1 scope (this iteration): attach, single-replica INSERT/SEARCH,
// slow-path SEARCH only (no DRAM-cache fast path yet), inline u64 KV (tiny
// size class).  No LFM lock yet — concurrent writes from multiple processes
// will race.  Stages 3.2–4 layer fast path, update/remove, and LFM lock on
// top.
//
// Layout on CXL:
//   [Header                          64 B]
//   [SlotLockTable region            reserved; populated in Stage 4]
//   [Bucket array                    num_buckets * 128 B]
//   [KV pair pool                    via CxlFuseeKvPairPool]

#include "cxl_fusee_bucket.h"
#include "cxl_fusee_index_cache.h"
#include "cxl_fusee_kvpair_pool.h"
#include "cxl_fusee_slot_lock.h"

#include <cstddef>
#include <cstdint>

namespace fusee {

class CxlKvStoreF {
 public:
  // Total CXL bytes required for `num_buckets` buckets + a pool of
  // `total_records` records (`record_size` bytes each) partitioned
  // across `num_hosts`.  `record_size` defaults to tiny (16 B) for
  // back-compat with existing tests; benchmark binaries pass 1024 for
  // paper §6.3 alignment.
  static std::size_t bytes_for(uint32_t num_buckets,
                               uint64_t total_records,
                               int      num_hosts,
                               uint32_t record_size = kFuseePoolTinyRecordSize);

  // API-parity overload with A/B/C.  Uses a default pool size
  // (kFuseeFDefaultPoolMultiplier * num_buckets records) and assumes
  // num_hosts=2 (FUSEE-CXL deployment default).  Tune via env var
  // FUSEE_F_TOTAL_RECORDS / FUSEE_F_NUM_HOSTS if a different sizing is
  // needed before the runner constructs the store.
  static std::size_t bytes_for(uint32_t num_buckets);

  // Attach to a CXL region.  Exactly one host across the cluster must call
  // with `init_region == true`.  All hosts must pass identical
  // `record_size`.  Defaults to tiny (16 B) for back-compat.
  int attach(void    *region_base,
             std::size_t region_bytes,
             uint32_t num_buckets,
             uint64_t total_records,
             int      host_id,
             int      num_hosts,
             bool     init_region,
             uint32_t record_size = kFuseePoolTinyRecordSize);

  // API-parity overload: reads FUSEE_F_TOTAL_RECORDS / FUSEE_F_NUM_HOSTS
  // env vars when present, otherwise uses the defaults the bytes_for
  // overload assumed.  `read_only` accepted for API symmetry with C
  // (which skips its replicator on read_only) but ignored — F has no
  // replicator thread.
  int attach(void    *region_base,
             std::size_t region_bytes,
             uint32_t num_buckets,
             int      host_id,
             int      num_hosts,
             bool     init_region,
             bool     read_only = false);

  // KV interface — u64 key + u64 value (tiny-class; used by unit tests
  // + iter-1A through iter-18A microbenches).  Requires record_size ≥ 16.
  int insert(uint64_t key, uint64_t value);  // 0 ok, -1 full
  int update(uint64_t key, uint64_t value);  // 0 ok, -1 not found
  int remove(uint64_t key);                  // 0 ok, -1 not found
  int search(uint64_t key, uint64_t *out) const;  // 0 ok, -1 not found

  // Byte-blob KV interface — u64 key + arbitrary `value_len`-byte value.
  // Used by Fig 10/11/13 binaries for paper §6.3 1024 B KV alignment.
  // `value_len` must satisfy `8 + value_len <= record_size`.  search_blob
  // fills up to `*io_value_len` bytes and updates `*io_value_len` with
  // the actual stored length.
  int insert_blob(uint64_t key, const void *value, uint32_t value_len);
  int update_blob(uint64_t key, const void *value, uint32_t value_len);
  int search_blob(uint64_t key, void *value_out, uint32_t *io_value_len) const;

  // API parity with Protocols A/B/C.  F has no replicator, but the pool's
  // background reclaim thread (FUSEE §4.4) needs to be joined here so the
  // runner exit is clean.
  void     stop()              { pool_.stop_reclaim_thread(); }
  uint64_t replicated_ops() const { return 0; }

  // API parity with B/C cache toggling.  F's index cache is always on by
  // design (with FUSEE §4.6 adaptive bypass for write-heavy keys); the
  // toggle is accepted for runner compatibility but is a no-op.
  void enable_dram_cache(bool /*on*/) {}

  // API parity with B's same-host bypass (B uses an in-DRAM message
  // matrix to short-circuit cross-process invalidation when senders and
  // receivers are on the same host).  F has no such concept — all writes
  // go through the per-slot LFM lock + slot publish path regardless of
  // peer locality.  No-op for runner compatibility.
  template <typename Matrix>
  void enable_same_host_bypass(Matrix * /*mat*/, int /*num_clients_per_host*/) {}

  // Per-thread LFM identity (set by each worker before its first KV op).
  // The LFM lock_slot path uses these instead of (host_id, num_hosts):
  // multi-thread benches must give every worker a UNIQUE LFM id (else
  // threads sharing id=0 race on b[0] → silent data corruption).
  //
  // Default (no override) — uses (host_id, num_hosts).  Suitable only
  // for single-thread-per-host runs (Fig 10 microbench, hashdiff,
  // unit tests).
  void set_total_workers(int n) { total_workers_ = n; }
  static void set_worker_id(int id) { t_worker_id_ = id; }
  static int  worker_id()           { return t_worker_id_; }

  // Diagnostics.
  uint32_t num_buckets()      const { return num_buckets_; }
  const CxlFuseeKvPairPool &pool() const { return pool_; }
  const CxlFuseeIndexCache &cache() const { return index_cache_; }
  uint64_t cache_capacity()   const { return index_cache_.capacity(); }

 private:
  // FUSEE cuckoo hashing: each key has two candidate bucket positions.
  uint32_t bucket_idx(uint64_t key) const {
    return static_cast<uint32_t>(cxl_fusee_hash(key) % num_buckets_);
  }
  uint32_t bucket_idx2(uint64_t key) const {
    return static_cast<uint32_t>(cxl_fusee_hash2(key) % num_buckets_);
  }

  // Address of a slot inside the bucket array, expressed as an absolute
  // byte offset from `region_base_`.  Used for cache `slot_addr` field.
  uint64_t slot_abs_off(uint32_t bucket, int slot) const;

  uint8_t              *region_base_     = nullptr;
  std::size_t           region_bytes_    = 0;
  CxlFuseeBucket       *buckets_         = nullptr;
  std::size_t           buckets_off_     = 0;
  uint32_t              num_buckets_     = 0;
  int                   host_id_         = -1;
  int                   num_hosts_       = 0;
  int                   total_workers_   = 0;   // 0 → use (host_id, num_hosts) for LFM
  CxlFuseeSlotLockTable lock_table_;
  CxlFuseeKvPairPool    pool_;
  mutable CxlFuseeIndexCache index_cache_;

  // Per-thread LFM identity.  Worker threads call set_worker_id() before
  // their first KV op so multi-thread benches give every worker a
  // UNIQUE LFM id (LFM b[id] arrays are not write-write safe with
  // duplicate ids).  -1 → fall back to host_id_.
  static thread_local int t_worker_id_;
};

}  // namespace fusee

#endif  // FUSEE_CXL_KV_OPS_F_H_
