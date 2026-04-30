#include "cxl_directory.h"

#include <cstring>
#include <pthread.h>

namespace fusee {

int slot_directory_init(SlotDirectory *dir, void *backing_mem,
                        uint32_t num_buckets, uint32_t slots_per_bucket) {
  if (!dir || !backing_mem || num_buckets == 0 || slots_per_bucket == 0) {
    return -1;
  }
  dir->num_buckets = num_buckets;
  dir->slots_per_bucket = slots_per_bucket;
  dir->entries = static_cast<SlotDirectoryEntry *>(backing_mem);

  std::size_t total = static_cast<std::size_t>(num_buckets) * slots_per_bucket;
  // Zero state + sharers; init spinlocks PROCESS_SHARED so forked
  // children share the lock state via MAP_SHARED.
  for (std::size_t i = 0; i < total; i++) {
    SlotDirectoryEntry *e = &dir->entries[i];
    e->state = kDirStateInvalid;
    e->sharer_bitmap = 0;
    e->_pad_a[0] = e->_pad_a[1] = 0;
    e->version = 0;
    if (pthread_spin_init(&e->spinlock, PTHREAD_PROCESS_SHARED) != 0) {
      return -2;
    }
  }
  return 0;
}

void slot_directory_destroy(SlotDirectory *dir) {
  if (!dir || !dir->entries) return;
  std::size_t total =
      static_cast<std::size_t>(dir->num_buckets) * dir->slots_per_bucket;
  for (std::size_t i = 0; i < total; i++) {
    pthread_spin_destroy(&dir->entries[i].spinlock);
  }
  dir->entries = nullptr;
  dir->num_buckets = 0;
  dir->slots_per_bucket = 0;
}

}  // namespace fusee
