#ifndef FUSEE_CXL_KV_STORE_H_
#define FUSEE_CXL_KV_STORE_H_

// Compile-time protocol switch for the CXL-FUSEE KV store.
//
// Build with one of:
//   -DCONSENSUS_OPT=1    // Option A (sync replication w/ ACK, A-v2 SPSC ring)
//   -DCONSENSUS_OPT=2    // Option B (eager push, no ACK wait)
//   -DCONSENSUS_OPT=3    // Option C (lazy release consistency) [default]
//
// For readability the numeric values are also aliased as FUSEE_OPT_A/B/C;
// downstream build systems can write -DCONSENSUS_OPT=FUSEE_OPT_A etc.
//
// The three underlying classes share the same public shape (attach, stop,
// insert, update, remove, search, bytes_for, num_buckets, replicated_ops),
// so downstream code that uses `fusee::CxlKvStore` compiles unchanged
// across protocols.

#define FUSEE_OPT_A 1
#define FUSEE_OPT_B 2
#define FUSEE_OPT_C 3
#define FUSEE_OPT_F 4

#ifndef CONSENSUS_OPT
#define CONSENSUS_OPT FUSEE_OPT_C
#endif

#if CONSENSUS_OPT == FUSEE_OPT_A
#include "cxl_kv_ops_A.h"
namespace fusee {
using CxlKvStore = CxlKvStoreA;
constexpr char kConsensusOpt = 'A';
}
#elif CONSENSUS_OPT == FUSEE_OPT_B
#include "cxl_kv_ops_B.h"
namespace fusee {
using CxlKvStore = CxlKvStoreB;
constexpr char kConsensusOpt = 'B';
}
#elif CONSENSUS_OPT == FUSEE_OPT_C
#include "cxl_kv_ops_C.h"
namespace fusee {
using CxlKvStore = CxlKvStoreC;
constexpr char kConsensusOpt = 'C';
}
#elif CONSENSUS_OPT == FUSEE_OPT_F
#include "cxl_kv_ops_F.h"
namespace fusee {
using CxlKvStore = CxlKvStoreF;
constexpr char kConsensusOpt = 'F';
}
#else
#error "CONSENSUS_OPT must be FUSEE_OPT_{A,B,C,F}"
#endif

#endif // FUSEE_CXL_KV_STORE_H_
