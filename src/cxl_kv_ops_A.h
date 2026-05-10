#ifndef FUSEE_CXL_KV_OPS_A_H_
#define FUSEE_CXL_KV_OPS_A_H_

// Protocol A (iter-4A-redo target). Spec: docs/design_goals.md
// §Protocol A (§I-XIII).
//
// Architectural pillars:
//   - I1:  CXL = unique authoritative; DRAM = cache only
//   - I2:  sharding routes writes to owner host
//   - I3:  same-host workers share KvCachePool via MAP_SHARED
//   - I5/I7/I8: per-slot SlotDirectory in DRAM, host-local spinlock
//   - I6:  copy-on-write only (CoW into per-host CxlKvBlockPool)
//   - I9:  reader fast = local cache; slow = register-then-fill
//          (OP_CACHE_REGISTER → owner directory.set_sharer + value)
//   - I10: write commit = all sharer ACK + CXL durable
//   - I11: cross-host write via N:1:1:N forward (ForwardRingMatrix)

#include "cxl_cache_pool.h"
#include "cxl_directory.h"
#include "cxl_forward_ring.h"
#include "cxl_hashtable.h"
#include "cxl_inval_ring.h"
#include "cxl_kv_blockpool.h"
#include "cxl_kv_blockpool_freelist.h"
#include "cxl_sharding.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace fusee {

class CxlKvStoreA {
 public:
  // Wire up. CxlKvBucket array lives at `bucket_base`. Sharding/directory/
  // cache_pool/freelist are caller-owned (pre-fork MAP_SHARED in DRAM
  // except the blockpool which is on CXL).
  //
  // `pool` is the per-host CxlKvBlockPool. iter-4A-redo uses one pool
  // per host (size_class fixed by pool->block_size()); each host
  // bump-allocates within its own segment. Cross-host responder reads
  // from src host's segment via blockpool->read().
  int attach(void *bucket_base, uint32_t num_buckets,
             int host_id, int num_hosts, bool init_region,
             ShardingTable *st, SlotDirectory *dir, KvCachePool *cache,
             BlockFreeList *freelist, CxlKvBlockPool *pool);

  // Phase 8: wire cross-host write forward / OP_CACHE_REGISTER via
  // ForwardRing in CXL. `fr` lives in CXL (init_region=true on host
  // 0 zeroes the matrix). Spawns one responder thread per host on
  // the primary client.
  int enable_forward(ForwardRingMatrix *fr, bool init_region,
                     bool spawn_responder);

  // iter-5A Phase 4: wire the SEPARATE invalidate channel. `ir` lives
  // in CXL (init_region=true on host 0 zeroes the matrix). Spawns one
  // cache_dispatcher thread per host on the primary client. Must be
  // called AFTER enable_forward (the writer-side broadcast in
  // execute_write_local needs both fr_ and ir_ wired).
  int enable_invalidate(InvalRingMatrix *ir, bool init_region,
                        bool spawn_dispatcher);

  // Stop the responder + dispatcher threads (call before destroying
  // CXL region).
  void stop_responder();
  void stop_dispatcher();
  // Convenience: same shape as B/C — stop all spawned threads.
  void stop() { stop_responder(); stop_dispatcher(); }

  // Public KV API (iter-9A Phase 1: variable-length value).
  //
  // value_len is bytes; must be ≤ pool->block_size(). The blockpool
  // is sized at attach time with one block_size class. Variable
  // length within that ceiling is supported.
  //
  // search() copies up to buf_len bytes into out_buf and writes the
  // actual value length to *out_len. If out_len is NULL the actual
  // length is dropped. If buf_len < value_len, only buf_len bytes
  // copied (truncating read; out_len reports the full length).
  int insert(uint64_t key, const void *value, uint32_t value_len);
  int update(uint64_t key, const void *value, uint32_t value_len);
  int remove(uint64_t key);
  int search(uint64_t key, void *out_buf, uint32_t buf_len,
             uint32_t *out_len);

  // Source-compat helpers for legacy 8-byte u64 callers
  // (protocol_a_local_test, protocol_a_rw_race_test,
  // protocol_a_invariant_check). New code SHOULD use the variable-
  // length API above; these helpers exist solely to keep iter-8A
  // tests passing without churn.
  int insert_u64(uint64_t key, uint64_t v) {
    return insert(key, &v, sizeof(v));
  }
  int update_u64(uint64_t key, uint64_t v) {
    return update(key, &v, sizeof(v));
  }
  int search_u64(uint64_t key, uint64_t *out) {
    if (!out) return -1;
    uint32_t got = 0;
    int rc = search(key, out, sizeof(uint64_t), &got);
    if (rc == 0 && got != sizeof(uint64_t)) return -1;
    return rc;
  }

  uint32_t num_buckets() const { return num_buckets_; }
  int host_id() const { return host_id_; }

 private:
  uint32_t bucket_idx(uint64_t key) const;
  uint32_t owner_host(uint64_t key) const;

  // Owner-self write path. Called by both local insert/update/remove
  // and by the responder (on behalf of a peer-host forwarder). Acquires
  // directory spinlock, broadcasts OP_INVALIDATE to non-self sharers,
  // CoW publish to CXL, updates directory, updates own cache.
  // value=nullptr + value_len=0 is the DELETE convention.
  int execute_write_local(uint64_t key, const void *value,
                          uint32_t value_len, int op_kind);

  // Cross-host helpers (Phase 7 + 8). All use ForwardRingMatrix slots.
  int forward_to_owner(uint32_t owner, uint64_t key,
                       const void *value, uint32_t value_len, int op_kind);
  int forward_cache_register(uint32_t owner, uint64_t key,
                             void *out_buf, uint32_t buf_len, uint32_t *out_len);

  // iter-5A: invalidate goes on its own channel (InvalRing) rather
  // than ForwardRing, to break the responder-context circular wait.
  int send_invalidate(uint32_t target_host, uint64_t key);

  // Responder dispatch (one entry per cycle).
  void responder_handle(ForwardEntry *e);

  CxlKvBucket *buckets_ = nullptr;
  uint32_t num_buckets_ = 0;
  int host_id_ = -1;
  int num_hosts_ = 1;

  ShardingTable *st_ = nullptr;
  SlotDirectory *dir_ = nullptr;
  KvCachePool *cache_ = nullptr;
  BlockFreeList *freelist_ = nullptr;
  CxlKvBlockPool *pool_ = nullptr;

  // Phase 8: cross-host forward.
  ForwardRingMatrix *fr_ = nullptr;
  std::thread responder_;
  std::atomic<bool> responder_stop_{false};
  std::atomic<uint64_t> req_op_counter_{0};

  // iter-5A Phase 4: cache invalidate channel + dispatcher.
  InvalRingMatrix *ir_ = nullptr;
  std::thread cache_dispatcher_;
  std::atomic<bool> dispatcher_stop_{false};
  std::atomic<uint64_t> inval_op_counter_{0};

  void responder_loop();
  void cache_dispatcher_loop();
};

// op_kind values used on ForwardEntry::op_kind. UPDATE/INSERT/DELETE
// flow through the responder; CACHE_REGISTER also flows through the
// responder (request side). INVALIDATE has its own channel
// (InvalRing) per iter-5A Phase 4 — it is NOT carried on ForwardEntry
// any more.
constexpr uint8_t kOpKindUpdate        = 0;
constexpr uint8_t kOpKindInsert        = 1;
constexpr uint8_t kOpKindDelete        = 2;
constexpr uint8_t kOpKindCacheRegister = 4;  // sharer -> owner; resp value = u64

}  // namespace fusee

#endif  // FUSEE_CXL_KV_OPS_A_H_
