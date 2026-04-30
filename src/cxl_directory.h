#ifndef FUSEE_CXL_DIRECTORY_H_
#define FUSEE_CXL_DIRECTORY_H_

// Protocol A v2 — per-slot directory (DRAM, MAP_SHARED across same-host
// workers). Spec: docs/design_goals.md §I5 (per-slot single-level
// directory), §I7 (directory home in DRAM, no CXL replica), §I8
// (directory lock = host-local, NOT LFM), §III layout.
//
// Each (bucket_idx, slot_idx) has one SlotDirectoryEntry (16 B). The
// owner host's directory authoritatively tracks (state, sharer_bitmap)
// for that slot. Reader hosts NEVER query this directory directly —
// they self-track their cache membership via LocalSelfFlags + N:1:1:N
// register / evict messages (see I9).
//
// Lock primitive: pthread_spin_init with PTHREAD_PROCESS_SHARED. This
// is HOST-LOCAL (DRAM, hardware coherent across same-host workers via
// MAP_SHARED); LFM is FORBIDDEN here (AP7).

#include <pthread.h>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fusee {

// MESI-style state. iter-4A uses I (no sharers), S (one+ sharers), and
// transient M during the writer commit. EXCLUSIVE not modeled separately
// — owner-self always re-registers as a sharer after a write.
enum DirectoryState : uint8_t {
  kDirStateInvalid = 0,
  kDirStateShared  = 1,
  kDirStateModified = 2,  // transient: writer holds spinlock + invalidating
};

// Distinct typedef so accidental shm_mutex_t* assignment is rejected
// at compile time (Hard enforcement H1 / I8 trip wire).
using host_local_spinlock_t = pthread_spinlock_t;

struct SlotDirectoryEntry {  // 16 B
  uint8_t   state;           // DirectoryState
  uint8_t   sharer_bitmap;   // bit i = host i is a sharer (max 8 hosts)
  uint8_t   _pad_a[2];
  uint32_t  version;         // ABA defense + observability (incremented per write)
  host_local_spinlock_t spinlock;  // pthread_spinlock_t = sizeof int = 4 B on x86
  uint32_t  _pad_b;          // pad to 16 B alignment
};

// pthread_spinlock_t is 4 bytes on glibc x86-64. Total: 1+1+2+4+4+4 = 16.
// Verify at compile time.
static_assert(sizeof(SlotDirectoryEntry) == 16,
              "SlotDirectoryEntry must be exactly 16 B");

// Owner-host directory: SlotDirectoryEntry[num_buckets][slots_per_bucket]
// flattened into a 1-D array. Mapped MAP_SHARED|MAP_ANONYMOUS pre-fork
// so all same-host worker processes share the same physical pages.
//
// The directory lives ONLY on the key's owner host; non-owner hosts
// have no directory state for that slot. Cross-host paths use
// register/evict/forward messages (Phase 5 onwards).
struct SlotDirectory {
  uint32_t num_buckets;
  uint32_t slots_per_bucket;
  // entries[i] for i in [0, num_buckets * slots_per_bucket)
  SlotDirectoryEntry *entries;
};

// Bytes to mmap for a directory of (B, S) buckets/slots.
inline std::size_t slot_directory_bytes(uint32_t num_buckets,
                                        uint32_t slots_per_bucket) {
  return static_cast<std::size_t>(num_buckets) * slots_per_bucket *
         sizeof(SlotDirectoryEntry);
}

// Initialize a SlotDirectory whose backing memory was already mmap'd
// MAP_SHARED|MAP_ANONYMOUS by the caller (typical: parent runner pre-
// fork). Initializes pthread_spinlock_t with PTHREAD_PROCESS_SHARED.
// Returns 0 on success.
int slot_directory_init(SlotDirectory *dir, void *backing_mem,
                        uint32_t num_buckets, uint32_t slots_per_bucket);

// Tear down (destroy spinlocks). Call from the same host primary that
// initialized.
void slot_directory_destroy(SlotDirectory *dir);

// Index helper.
inline SlotDirectoryEntry *slot_directory_entry(const SlotDirectory *dir,
                                                uint32_t bucket_idx,
                                                uint32_t slot_idx) {
  return &dir->entries[bucket_idx * dir->slots_per_bucket + slot_idx];
}

// Lock / unlock helpers (typed against host_local_spinlock_t to enforce
// the "no LFM here" rule at compile time).
inline void slot_directory_lock(SlotDirectoryEntry *e) {
  pthread_spin_lock(&e->spinlock);
}
inline void slot_directory_unlock(SlotDirectoryEntry *e) {
  pthread_spin_unlock(&e->spinlock);
}

// CRUD primitives. Caller must hold the entry's spinlock.
inline void slot_directory_set_sharer(SlotDirectoryEntry *e, uint32_t host_id) {
  e->sharer_bitmap = (uint8_t)(e->sharer_bitmap | (1u << host_id));
  if (e->state == kDirStateInvalid) e->state = kDirStateShared;
}
inline void slot_directory_clear_sharer(SlotDirectoryEntry *e, uint32_t host_id) {
  e->sharer_bitmap = (uint8_t)(e->sharer_bitmap & ~(1u << host_id));
  if (e->sharer_bitmap == 0) e->state = kDirStateInvalid;
}
inline uint8_t slot_directory_sharers(const SlotDirectoryEntry *e) {
  return e->sharer_bitmap;
}

}  // namespace fusee

#endif  // FUSEE_CXL_DIRECTORY_H_
