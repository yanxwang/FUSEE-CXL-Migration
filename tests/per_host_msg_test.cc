// iter-4A Phase 5 message-type dispatch smoke test.
//
// Validates spec §I11 (cross-host write via N:1:1:N forward, not LFM):
// the PerHostInvalEntry layout supports 5 distinct message types
// (Invalidate, CacheRegister, CacheEvict, WriteForward, Response) plus
// the legacy iter-3A invalidate path, all sharing the 64-B cacheline.

#include "cxl_per_host_ring.h"

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace fusee;

int main() {
  // Layout sanity: still exactly 64 B.
  if (sizeof(PerHostInvalEntry) != 64) {
    fprintf(stderr, "FAIL: sizeof(PerHostInvalEntry) = %zu (expected 64)\n",
            sizeof(PerHostInvalEntry));
    return 1;
  }

  // Pack / unpack value_size_and_slot helper.
  uint64_t packed = per_host_msg_pack(7, 256);
  if (per_host_msg_slot_idx(packed) != 7 ||
      per_host_msg_value_size(packed) != 256) {
    fprintf(stderr, "FAIL: pack/unpack(7,256) round-trip\n"); return 1;
  }
  if (per_host_msg_pack(0xffffffff, 0xffffffff) !=
      0xffffffffffffffffULL) {
    fprintf(stderr, "FAIL: pack edge case\n"); return 1;
  }

  // Build one of each message type and verify fields readback.
  PerHostInvalEntry e;
  std::memset(&e, 0, sizeof(e));

  // 1. Invalidate (iter-4A): owner -> sharer
  e.op_type = kMsgInvalidate;
  e.bucket_idx = 1234;
  e.value_size_and_slot = per_host_msg_pack(3, 0);
  e.src_worker_op_id = 0xCAFEBABEULL;
  if (e.op_type != kMsgInvalidate || e.bucket_idx != 1234 ||
      per_host_msg_slot_idx(e.value_size_and_slot) != 3 ||
      e.src_worker_op_id != 0xCAFEBABEULL) {
    fprintf(stderr, "FAIL: invalidate fields\n"); return 1;
  }

  // 2. CacheRegister (sharer -> owner)
  std::memset(&e, 0, sizeof(e));
  e.op_type = kMsgCacheRegister;
  e.bucket_idx = 5678;
  e.value_size_and_slot = per_host_msg_pack(5, 0);
  e.key = 0xDEADBEEFULL;
  e.src_worker_slot = 11;
  if (e.op_type != kMsgCacheRegister || e.key != 0xDEADBEEFULL ||
      e.src_worker_slot != 11) {
    fprintf(stderr, "FAIL: register fields\n"); return 1;
  }

  // 3. CacheEvict
  std::memset(&e, 0, sizeof(e));
  e.op_type = kMsgCacheEvict;
  e.bucket_idx = 99;
  e.value_size_and_slot = per_host_msg_pack(1, 0);
  e.key = 0x42424242ULL;
  if (e.op_type != kMsgCacheEvict || e.key != 0x42424242ULL) {
    fprintf(stderr, "FAIL: evict fields\n"); return 1;
  }

  // 4. WriteForward (Option A: payload via CXL pointer)
  std::memset(&e, 0, sizeof(e));
  e.op_type = kMsgWriteForward;
  e.op_kind = 0;  // 0 = update
  e.bucket_idx = 333;
  e.key = 0xAAAAAAAAULL;
  e.payload_cxl_off = 0x40000ULL;  // pointer into requester's blockpool segment
  e.value_size_and_slot = per_host_msg_pack(0, 256);  // 256-B value
  e.src_worker_slot = 7;
  if (e.op_type != kMsgWriteForward || e.op_kind != 0 ||
      e.payload_cxl_off != 0x40000ULL ||
      per_host_msg_value_size(e.value_size_and_slot) != 256) {
    fprintf(stderr, "FAIL: write-forward fields\n"); return 1;
  }

  // 5. Response (owner -> requester for register/forward)
  std::memset(&e, 0, sizeof(e));
  e.op_type = kMsgResponse;
  e.status = 0;  // OK
  e.bucket_idx = 333;
  e.key = 0xAAAAAAAAULL;
  e.payload_cxl_off = 0x80000ULL;  // pointer to owner's blockpool block carrying the result
  e.value_size_and_slot = per_host_msg_pack(2, 256);
  if (e.op_type != kMsgResponse || e.status != 0 ||
      e.payload_cxl_off != 0x80000ULL) {
    fprintf(stderr, "FAIL: response fields\n"); return 1;
  }

  // 6. Legacy iter-3A invalidate path: op_type=0, new_epoch field used
  std::memset(&e, 0, sizeof(e));
  e.op_type = 0;        // legacy
  e.bucket_idx = 9999;
  e.new_epoch = 0xDEADULL;
  e.src_worker_op_id = 0xBEEFULL;
  if (e.new_epoch != 0xDEADULL || e.bucket_idx != 9999) {
    fprintf(stderr, "FAIL: legacy iter-3A new_epoch path\n"); return 1;
  }

  printf("ALL PASS — 6 message types (1-5 iter-4A + legacy) compile + readback\n");
  return 0;
}
