#ifndef FUSEE_CXL_FUSEE_KVPAIR_POOL_H_
#define FUSEE_CXL_FUSEE_KVPAIR_POOL_H_

// KV pair pool for Protocol F.  Mirrors FUSEE §4.4 (two-level memory
// management) with a free bitmap shared on CXL and per-host DRAM free lists.
//
// Configurable record size (set at attach time): every record in this
// pool is `record_size` bytes.  Two canonical sizes:
//   - tiny   = 16 B   (u64 key + u64 value; default, used by existing unit
//                      tests + iter-1A-...-iter-18A microbenches)
//   - Block1024 = 1024 B (8 B key + 1016 B value; paper §6.3 alignment for
//                         Fig 10/11/13 benchmarks)
// Multi-class support (Stage 6) lands later; for now one pool = one size.
//
// Layout on CXL:
//   [Header                           64 B]
//   [HostCursor[kMaxHosts]            kMaxHosts * 64 B]
//   [Free bitmap (1 bit per record)   ceil(num_records, 8) bytes, padded]
//   [Records                          num_records * record_size]
//
// Each host owns a contiguous segment of `records_per_host` records.  All
// allocations from a host bump its own cursor (no cross-host contention on
// the cursor).  Free is global: any host can set any bit; the bit-setter only
// touches one cacheline of the bitmap.  Reclaim is per-segment: a host scans
// the bits covering its segment, atomically clears set bits, and harvests
// the freed addresses into its DRAM-local free list.
//
// Safety against use-after-free: Protocol F guarantees correctness by the
// reader-side (key) verify after following slot.blk_off — see
// docs/protocol_F_design_and_plan.md §2.5.3 and §3.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace fusee {

// kFuseePoolMaxHosts is the upper bound on the `num_hosts` arg passed to
// pool.attach().  The YCSB runner passes total cluster worker count
// (physical hosts × clients-per-host), so this needs to cover the largest
// (T × N) we want to sweep.  T=32 × N=2 = 64; round up to 128.
constexpr int      kFuseePoolMaxHosts = 128;
constexpr uint32_t kFuseePoolTinyRecordSize    = 16;    // u64 key + u64 value
constexpr uint32_t kFuseePoolBlock1024RecordSize = 1024;  // 8 B key + 1016 B value (paper §6.3)
constexpr uint64_t kFuseePoolMagic = 0x46555345505f5031ULL;  // "FUSEEP_1"

// A tiny KV record stored at `blk_off` inside the pool.
struct CxlFuseeTinyRecord {
  uint64_t key;
  uint64_t value;
};
static_assert(sizeof(CxlFuseeTinyRecord) == 16);

class CxlFuseeKvPairPool {
 public:
  CxlFuseeKvPairPool() = default;
  ~CxlFuseeKvPairPool();

  // Pool owns a kernel thread + mutex; not copyable / movable.
  CxlFuseeKvPairPool(const CxlFuseeKvPairPool &) = delete;
  CxlFuseeKvPairPool &operator=(const CxlFuseeKvPairPool &) = delete;

  // Total CXL bytes required for a pool of `total_records` records of
  // `record_size` bytes each, partitioned across `num_hosts` hosts.
  // record_size defaults to tiny (16 B) for back-compat.
  static std::size_t bytes_for(uint64_t total_records, int num_hosts,
                               uint32_t record_size = kFuseePoolTinyRecordSize);

  // Attach to a CXL region. Exactly one host across the cluster must call
  // with `init_region == true`.  All hosts must pass the same record_size.
  int attach(void *base, std::size_t bytes, uint64_t total_records,
             int host_id, int num_hosts, bool init_region,
             uint32_t record_size = kFuseePoolTinyRecordSize);

  // Allocate one record from this host's segment.  Returns absolute byte
  // offset from pool base, or 0 on exhaustion.  Tries the local DRAM free
  // list first (populated by reclaim_pass), then bumps the host's cursor.
  // 0 is a valid sentinel because record_off >= records_base_off > 0.
  uint64_t alloc_tiny();   // legacy name; works for any record_size
  uint64_t alloc_block() { return alloc_tiny(); }

  uint32_t record_size() const { return record_size_; }

  // Mark the record at `off` as freed by setting its bit in the CXL-resident
  // free bitmap.  Atomic + flush so any host that does a reclaim pass on
  // this segment can pick it up.  Safe to call from any host.
  void free(uint64_t off);

  // Scan this host's segment of the free bitmap, atomically clear set bits,
  // and push the freed offsets into the local DRAM free list.  Returns the
  // number of records harvested.
  uint32_t reclaim_pass();

  // Start a background thread that calls reclaim_pass() every
  // `interval_ms` milliseconds.  Per FUSEE §4.4: keeps the critical-path
  // alloc/free off the bitmap-scan path.  Idempotent — second call is a
  // no-op while a thread is already running.
  void start_reclaim_thread(uint32_t interval_ms);

  // Stop the background thread (if any) and join.  Safe to call multiple
  // times.  Automatically called from the destructor.
  void stop_reclaim_thread();

  // Write `len` bytes (`<= record size`) at absolute offset `off`.  Flushes
  // the affected cachelines with sfence afterwards so peer hosts see the
  // bytes.  Caller is responsible for `off` having come from `alloc_tiny()`.
  void write(uint64_t off, const void *data, uint32_t len);

  // Read `len` bytes from absolute offset `off`.  flush_line + mfence each
  // affected cacheline before the load so a peer-host write is visible.
  void read(uint64_t off, void *out, uint32_t len) const;

  // Convenience accessors.
  uint64_t total_records() const { return total_records_; }
  uint64_t records_per_host() const { return records_per_host_; }
  bool     valid() const { return base_ != nullptr; }
  std::size_t total_bytes() const { return total_bytes_; }

  // Counters for diagnostics.
  uint64_t bump_exhaust_count() const {
    return bump_exhausts_.load(std::memory_order_relaxed);
  }
  uint64_t free_calls() const {
    return free_calls_.load(std::memory_order_relaxed);
  }
  uint64_t reclaimed_total() const {
    return reclaimed_total_.load(std::memory_order_relaxed);
  }

 private:
  struct alignas(64) Header {
    uint64_t magic;
    uint64_t total_records;
    uint32_t num_hosts;
    uint32_t record_size;
    uint64_t records_base_off;
    uint64_t bitmap_base_off;
    uint64_t bitmap_bytes;
    char _pad[64 - 48];
  };
  static_assert(sizeof(Header) == 64);

  struct alignas(64) HostCursor {
    std::atomic<uint64_t> bump;   // next index in this host's segment
    char _pad[64 - 8];
  };
  static_assert(sizeof(HostCursor) == 64);

  // Translate absolute offset to (host_id, index-within-segment).
  void off_to_segment(uint64_t off, int *out_host, uint64_t *out_idx) const;
  // Translate (host_id, index-within-segment) to absolute offset.
  uint64_t segment_to_off(int host_id, uint64_t idx) const;

  uint8_t        *base_           = nullptr;
  std::size_t     total_bytes_    = 0;
  uint64_t        total_records_  = 0;
  uint64_t        records_per_host_ = 0;
  uint64_t        records_base_off_ = 0;
  uint64_t        bitmap_base_off_  = 0;
  uint64_t        bitmap_bytes_     = 0;
  int             host_id_        = -1;
  int             num_hosts_      = 0;
  uint32_t        record_size_    = kFuseePoolTinyRecordSize;

  HostCursor     *cursors_        = nullptr;  // on CXL
  uint8_t        *bitmap_         = nullptr;  // on CXL, points into base_
  uint8_t        *records_        = nullptr;  // on CXL, points into base_

  // DRAM-local free list: offsets reclaimed from this host's segment by
  // reclaim_pass().  alloc_tiny() pops from here first.  Guarded by
  // free_list_mutex_ because the background reclaim thread (Stage 6) and
  // the caller-side alloc/free can race on the vector.
  std::mutex            free_list_mutex_;
  std::vector<uint64_t> local_free_list_;

  // Background reclaim thread state (FUSEE §4.4).
  std::atomic<bool>     reclaim_stop_   {false};
  std::thread           reclaim_thread_;

  std::atomic<uint64_t> bump_exhausts_   {0};
  std::atomic<uint64_t> free_calls_      {0};
  std::atomic<uint64_t> reclaimed_total_ {0};
};

}  // namespace fusee

#endif  // FUSEE_CXL_FUSEE_KVPAIR_POOL_H_
