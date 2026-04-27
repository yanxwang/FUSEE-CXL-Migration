#ifndef FUSEE_CXL_A_CACHE_EPOCH_ARR_H_
#define FUSEE_CXL_A_CACHE_EPOCH_ARR_H_

// iter-2A-revised — per-host shared cache_epoch array (DRAM, sysv-shm-
// equivalent via mmap MAP_SHARED|MAP_ANONYMOUS pre-fork).
//
// Replaces the per-process `std::vector<std::atomic<uint64_t>>` cache_epoch
// from the iter-1A code with one shared array per host. The receiver
// thread atomic_stores into this array; all client processes on the
// same host see the new epoch via x86 cache coherence and a plain
// atomic acquire-load. No DramInvalQueue fan-out needed.
//
// Strict A semantics: writer's release-store + receiver's release-store
// happen-before any reader's acquire-load that observes the new value.
// On x86 TSO this is free (no fence beyond mfence between the
// CXL-bound write_epoch bump and the local atomic_store).

#include <atomic>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>

namespace fusee {

constexpr uint32_t kCacheEpochArrMaxBuckets = 65536;

struct alignas(64) CacheEpochArrHeader {
  std::atomic<uint64_t> magic;
  char _pad_magic[64 - sizeof(std::atomic<uint64_t>)];
  std::atomic<uint64_t> init_done;
  char _pad_init[64 - sizeof(std::atomic<uint64_t>)];
  uint32_t num_buckets;
  char _pad_num[64 - sizeof(uint32_t)];
};

struct CacheEpochArr {
  CacheEpochArrHeader hdr;
  std::atomic<uint64_t> epoch[kCacheEpochArrMaxBuckets];
};

constexpr uint64_t kCacheEpochArrMagic = 0x434548454e544152ULL;  // "CEHENTAR"

inline size_t cache_epoch_arr_bytes(uint32_t num_buckets) {
  if (num_buckets == 0 || num_buckets > kCacheEpochArrMaxBuckets) return 0;
  return sizeof(CacheEpochArrHeader) +
         (size_t)num_buckets * sizeof(std::atomic<uint64_t>);
}

inline int cache_epoch_arr_init(CacheEpochArr *a, uint32_t num_buckets) {
  if (!a || num_buckets == 0 || num_buckets > kCacheEpochArrMaxBuckets) return -1;
  a->hdr.magic.store(0, std::memory_order_relaxed);
  a->hdr.init_done.store(0, std::memory_order_relaxed);
  a->hdr.num_buckets = num_buckets;
  for (uint32_t b = 0; b < num_buckets; b++) {
    a->epoch[b].store(0, std::memory_order_relaxed);
  }
  std::atomic_thread_fence(std::memory_order_release);
  a->hdr.magic.store(kCacheEpochArrMagic, std::memory_order_release);
  a->hdr.init_done.store(1, std::memory_order_release);
  return 0;
}

inline int cache_epoch_arr_attach(CacheEpochArr *a) {
  if (!a) return -1;
  while (a->hdr.init_done.load(std::memory_order_acquire) == 0) {
    sched_yield();
  }
  if (a->hdr.magic.load(std::memory_order_acquire) != kCacheEpochArrMagic) return -1;
  return 0;
}

}  // namespace fusee

#endif  // FUSEE_CXL_A_CACHE_EPOCH_ARR_H_
