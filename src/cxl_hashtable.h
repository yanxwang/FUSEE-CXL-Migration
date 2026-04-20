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

// Inline KV slot. key == kEmptyKey means the slot is free. Writers must
// publish value before key (so a reader racing between the two stores
// observes either the old key or the new complete pair, never a new key
// with an old value — the seqlock retry on epoch change covers the rest).
struct CxlKvSlot {
  uint64_t key;
  uint64_t value;
};

constexpr uint64_t kEmptyKey = 0;

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
