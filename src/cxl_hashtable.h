#ifndef FUSEE_CXL_HASHTABLE_H_
#define FUSEE_CXL_HASHTABLE_H_

#include <stddef.h>
#include <stdint.h>

namespace fusee {

// Minimal RACE-style bucket for Phase 3. Fixed-slot, fixed-KV (u64 key, u64
// value) inline. Designed to be extended (or replaced) in later phases as we
// move toward the full FUSEE layout (variable-length K/V, pointer-to-KV,
// fingerprint, directory splits). Today's priority is to get all three
// protocols ported against a stable bucket API and benchmarkable.

constexpr int kCxlKvSlotsPerBucket = 7;

// KV slot, 16 B. Two layouts coexist (selected by size_class):
//
//   size_class == kSizeClassInline: { key:8, value:8 } — legacy/test-only
//     inline u64 value. Used by Protocol B/C and the archived iter-3A A.
//
//   size_class >= kSizeClassBlock256: { key:8, blk_off:6 (low),
//     size_class:1, fp:1 } — value lives in a CxlKvBlockPool block at
//     `blk_off`. blk_off is a CXL-base-relative byte offset
//     (6 bytes = 256 TB max addressable, sufficient for 512 GiB devdax).
//     size_class encodes 0/1/2 = 256/512/1024 B blocks. fp = key
//     fingerprint (low 8 bits of fnv1a) for fast scan rejection.
//
// Writers always publish whole slot atomically via 16-B aligned store
// after the value bytes are durable on CXL (CoW pattern, see §I6 / I10).
// Readers do `key == kEmptyKey` test BEFORE decoding value field; if
// non-empty and size_class >= 1, fetch value via blockpool.read().
struct CxlKvSlot {
  uint64_t key;
  // Encoded value field. Use the helpers below to interpret.
  uint64_t value;
};

constexpr uint64_t kEmptyKey = 0;

// size_class encoding for the high byte of slot.value (block layout).
// Inline layout has size_class=0 in this byte, but the slot is identified
// as inline by the caller using a separate context flag (Protocol B/C
// always uses inline; Protocol A always uses block layout post-iter-4A).
enum CxlSlotSizeClass : uint8_t {
  kSizeClassInline   = 0,   // legacy: low 64 bits is u64 value
  kSizeClassBlock256 = 1,   // 256 B block in pool[0]
  kSizeClassBlock512 = 2,   // 512 B block in pool[1]
  kSizeClassBlock1024 = 3,  // 1024 B block in pool[2]
};

// Block-layout encode/decode for slot.value when Protocol A is active.
// Layout (LE bytes from low to high):
//   [0..5]  blk_off  (48-bit, CXL-base-relative byte offset)
//   [6]     size_class
//   [7]     fingerprint (low 8 bits of fnv1a(key))
inline uint64_t cxl_slot_pack(uint64_t blk_off, uint8_t size_class,
                              uint8_t fp) {
  return (blk_off & 0x0000FFFFFFFFFFFFULL)
       | ((uint64_t)size_class << 48)
       | ((uint64_t)fp << 56);
}

inline uint64_t cxl_slot_blk_off(uint64_t v) {
  return v & 0x0000FFFFFFFFFFFFULL;
}

inline uint8_t cxl_slot_size_class(uint64_t v) {
  return (uint8_t)((v >> 48) & 0xFF);
}

inline uint8_t cxl_slot_fp(uint64_t v) {
  return (uint8_t)((v >> 56) & 0xFF);
}

// Bucket laid out to 128 B = 2 cachelines. Keeps bucket aligned so writes
// do not straddle unpredictable cacheline boundaries.
struct CxlKvBucket {
  CxlKvSlot slots[kCxlKvSlotsPerBucket]; // 7 * 16 = 112 B
  uint64_t  pad[2];                      // 16 B pad -> 128 B total
};
static_assert(sizeof(CxlKvBucket) == 128,
              "CxlKvBucket must be exactly 2 cachelines");

// FNV-1a 64-bit hash of a 64-bit key. Used for bucket selection.
inline uint64_t fnv1a_u64(uint64_t x) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (int i = 0; i < 8; i++) {
    h ^= (x & 0xFF);
    h *= 0x100000001b3ULL;
    x >>= 8;
  }
  return h;
}

} // namespace fusee

#endif // FUSEE_CXL_HASHTABLE_H_
