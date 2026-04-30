#ifndef FUSEE_CXL_KV_BLOCKPOOL_FREELIST_H_
#define FUSEE_CXL_KV_BLOCKPOOL_FREELIST_H_

// Protocol A v2 — host-local DRAM free-list overlay for CxlKvBlockPool.
//
// Spec: docs/design_goals.md §I6 (CoW: every UPDATE allocates a fresh
// block; old block goes through lazy GC), §VI (KV blockpool free list
// = host-local spinlock, NOT LFM).
//
// CxlKvBlockPool already partitions into H segments with per-host
// bump-cursor alloc. iter-4A adds: a DRAM-resident free list per host
// that records lazily-freed block offsets so CoW UPDATE can reuse old
// blocks. The free list is host-local (only owner host frees / allocs
// from it; sharding rule in §I2 makes this single-writer).
//
// Lock primitive: host_local_spinlock_t (PROCESS_SHARED across same-
// host workers, hardware coherent in DRAM). NOT LFM.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "cxl_directory.h"  // host_local_spinlock_t

namespace fusee {

// Per-segment free list living entirely in DRAM (NOT in CXL).
struct BlockFreeList {
  host_local_spinlock_t lock;
  std::vector<uint64_t> stack;  // LIFO of free CXL offsets
};

// Init per-host free list. Caller-owned struct; spinlock initialized
// PROCESS_SHARED so forked workers can share.
int block_freelist_init(BlockFreeList *fl);
void block_freelist_destroy(BlockFreeList *fl);

// Push a freed block offset onto the free list. Host-local spinlock.
void block_freelist_push(BlockFreeList *fl, uint64_t off);

// Pop a block offset for reuse. Returns 0 if list empty. Host-local
// spinlock. Caller falls back to bump alloc on empty.
uint64_t block_freelist_pop(BlockFreeList *fl);

// Number of free blocks currently waiting (relaxed read, advisory).
std::size_t block_freelist_size(const BlockFreeList *fl);

}  // namespace fusee

#endif  // FUSEE_CXL_KV_BLOCKPOOL_FREELIST_H_
