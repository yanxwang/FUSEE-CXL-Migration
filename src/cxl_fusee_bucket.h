#ifndef FUSEE_CXL_FUSEE_BUCKET_H_
#define FUSEE_CXL_FUSEE_BUCKET_H_

#include <stddef.h>
#include <stdint.h>

namespace fusee {

// Protocol F bucket / slot layout. Mirrors FUSEE RACE hashing exactly (see
// /home/yanwang/g1/FUSEE/src/hashtable.h::TagRaceHashSlot / TagRacsHashBucket).
//
// Slot is 8 B pure data: 48-bit pointer + 8-bit size_class + 8-bit fingerprint.
// Bucket is 64 B = 1 cacheline = (8 B header: local_depth + prefix) + 7 slots.
// LFM lock state lives in an external SlotLockTable (cxl_fusee_slot_lock.h),
// so this header stays FUSEE-shaped — no lock fields contaminate the layout.

constexpr int kCxlFuseeSlotsPerBucket = 7;
constexpr uint64_t kCxlFuseeEmptySlot = 0;

// 8 B packed slot. Bit layout (LE, low byte first):
//   [0..5]  blk_off      48-bit CXL-base-relative byte offset into KV pair pool
//   [6]     size_class   8-bit selector for pool size class (0..3)
//   [7]     fingerprint  low 8 bits of fnv1a(key); for fast slot reject
// packed == 0 means empty slot.
struct CxlFuseeSlot {
  uint64_t packed;
};
static_assert(sizeof(CxlFuseeSlot) == 8,
              "CxlFuseeSlot must be 8 B to match FUSEE RACE slot");

// Size classes for the KV pair pool. Tiny is u64 inline KV (16 B record).
// 256/512/1024 mirror Protocol A's cxl_kv_blockpool size classes.
enum CxlFuseeSizeClass : uint8_t {
  kFuseeSizeClassTiny    = 0,  // 16 B record: key:8 + value:8 (inline u64)
  kFuseeSizeClassBlock256 = 1,
  kFuseeSizeClassBlock512 = 2,
  kFuseeSizeClassBlock1024 = 3,
  kFuseeSizeClassCount   = 4,
};

inline uint64_t cxl_fusee_slot_pack(uint64_t blk_off, uint8_t size_class,
                                    uint8_t fp) {
  return (blk_off & 0x0000FFFFFFFFFFFFULL)
       | ((uint64_t)size_class << 48)
       | ((uint64_t)fp << 56);
}

inline uint64_t cxl_fusee_slot_blk_off(uint64_t v) {
  return v & 0x0000FFFFFFFFFFFFULL;
}

inline uint8_t cxl_fusee_slot_size_class(uint64_t v) {
  return (uint8_t)((v >> 48) & 0xFF);
}

inline uint8_t cxl_fusee_slot_fp(uint64_t v) {
  return (uint8_t)((v >> 56) & 0xFF);
}

// 64 B bucket = 1 cacheline.  Field order mirrors FUSEE RaceHashBucket so
// the on-CXL layout is binary-identical:
//   { uint32 local_depth, uint32 prefix, RaceHashSlot slots[7] }
// local_depth and prefix are part of FUSEE's extendible-hashing
// infrastructure (subtable splitting); Protocol F does not yet implement
// subtable splits, but we keep the fields zeroed for layout compatibility.
struct CxlFuseeBucket {
  uint32_t     local_depth;                     // FUSEE compat (unused)
  uint32_t     prefix;                          // FUSEE compat (unused)
  CxlFuseeSlot slots[kCxlFuseeSlotsPerBucket];  // 7 * 8 = 56 B
};
static_assert(sizeof(CxlFuseeBucket) == 64,
              "CxlFuseeBucket must be exactly 1 cacheline (FUSEE-shaped)");

// FNV-1a 64-bit hash of a 64-bit key. Used as the first cuckoo bucket
// position and for the fingerprint (low 8 bits).
inline uint64_t cxl_fusee_hash(uint64_t x) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (int i = 0; i < 8; i++) {
    h ^= (x & 0xFF);
    h *= 0x100000001b3ULL;
    x >>= 8;
  }
  return h;
}

// Second cuckoo bucket position.  FUSEE RACE hashing uses two hash
// positions per key (`f_main_idx`, `s_main_idx`) so each key has up to 14
// candidate slots (7 in each of 2 buckets) instead of 7.  This drops the
// per-bucket collision pressure when the table is moderately loaded.
// Different seed + different multiplier from cxl_fusee_hash.
inline uint64_t cxl_fusee_hash2(uint64_t x) {
  uint64_t h = 0x84222325cbf29ce4ULL;  // bit-reversed seed
  for (int i = 0; i < 8; i++) {
    h ^= (x & 0xFF);
    h *= 0xc6a4a7935bd1e995ULL;        // distinct multiplier (MurmurHash64)
    x >>= 8;
  }
  return h;
}

inline uint8_t cxl_fusee_fp(uint64_t key) {
  return (uint8_t)(cxl_fusee_hash(key) & 0xFF);
}

} // namespace fusee

#endif // FUSEE_CXL_FUSEE_BUCKET_H_
