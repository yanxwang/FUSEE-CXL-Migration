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
//   - I11: cross-host write via N:1:1:N forward (iter-9A Phase 2:
//          WriteRingMatrix + ReadRingMatrix + InvalRingMatrix +
//          ForwardStagingMatrix; control on rings, value bytes on
//          staging arena per C2)

#include "cxl_cache_pool.h"
#include "cxl_directory.h"
#include "cxl_forward_staging.h"
#include "cxl_read_staging.h"
#include "cxl_hashtable.h"
#include "cxl_inval_ring.h"
#include "cxl_kv_blockpool.h"
#include "cxl_kv_blockpool_freelist.h"
#include "cxl_op_aggregator.h"
#include "cxl_read_ring.h"
#include "cxl_sharding.h"
#include "cxl_tls_cache.h"
#include "cxl_write_ring.h"

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

  // iter-9A Phase 2.A: wire the WriteRing channel for op 1/2/3
  // (UPDATE/INSERT/DELETE). `wr` and `fs` live in CXL; `wr` carries
  // control-only WriteEntry messages and `fs` is the per-host value-
  // bytes staging arena. init_region=true on host 0 zeroes the
  // matrices. Spawns one named WriteReceiver thread (cpu 65) per host
  // on the primary client.
  int enable_write_ring(WriteRingMatrix *wr, ForwardStagingMatrix *fs,
                        bool init_region, bool spawn_receiver);

  // iter-9A Phase 2.A: wire the ReadRing channel for op 4
  // (CACHE_REGISTER, register-then-fill). `rr` lives in CXL. Response
  // carries (status, owner_blk_off, value_len) and the reader pulls
  // value bytes directly via pool->read — no value bytes on the ring.
  // Spawns one named ReadReceiver thread (cpu 67) per host on the
  // primary client.
  // iter-11A Phase 1: enable_read_ring takes a ReadStagingMatrix (rs)
  // for forwarder-pool-direct value-bytes deposit. Owner forwarder
  // writes value bytes directly to rs[req_host][me][slot_idx] +
  // bumps ready_epoch; reader polls staging, no second pool->read.
  int enable_read_ring(ReadRingMatrix *rr, ReadStagingMatrix *rs,
                       bool init_region, bool spawn_receiver);

  // iter-5A Phase 4: wire the SEPARATE invalidate channel. `ir` lives
  // in CXL (init_region=true on host 0 zeroes the matrix). Spawns one
  // cache_dispatcher thread (now named "InvalReceiver", cpu 69) per
  // host on the primary client. Must be called AFTER both
  // enable_write_ring and enable_read_ring (writer broadcast in
  // execute_write_local needs them all wired).
  int enable_invalidate(InvalRingMatrix *ir, bool init_region,
                        bool spawn_dispatcher);

  // iter-9A Phase 2.C — wire the per-worker DRAM aggregator + 3 named
  // CPU-pinned sender threads. `ar` lives in DRAM (MAP_SHARED|
  // MAP_ANONYMOUS pre-fork); senders are spawned on the primary
  // client only. After this returns, workers should call
  // set_worker_id(client_id) on each forked process so they hash to
  // their slot[*][worker_id]. WriteSender pinned cpu 64, ReadSender
  // 66, InvalSender 68.
  int enable_senders(AggregatorRegion *ar, int num_workers,
                     bool spawn_senders);

  // Per-worker thread-local register (post-fork in each child).
  // Workers without this set fall back to the DIRECT cross-host path,
  // i.e. the worker itself does fetch_add on the CXL ring.
  static void set_worker_id(int wid);

  // iter-10A Phase 1.C: per-worker TlsCache attach. Worker calls this
  // post-fork (after tls_cache_init). search() / execute_write_local
  // route through TLS L1 if set; otherwise skip and go straight to
  // shared cache_pool L2.
  static void set_thread_tls_cache(TlsCache *tls);

  void stop_write_sender();
  void stop_read_sender();
  void stop_inval_sender();

  // iter-9A Phase 2.G — C4 startup assert (replaces the obsolete
  // phys_hosts_pr_ ≥ 2 check). In multi-host mode (num_hosts_ ≥ 2)
  // ALL of {wr_, rr_, fs_, ir_} must be non-null before any
  // cross-host operation can proceed. Call this AFTER all enable_*
  // calls and BEFORE the first user op. Aborts on missing wiring;
  // returns 0 on success.
  int assert_n_to_n_active();

  // Stop the receiver threads (call before destroying CXL region).
  // iter-9A: 3 named system threads instead of the iter-5A pair.
  void stop_write_receiver();
  void stop_read_receiver();
  void stop_inval_receiver();
  // Convenience: same shape as B/C — stop all spawned threads.
  // Senders BEFORE receivers so any in-flight forwards complete.
  void stop() {
    stop_write_sender();
    stop_read_sender();
    stop_inval_sender();
    stop_write_receiver();
    stop_read_receiver();
    stop_inval_receiver();
  }
  // Legacy aliases — iter-9A renamed responder→write_receiver,
  // dispatcher→inval_receiver. Kept here so test code compiled
  // against pre-Phase-2 names (protocol_a_ycsb's stop sequence)
  // still links.
  void stop_responder()  { stop_write_receiver(); }
  void stop_dispatcher() { stop_inval_receiver(); }

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

  // Cross-host helpers — iter-9A Phase 2.A 3-ring split.
  // Public dispatchers: route through aggregator if enabled, else
  // call the direct CXL forward path.
  int forward_write(uint32_t owner, uint64_t key,
                    const void *value, uint32_t value_len, int op_kind);
  int forward_read(uint32_t owner, uint64_t key,
                   void *out_buf, uint32_t buf_len, uint32_t *out_len);
  int send_invalidate(uint32_t target_host, uint64_t key);

  // Direct CXL paths — used by senders and (when aggregator is
  // disabled) by workers themselves. Receivers calling
  // execute_write_local also call send_invalidate_direct (NEVER the
  // aggregator path, since receivers are not workers).
  int forward_write_direct(uint32_t owner, uint64_t key,
                           const void *value, uint32_t value_len,
                           int op_kind);
  int forward_read_direct(uint32_t owner, uint64_t key,
                          void *out_buf, uint32_t buf_len,
                          uint32_t *out_len);
  int send_invalidate_direct(uint32_t target_host, uint64_t key);

  // Receiver dispatch — one handler per ring (iter-9A Phase 2.D).
  // src is the originating host id (decoded from req_op_id high bits).
  void write_handler(WriteEntry *e, int src);
  // iter-11A Phase 1: slot_idx required for ReadStaging direct-deposit.
  void read_handler(ReadEntry *e, int src, uint32_t slot_idx);

  CxlKvBucket *buckets_ = nullptr;
  uint32_t num_buckets_ = 0;
  int host_id_ = -1;
  int num_hosts_ = 1;

  ShardingTable *st_ = nullptr;
  SlotDirectory *dir_ = nullptr;
  KvCachePool *cache_ = nullptr;
  BlockFreeList *freelist_ = nullptr;
  CxlKvBlockPool *pool_ = nullptr;

  // iter-9A Phase 2.A: 3-ring split + ForwardStaging arena.
  // iter-11A Phase 1: ReadStagingMatrix for forwarder-pool-direct.
  WriteRingMatrix       *wr_ = nullptr;
  ReadRingMatrix        *rr_ = nullptr;
  ForwardStagingMatrix  *fs_ = nullptr;
  ReadStagingMatrix     *rs_ = nullptr;  // iter-11A Phase 1
  InvalRingMatrix       *ir_ = nullptr;

  // iter-9A Phase 2.C: aggregator + 3 sender threads.
  AggregatorRegion *aggr_ = nullptr;
  int               num_aggr_workers_ = 0;
  std::thread       write_sender_;
  std::thread       read_sender_;
  std::thread       inval_sender_;
  std::atomic<bool> write_sender_stop_{false};
  std::atomic<bool> read_sender_stop_{false};
  std::atomic<bool> inval_sender_stop_{false};

  // iter-9A Phase 2.D-E: 3 named system threads per host.
  // iter-11A Phase 2 (455379e) added InvalDispatcher + 8 InvalWorker
  // threads but was reverted in the next commit due to w_p99 26×
  // regression. Restored to single-thread receiver.
  std::thread write_receiver_;
  std::thread read_receiver_;
  std::thread inval_receiver_;
  std::atomic<bool> write_receiver_stop_{false};
  std::atomic<bool> read_receiver_stop_{false};
  std::atomic<bool> inval_receiver_stop_{false};

  std::atomic<uint64_t> write_op_counter_{0};
  std::atomic<uint64_t> read_op_counter_{0};
  std::atomic<uint64_t> inval_op_counter_{0};

  void write_receiver_loop();
  void read_receiver_loop();
  void inval_receiver_loop();

  void write_sender_loop();
  void read_sender_loop();
  void inval_sender_loop();

  // iter-10A Phase 3: per-dst batched drain helpers. Issue one
  // fetch_add(n) on (host_id_, dst) ring tail, fill n entries +
  // staging in parallel, single sfence, then spin on n resp_op_ids
  // round-robin and flip ack as each comes back.
  int write_sender_drain_dst(int dst, int n, const int *slot_workers);
  int read_sender_drain_dst (int dst, int n, const int *slot_workers);
  int inval_sender_drain_dst(int dst, int n, const int *slot_workers);

  // Per-policy generic sender body — selects between
  // FUSEE_BATCH_POLICY=P0/P1/P2/P3 at startup.
  template <int RING_KIND>
  void sender_loop_dispatch(std::atomic<bool> *stop_flag);
};

// op_kind values for write-path messages (WriteEntry::op_kind):
//   UPDATE/INSERT/DELETE  → carried on WriteRing
//   CACHE_REGISTER (op 4) → carried on ReadRing instead (no payload)
//   INVALIDATE     (op 5) → carried on InvalRing (control-only, iter-5A)
constexpr uint8_t kOpKindUpdate        = 0;
constexpr uint8_t kOpKindInsert        = 1;
constexpr uint8_t kOpKindDelete        = 2;
constexpr uint8_t kOpKindCacheRegister = 4;  // routed via ReadRing

}  // namespace fusee

#endif  // FUSEE_CXL_KV_OPS_A_H_
