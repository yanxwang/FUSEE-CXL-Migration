#ifndef FUSEE_CXL_KV_BLOCKPOOL_H_
#define FUSEE_CXL_KV_BLOCKPOOL_H_

// Per-host bump-allocated block pool on CXL for variable-length values.
//
// Layout (one CXL region per pool):
//   [u64 magic                         | 8 B  ]
//   [u32 block_size                    | 4 B  ]
//   [u32 num_blocks_per_host            | 4 B  ]
//   [u32 num_hosts                     | 4 B  ]
//   [pad to 64 B                              ]
//   [host0 bump_cursor (cacheline)     | 64 B ]
//   [host1 bump_cursor (cacheline)     | 64 B ]
//   ... up to MAX_POOL_HOSTS
//   [host0 segment: num_blocks_per_host * block_size]
//   [host1 segment: num_blocks_per_host * block_size]
//   ...
//
// Each host's segment is internally partitioned into:
//   [private  (~70 %)][peer_0_reserved][peer_1_reserved]...[peer_{N-2}_reserved]
//
// iter-13A Phase 2 W1: cross-host writes from peer P to owner O allocate
// from O's "peer-reserved-for-P" sub-segment via a DRAM-local bump pointer
// owned by P (no CXL atomic). Local writes still bump the CXL cursor as
// before.
//
// Each host atomically bump-allocates within its own segment; no
// cross-host contention on the cursor. Writers flush the value bytes
// (clflushopt + sfence) so a peer host that follows blk_off later sees
// the bytes. Readers issue clflushopt + mfence on the bytes before
// loading them (FUSEE's standard "CACHELINE_LOAD" pattern).
//
// blk_off (slot's blk_off field) encodes the absolute byte offset from
// pool base. blk_off == 0 means "no value" (matches the `kEmptyKey == 0`
// convention used by the hashtable; an empty slot has both fields zero).

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fusee {

constexpr int kMaxPoolHosts = 4;
constexpr uint64_t kPoolMagic = 0x5046554b56504c4dULL;  // "MLPVKUFP" reversed-ish

// iter-13A Phase 2 W1: fraction of each host's segment reserved for peer
// allocators. The remainder is the host's own private bump region.
// 30 % matches user QR3.b decision (2026-05-17).
constexpr float kPeerReserveFrac = 0.30f;

class CxlKvBlockPool {
 public:
  static std::size_t bytes_for(uint32_t num_blocks_per_host,
                               uint32_t block_size, int num_hosts);

  int attach(void *base, std::size_t bytes,
             uint32_t num_blocks_per_host, uint32_t block_size,
             int host_id, int num_hosts, bool init_region);

  // Allocate a block from this host's PRIVATE region (first kPrivateFrac
  // of own segment). Returns absolute offset from pool base. Returns 0
  // on exhaustion (since 0 == "empty" sentinel, an alloc result of 0 is
  // invalid by construction — the first valid block lives past the pool
  // header).
  uint64_t alloc();  // alias for alloc_local()
  uint64_t alloc_local();

  // iter-13A Phase 2 W1: allocate a block from owner_host's "reserved-for-me"
  // sub-segment. Uses a DRAM-local bump pointer (no CXL atomic — only
  // this host ever advances it). Returns 0 on exhaustion; caller should
  // fall back to staging path on 0.
  uint64_t alloc_peer(int owner_host);

  // Write `len` bytes at absolute offset `off`. Caller must ensure off
  // came from this host's `alloc()` or `alloc_peer(owner)` (cross-host
  // alloc into private would race the bump cursor); reads happen from
  // any host. Flushes per cacheline + sfence before returning.
  void write(uint64_t off, const void *data, uint32_t len);

  // iter-21A LR-D2 + XR-D3: split read by direction.
  //
  // read_local(): caller is on the same host that owns this segment.
  //   Same-host MOESI keeps the local L1 coherent with any prior
  //   pool->write on this host. No clflushopt + mfence needed.
  //   Call sites: search() owner-self miss + read_handler hdr_buf fetch
  //   + execute_write_local hdr probes.
  //
  // read_xhost(): caller is on a DIFFERENT host from the segment owner.
  //   CXL Type-3 on g1/g2 (XConn switch) does NOT provide cross-host
  //   CPU cache coherence — the caller's L1 may serve stale bytes from
  //   a prior access. Must clflushopt + mfence so the subsequent memcpy
  //   reads fresh bytes from CXL memory.
  //   Call sites: forward_read_direct HAZARD-mode pool fetch.
  void read_local(uint64_t off, void *out, uint32_t len) const;
  void read_xhost(uint64_t off, void *out, uint32_t len) const;

  // Legacy read() — kept as backward-compat alias for read_xhost (the
  // safe-but-slow choice). All in-repo call sites should migrate to
  // read_local / read_xhost so direction is explicit. Future cleanup
  // iter will retire read().
  void read(uint64_t off, void *out, uint32_t len) const { read_xhost(off, out, len); }

  // Lazy free is a stub — see iter4 plan §8.4. Keeps interface stable
  // for iter-5 GC.
  void free_lazy(uint64_t /*off*/) {}

  uint32_t block_size() const { return block_size_; }
  uint32_t num_blocks_per_host() const { return num_blocks_per_host_; }
  uint32_t private_blocks() const { return private_blocks_; }
  uint32_t peer_blocks_per_peer() const { return peer_blocks_per_peer_; }
  bool valid() const { return base_ != nullptr; }
  std::size_t total_bytes() const { return total_bytes_; }

  // iter-13A Phase 2 W1 instrumentation: peer-reserved exhaust counter.
  // When the reserved region for an owner is exhausted, alloc_peer
  // returns 0 and caller should fall back to staging copy. We count
  // these so the iter summary can surface "did fallback fire?" — per
  // task plan C9 (no silent fallback).
  uint64_t peer_exhaust_count(int owner_host) const {
    if (owner_host < 0 || owner_host >= kMaxPoolHosts) return 0;
    return peer_exhausts_[owner_host].load(std::memory_order_relaxed);
  }

 private:
  // Header layout (cacheline-aligned).
  struct alignas(64) Header {
    uint64_t magic;
    uint32_t block_size;
    uint32_t num_blocks_per_host;
    uint32_t num_hosts;
    uint32_t _rsv;
    char _pad[64 - 24];
  };
  static_assert(sizeof(Header) == 64, "Header must be 64 B");

  struct alignas(64) HostCursor {
    std::atomic<uint64_t> bump;  // count of blocks allocated by this host
    char _pad[64 - 8];
  };
  static_assert(sizeof(HostCursor) == 64, "HostCursor must be 64 B");

  // Map (peer_id, owner_id) → peer_index within owner's reserved layout.
  // For owner O with hosts {0, 1, 2, ..., N-1}\{O}, peers are sorted by
  // id. peer P's index = its sorted position among non-O hosts.
  // E.g., for N=2: owner 0's peer set = {1}, so 1's index in 0's
  // reserved = 0. For N=4 owner 1: peer set = {0,2,3}, so 0→0, 2→1, 3→2.
  static int peer_index_in_owner(int peer_id, int owner_id);

  uint8_t *base_ = nullptr;
  std::size_t total_bytes_ = 0;
  uint32_t block_size_ = 0;
  uint32_t num_blocks_per_host_ = 0;
  // iter-13A Phase 2 W1: private vs peer-reserved split.
  uint32_t private_blocks_ = 0;      // first private_blocks_ of each segment
  uint32_t peer_blocks_per_peer_ = 0; // (num_blocks_per_host_ - private_blocks_) / (num_hosts - 1)
  int      host_id_ = -1;
  int      num_hosts_ = 0;
  HostCursor *cursors_ = nullptr;  // CXL: private-region bump per host
  uint8_t  *seg_base_ = nullptr;   // points to start of host_id_'s segment
  std::size_t header_bytes_ = 0;   // bytes from base_ to first segment

  // iter-13A Phase 2 W1: DRAM-local peer bumps. Index = owner_id.
  // peer_bumps_[O] = my next allocation index into O's reserved-for-me region.
  // Only THIS host (host_id_) advances peer_bumps_[*]; never read by peers.
  std::atomic<uint64_t> peer_bumps_[kMaxPoolHosts];
  std::atomic<uint64_t> peer_exhausts_[kMaxPoolHosts];
};

}  // namespace fusee
#endif
