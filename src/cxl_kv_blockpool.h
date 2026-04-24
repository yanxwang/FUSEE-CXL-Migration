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

class CxlKvBlockPool {
 public:
  static std::size_t bytes_for(uint32_t num_blocks_per_host,
                               uint32_t block_size, int num_hosts);

  int attach(void *base, std::size_t bytes,
             uint32_t num_blocks_per_host, uint32_t block_size,
             int host_id, int num_hosts, bool init_region);

  // Allocate a block from this host's segment. Returns absolute offset
  // from pool base. Returns 0 on exhaustion (since 0 == "empty"
  // sentinel, an alloc result of 0 is invalid by construction — the
  // first valid block lives past the pool header).
  uint64_t alloc();

  // Write `len` bytes at absolute offset `off`. Caller must ensure off
  // came from this host's `alloc()` (cross-host alloc would race the
  // bump cursor); reads happen from any host. Flushes per cacheline +
  // sfence before returning.
  void write(uint64_t off, const void *data, uint32_t len);

  // Read `len` bytes from absolute offset `off`. Issues clflushopt on
  // each cacheline + mfence before the load to refetch from CXL on
  // peer-host writes.
  void read(uint64_t off, void *out, uint32_t len) const;

  // Lazy free is a stub — see iter4 plan §8.4. Keeps interface stable
  // for iter-5 GC.
  void free_lazy(uint64_t /*off*/) {}

  uint32_t block_size() const { return block_size_; }
  uint32_t num_blocks_per_host() const { return num_blocks_per_host_; }
  bool valid() const { return base_ != nullptr; }
  std::size_t total_bytes() const { return total_bytes_; }

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

  uint8_t *base_ = nullptr;
  std::size_t total_bytes_ = 0;
  uint32_t block_size_ = 0;
  uint32_t num_blocks_per_host_ = 0;
  int      host_id_ = -1;
  int      num_hosts_ = 0;
  HostCursor *cursors_ = nullptr;  // points into base_ + sizeof(Header)
  uint8_t  *seg_base_ = nullptr;   // points to start of host_id_'s segment
  std::size_t header_bytes_ = 0;   // bytes from base_ to first segment
};

}  // namespace fusee
#endif
