#ifndef FUSEE_CXL_KV_OPS_F_DECOMP_H_
#define FUSEE_CXL_KV_OPS_F_DECOMP_H_

// Protocol F stage-decomposition probe.
//
// Build with -DFUSEE_F_DECOMP=1 to enable. Default off → all macros expand
// to no-ops, zero runtime cost.  When on, each instrumented stage records a
// nanosecond sample into a thread-local vector; the microbench harness
// reads them out at the end of each op-type loop and prints per-stage
// p50/p99/min/max.
//
// Stages are aligned with the 10-step INSERT walkthrough in
// docs/protocol_F_design_and_plan.md §2.4 + the equivalent UPDATE / DELETE /
// SEARCH paths in src/cxl_kv_ops_F.cc.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace fusee {

enum FDecompStage {
  // INSERT (10 + 1)
  kF_INS_PRE_FLUSH = 0,    // step 1: flush 2 bucket lines + mfence
  kF_INS_PRE_SCAN,         // step 2: unlocked 14-slot scan
  kF_INS_ALLOC,            // step 4: pool.alloc_tiny
  kF_INS_WRITE_PAIR,       // step 5: pool.write KV pair + flush + sfence
  kF_INS_LOCK,             // step 6: LFM lock_slot acquire
  kF_INS_VERIFY,           // steps 7+8: re-flush + under-lock re-scan
  kF_INS_PUBLISH,          // step 9: slot.packed store + flush + sfence
  kF_INS_UNLOCK,           // step 10: LFM unlock_slot
  kF_INS_CACHE,            // index_cache.insert
  kF_INS_TOTAL,            // end-to-end

  // UPDATE
  kF_UPD_FLUSH_SCAN,       // bucket flush + find target slot
  kF_UPD_ALLOC_WRITE,      // pool.alloc_tiny + pool.write new pair
  kF_UPD_LOCK,             // LFM lock_slot acquire
  kF_UPD_VERIFY,           // re-flush + re-verify under lock
  kF_UPD_PUBLISH,          // slot.packed store + flush + sfence
  kF_UPD_UNLOCK,           // LFM unlock_slot
  kF_UPD_FREE,             // pool.free old KV pair (set bitmap bit + flush)
  kF_UPD_CACHE,            // index_cache.insert
  kF_UPD_TOTAL,

  // DELETE
  kF_DEL_FLUSH_SCAN,
  kF_DEL_LOCK,
  kF_DEL_VERIFY,
  kF_DEL_CLEAR,            // slot.packed = 0 + flush + sfence
  kF_DEL_UNLOCK,
  kF_DEL_FREE,             // pool.free old
  kF_DEL_CACHE,            // index_cache.evict
  kF_DEL_TOTAL,

  // SEARCH
  kF_SRC_CACHE_LOOKUP,     // index_cache.lookup (DRAM)
  kF_SRC_FAST_PATH,        // slot flush+LD + KV pair read + verify
  kF_SRC_SLOW_PATH,        // bucket scan path (when cache misses / collision)
  kF_SRC_CACHE_UPDATE,     // record_hit / record_miss / insert
  kF_SRC_TOTAL,

  kF_DECOMP_COUNT,
};

const char *f_decomp_name(int stage);

struct FDecompProbe {
  std::vector<uint32_t> stage_ns[kF_DECOMP_COUNT];
};

FDecompProbe &f_decomp_probe();          // thread-local singleton
void          f_decomp_reset();
void          f_decomp_dump_stages(const char *prefix, FILE *out);

#if defined(FUSEE_F_DECOMP) && FUSEE_F_DECOMP

inline uint64_t f_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline void f_decomp_record(int stage, uint64_t ns) {
  if (ns > 0xFFFFFFFFULL) ns = 0xFFFFFFFFULL;
  f_decomp_probe().stage_ns[stage].push_back(static_cast<uint32_t>(ns));
}

#define F_DECOMP_TS(name)         uint64_t name = ::fusee::f_now_ns()
#define F_DECOMP_REC(stage, a, b) ::fusee::f_decomp_record((stage), (b) - (a))

#else

#define F_DECOMP_TS(name)         do {} while (0)
#define F_DECOMP_REC(stage, a, b) do {} while (0)

#endif

}  // namespace fusee

#endif  // FUSEE_CXL_KV_OPS_F_DECOMP_H_
