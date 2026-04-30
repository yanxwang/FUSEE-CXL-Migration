#include "cxl_kv_blockpool_freelist.h"

#include <pthread.h>

namespace fusee {

int block_freelist_init(BlockFreeList *fl) {
  if (!fl) return -1;
  // PROCESS_SHARED so spinlock is valid across forked workers via
  // MAP_SHARED memory if the caller chose to place BlockFreeList in
  // shared memory. Tests use process-local; the runtime path will
  // place these in MAP_SHARED|MAP_ANONYMOUS pre-fork.
  if (pthread_spin_init(&fl->lock, PTHREAD_PROCESS_SHARED) != 0) return -2;
  fl->stack.clear();
  return 0;
}

void block_freelist_destroy(BlockFreeList *fl) {
  if (!fl) return;
  pthread_spin_destroy(&fl->lock);
  fl->stack.clear();
}

void block_freelist_push(BlockFreeList *fl, uint64_t off) {
  if (!fl || off == 0) return;
  pthread_spin_lock(&fl->lock);
  fl->stack.push_back(off);
  pthread_spin_unlock(&fl->lock);
}

uint64_t block_freelist_pop(BlockFreeList *fl) {
  if (!fl) return 0;
  pthread_spin_lock(&fl->lock);
  uint64_t off = 0;
  if (!fl->stack.empty()) {
    off = fl->stack.back();
    fl->stack.pop_back();
  }
  pthread_spin_unlock(&fl->lock);
  return off;
}

std::size_t block_freelist_size(const BlockFreeList *fl) {
  return fl ? fl->stack.size() : 0;
}

}  // namespace fusee
