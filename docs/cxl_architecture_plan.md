# CXL-FUSEE Architecture Plan

## 2026-04-01 — Initial Design

> Transform FUSEE (RDMA-based disaggregated KV store, FAST'23) into a CXL-based distributed KV store.

---

## 1. Goal

Replace all RDMA operations in FUSEE with CXL shared memory access + software-based consensus, turning it into a CXL-based distributed KV store where multiple unified nodes (compute + memory) achieve data consistency through CXL memory.

## 2. Target Topology

```
   Node 0 (DRAM)     Node 1 (DRAM)     Node 2 (DRAM)
   [Hash Table]      [Hash Table]      [Hash Table]
   [KV Data]         [KV Data]         [KV Data]
   [CXLNode]         [CXLNode]         [CXLNode]
       |                  |                  |
       +--------+---------+--------+---------+
                |   CXL Switch     |
                +---------+--------+
                          |
                CXL Type 3 Memory Expander
                [ConsensusLog | OpLog | StagingBuffers | GlobalMeta]
```

- **3 unified nodes**: each is both compute and storage (no separate client/server)
- **1 CXL Type 3 memory expander**: connected via CXL switch, forming global shared address space
- **Hash table + KV data**: remain in each node's local DRAM (replicated)
- **CXL memory**: holds consensus state, operation logs, data staging buffers, global metadata

## 3. Key Design Constraints

1. **No cross-node hardware atomics**: Multi-node shared CXL has no unified global coherence domain. CAS/FAA are NOT reliable across nodes. All consensus must be software-based using only load/store + memory fences.
2. **Crash recovery**: FUSEE's embedded operation log scheme must be preserved and adapted for CXL.
3. **Coroutine model**: Keep Boost.Fiber for high-concurrency KV request handling.
4. **CXL access**: via `shm_open()` + `mmap()` directly on real CXL hardware.
5. **Data replication**: KV data staged through CXL shared memory (no separate network for data transfer).

## 4. Architecture Comparison: FUSEE vs CXL-FUSEE

| Aspect | FUSEE (RDMA) | CXL-FUSEE |
|--------|-------------|-----------|
| Node roles | Client (compute) + Server (memory) separate | Unified CXLNode (both) |
| Transport | RDMA one-sided read/write/CAS over InfiniBand | Local DRAM access + CXL load/store |
| Index update | RDMA CAS on remote hash slots | Software consensus via CXL shared log |
| KV replication | RDMA WRITE to remote servers | CXL staging buffer -> local DRAM copy |
| Connection setup | UDP control plane + IB QP creation | shm_open + mmap (shared CXL region) |
| Consensus | CAS-based (hardware atomic on remote memory) | Propose-Vote-Commit on CXL log (software) |
| Crash recovery | Embedded op log via RDMA read | Op log in CXL memory (visible to all nodes) |

## 5. CXL Shared Memory Layout

Mapped via `shm_open("/cxl_fusee") + mmap()` at agreed base address on all nodes.

```
Offset 0x0000_0000: GlobalHeader (4 KB)
  - magic (0xCXL0FUSE), num_nodes, region_size
  - offsets to each sub-region below

Offset 0x0000_1000: Heartbeat & Membership (4 KB)
  - Per-node entry (64B cache-line aligned):
    epoch(8) | timestamp_us(8) | status(1: ALIVE/SUSPECT/DEAD) | pad(47)

Offset 0x0000_2000: Shared RaceHashRoot (4 KB)
  - Authoritative copy of RaceHashRoot
  - subtable_entry[][].server_id -> now means node_id

Offset 0x0000_3000: AllocationDirectory (16 MB)
  - Per-node block allocation bitmap (4 MB each)
  - next_free_block(8) | num_blocks(8) | bitmap[]
  - Each node updates its own section; reads others for recovery

Offset 0x0100_3000: ConsensusLog (512 MB)
  - Array of ConsensusLogEntry (64B each, cache-line aligned)
  - Indexed by hash(slot_addr) % NUM_CONSENSUS_SLOTS as ring buffer

  ConsensusLogEntry structure:
    slot_addr(8)        — which hash slot this covers
    proposer_id(8)      — node that proposed (0, 1, or 2)
    proposal_epoch(8)   — monotonic per-node epoch
    old_slot_value(8)   — expected current 8-byte RaceHashSlot
    new_slot_value(8)   — proposed new 8-byte RaceHashSlot
    vote[3](3)          — per-node: 0=none, 1=ACK, 2=NACK
    status(1)           — FREE=0, PROPOSED=1, COMMITTED=2, ABORTED=3
    pad(4)
    commit_timestamp(8)
    pad(8)

Offset 0x2100_3000: OperationLog (256 MB)
  - Per-node ring buffer (~80 MB each) for crash recovery
  - OpLogHeader: head(8) | tail(8) | capacity(8) | pad(40)
  - OpLogEntry (128B each):
    op_epoch(8) | op_type(1) | op_status(1: IN_PROGRESS/COMMITTED/DONE) |
    node_id(1) | pad(1) | key_len(4) | value_len(4) | key_hash(8) | pad(4) |
    local_kv_addr(8) | old_slot_value(8) | new_slot_value(8) |
    consensus_log_idx(8) | replica_addrs[3](24) | pad(8)

Offset 0x3100_3000: DataStagingArea (remaining, ~256 MB+)
  - Per-node ring buffer for KV data replication
  - StagingHeader: head(8) + tail(8)
  - StagingEntry: length(4) | src_node(4) | data[]
  - Writer puts KV payload here; other nodes copy to local DRAM and advance head
```

## 6. Software Consensus Protocol: Per-Slot Propose-Vote-Commit

### Overview

Replaces FUSEE's RDMA CAS-based consensus. Uses **only load/store + memory fences** (sfence/lfence on x86). Provides per-slot linearizability with 2-of-3 majority voting.

### Protocol Steps (for INSERT/UPDATE/DELETE)

**Phase 0 — Claim:**
1. Compute `idx = hash(slot_addr) % NUM_CONSENSUS_SLOTS`
2. Scan ring at `idx` for free entry (status == FREE or old COMMITTED/ABORTED)
3. Write `proposer_id + proposal_epoch` -> `sfence`
4. Read back -> verify no higher-epoch claim from another node
   - Tie broken by lower node_id
   - If lost, retry from step 2

**Phase 1 — Propose:**
1. Write full ConsensusLogEntry with `status = PROPOSED`, `vote[self] = ACK` -> `sfence`
2. Brief spin + re-read to confirm no race on proposer_id

**Phase 2 — Vote (background voter fibers on each node):**
Each node runs a background voter fiber continuously scanning ConsensusLog:
- On seeing PROPOSED entry for a slot this node holds a replica of:
  - Read own local hash slot value
  - If matches `old_slot_value` (or empty for INSERT): write `vote[my_id] = ACK` -> `sfence`
  - If mismatch (concurrent modification): write `vote[my_id] = NACK` -> `sfence`

**Phase 3 — Commit/Abort (proposer):**
- Poll `vote[]` array in CXL
- If >= 2 ACKs (of 3, including self): write `status = COMMITTED` -> `sfence`
- If timeout or >= 2 NACKs: write `status = ABORTED` -> `sfence`, retry operation

**Phase 4 — Apply (all nodes):**
- Each node's replication fiber sees COMMITTED entries -> applies `new_slot_value` to local hash table
- For INSERT/UPDATE: also copies KV data from CXL staging to local DRAM
- Proposer returns success to caller

### Linearizability Argument
- At most one proposal per slot is active (epoch ordering in claim phase)
- 2-of-3 voting prevents split-brain
- COMMITTED status written to CXL is the global serialization point

### SEARCH (read-only, no consensus)
- Read directly from local DRAM hash table + KV data
- Optionally check ConsensusLog for pending PROPOSED on target slot (for strict linearizability)

## 7. Per-Operation Data Flow

### SEARCH
1. Hash key -> compute bucket indices [LOCAL]
2. Read hash buckets from local DRAM [LOCAL]
3. Find matching slot (fingerprint) [LOCAL]
4. Read KV data from local DRAM [LOCAL]
5. Verify key match, return value [LOCAL]

### INSERT
1. Allocate KV block in local DRAM; update CXL AllocationDirectory [LOCAL + CXL store]
2. Write KV header + key + value to local DRAM [LOCAL]
3. Stage KV data in CXL DataStagingArea [CXL store + sfence]
4. Write OpLogEntry (status=IN_PROGRESS) to CXL OpLog [CXL store + sfence]
5. Read hash buckets from local DRAM; find empty slot [LOCAL]
6. **Consensus**: propose slot change (old=0x0, new=slot_with_pointer) [CXL consensus protocol]
7. On COMMITTED: update OpLog status=COMMITTED [CXL store]
8. Background on other nodes: copy KV from staging to local DRAM, apply slot to local hash table

### UPDATE
1. Read hash buckets from local DRAM; find existing slot [LOCAL]
2. Read old KV from local DRAM; verify key [LOCAL]
3. Allocate new KV block; write new value to local DRAM [LOCAL]
4. Stage new KV in CXL; write OpLog [CXL store]
5. **Consensus**: propose slot change (old=current_slot, new=new_pointer_slot) [CXL consensus]
6. On COMMITTED: free old block; other nodes replicate in background [LOCAL + CXL]

### DELETE
1. Read hash buckets; find matching slot [LOCAL]
2. Read KV; verify key [LOCAL]
3. Write OpLog [CXL store]
4. **Consensus**: propose slot change (old=current_slot, new=0x0) [CXL consensus]
5. On COMMITTED: mark old KV block for GC [LOCAL + CXL]

## 8. Node Architecture: CXLNode Class

Replaces both `Client` and `Server`:

```cpp
class CXLNode {
    // Identity
    uint32_t node_id_;        // 0, 1, or 2
    uint32_t num_nodes_;      // 3

    // CXL shared memory (mmap'd)
    CXLMemoryManager *cxl_mm_;

    // Local DRAM management (from ServerMM, without RDMA MR)
    LocalMemoryManager *local_mm_;
    RaceHashRoot *local_root_;       // local hash table replica
    void *local_hash_area_;
    void *local_kv_area_;

    // Fibers (Boost.Fiber, kept from FUSEE)
    // - KV request handler fibers (num_coroutines)
    // - Consensus voter fiber (background)
    // - Replication applier fiber (background)
    // - Heartbeat fiber (background)
    // - GC fiber (background)

    // KV operations (from Client, adapted)
    void* kv_search(KVInfo *info);
    int   kv_insert(KVInfo *info);
    int   kv_update(KVInfo *info);
    int   kv_delete(KVInfo *info);

    // Consensus (new)
    int   propose_slot_change(uint64_t slot_addr, uint64_t old_val, uint64_t new_val);
    void  voter_fiber_main();        // background: vote on others' proposals
    void  applier_fiber_main();      // background: apply COMMITTED entries to local DRAM

    // Crash recovery (from ClientCR, adapted)
    int   recover_from_crash();
    void  scan_oplog_for_recovery();
};
```

## 9. File Transformation Plan

### Files to REMOVE (RDMA-specific, no longer needed):
- `src/ib.h`, `src/ib.cc` — InfiniBand QP/WR utilities
- `src/nm.h`, `src/nm.cc` — RDMA network manager (UDPNetworkManager)

### New files to CREATE:
| File | Purpose |
|------|---------|
| `src/fence.h` | Hardware fence wrappers: `cxl_sfence()`, `cxl_lfence()`, `cxl_mfence()` |
| `src/cxl_mm.h/cc` | CXL memory manager: shm_open, mmap, layout init, region accessors |
| `src/cxl_consensus.h/cc` | ConsensusLogEntry struct, propose/vote/commit protocol |
| `src/cxl_node.h/cc` | Unified CXLNode class (replaces Client + Server) |
| `src/cxl_node_cr.h/cc` | Crash recovery adapted for CXL |
| `src/local_mm.h/cc` | Local DRAM memory manager (from ServerMM, without RDMA MR) |

### Files to HEAVILY MODIFY:
| File | Changes |
|------|---------|
| `src/kv_utils.h/cc` | Remove: QpInfo, MrInfo, IbInfo, ConnInfo, KVMsg, GlobalInfo, IB fields from GlobalConfig, serialize/deserialize functions. Add: CXL config fields (cxl_dev_path, cxl_region_size, node_id, num_nodes). Keep: KVLogHeader, KVLogTail, KVLogOp, GlobalConfig (refactored), helper functions. |
| `src/hashtable.h/cc` | Keep RACE Hash structures. Change `server_id` -> `node_id` in RaceHashSlot, RaceHashSubtableEntry. Remove rkey fields from KVTableAddrInfo, KVRWAddr, KVCASAddr. |
| `src/CMakeLists.txt` | New source list, remove ibverbs linkage, add -lrt for shm_open. |

### Files to KEEP with minor changes:
- `src/spinlock.h` — add CXL-aware fence variant
- `src/kv_debug.h` — as-is

### Test/benchmark adaptation:
- `tests/` — adapt to CXLNode API (multi-process instead of separate client/server binaries)
- `ycsb-test/` — adapt server/client programs to unified CXLNode
- `micro-test/` — adapt latency/throughput tests

## 10. Phased Implementation

### Phase 1: CXL Infrastructure
1. Create `src/fence.h` — hardware fence wrappers
2. Create `src/cxl_mm.h/cc` — CXL memory manager (shm_open + mmap + layout init)
3. Refactor `src/kv_utils.h/cc` — add CXL config, remove RDMA structs
4. Create `src/local_mm.h/cc` — extract ServerMM block allocation (without RDMA MR)
5. **Verify**: multi-process CXL memory mapping + read/write visibility

### Phase 2: Consensus Protocol
1. Create `src/cxl_consensus.h/cc` — full propose-vote-commit implementation
2. Background voter fiber implementation
3. **Verify**: 3-process consensus correctness under contention

### Phase 3: Unified Node + KV Operations
1. Create `src/cxl_node.h/cc` — port kv_search/insert/update/delete from Client
2. Implement data replication via CXL staging buffers
3. Local hash table management
4. **Verify**: single-node then multi-node CRUD correctness

### Phase 4: Crash Recovery
1. Create `src/cxl_node_cr.h/cc` — OpLog scan + consensus log replay + hash table rebuild
2. **Verify**: kill-restart scenarios, data consistency

### Phase 5: Cleanup & Benchmarks
1. Remove old RDMA code (ib.h/cc, nm.h/cc, server.h/cc, client.h/cc)
2. Update root CMakeLists.txt
3. Port YCSB and micro-benchmarks to CXLNode
4. Performance tuning (consensus log sizing, staging buffer sizing, voter poll interval)

## 11. Verification Plan

1. **CXL memory unit test**: map from 3 processes, store from one + sfence, load from others + lfence, verify visibility
2. **Consensus correctness**: 3 processes propose conflicting updates to same slot, verify linearizable outcome
3. **KV CRUD correctness**: insert N keys, search all, update subset, delete subset, verify
4. **Crash recovery**: kill a node mid-operation, restart, verify data consistency
5. **YCSB benchmark**: run workloads A-D, compare throughput/latency

## 12. Open Questions / Discussion Points

- [ ] Exact voter polling interval (balance between latency and CPU overhead)
- [ ] ConsensusLog ring buffer overflow policy (block? evict oldest committed?)
- [ ] Should SEARCH check ConsensusLog for pending proposals? (strict linearizability vs performance)
- [ ] CXL memory mapping: MAP_FIXED vs offset-based addressing?
- [ ] How to handle subtable resizing (RACE Hash global/local depth changes) under new consensus?
- [ ] GC strategy for committed ConsensusLog entries and freed KV blocks

---

# REVISION 2 (2026-04-08): Per-Bucket LFM Lock Design

> The Propose-Vote-Commit design above has a correctness issue in the Claim phase
> (two nodes can both believe they claimed successfully) and high latency due to
> voter polling. This revision replaces voting with **real software mutex locks**
> that work on non-coherent CXL memory.

## R2.1 Why the Old Design is Replaced

### Problems with Propose-Vote-Commit
1. **Claim race**: Without mutual exclusion, two nodes' claim writes can both
   "appear successful" — each sees its own value after sfence, because CXL Type 3
   has no global coherence domain.
2. **Voter polling latency**: Background voter fibers add 15-50 μs even on the
   uncontended path.
3. **Per-slot semantics mismatch**: INSERT actually scans 7 slots in a bucket
   to find a free one — a per-slot lock cannot protect this scan.

### What Changed
- **Per-Bucket lock** instead of per-slot consensus log
- **LFM software mutex** (from `cxl_shm_profiling/locks/lfm_lock.c`) replaces
  voting entirely
- The lock IS the consensus

## R2.2 LFM as the Foundation

We use the `cxl_shm_profiling` repository's `shm_mutex_t` API:

```c
typedef struct shm_mutex_t shm_mutex_t;
void     shm_mutex_init(shm_mutex_t *m);
uint64_t shm_mutex_lock(shm_mutex_t *m, int my_id, int num_hosts);
void     shm_mutex_unlock(shm_mutex_t *m, int my_id);
```

Three implementations available; we choose **LFM** (Lamport's Fast Mutex) because:
- Uncontended fast path is ~5 CACHELINE ops (lowest of the three)
- KV slot contention is naturally low (keys spread across millions of buckets)
- Fairness not required for KV ops

Tradeoff matrix:

| | LFM | Peterson | Bakery |
|---|---|---|---|
| Uncontended latency | **Lowest (~5 ops)** | Medium (~4×log2N) | Highest (~3N) |
| Fairness | None | Bounded | FCFS |
| Best for | Low contention | Medium | High contention |

LFM only uses load/store + sfence/mfence — works on non-coherent CXL.

## R2.3 Revised CXL Memory Layout

```
CXL Type 3 Shared Memory (mmap'd by all nodes):

Offset 0x0000_0000: GlobalHeader (4 KB)
Offset 0x0000_1000: Heartbeat & Membership (4 KB)
Offset 0x0000_2000: Shared RaceHashRoot (4 KB)
Offset 0x0000_3000: AllocationDirectory (16 MB)

Offset 0x0100_3000: BucketLock Table (~256 MB)         <-- NEW
  array of BucketLockEntry, indexed by bucket_idx
  
  struct BucketLockEntry {
    shm_mutex_t    lock;           // LFM mutex (cache-line aligned)
    RaceHashBucket bucket;         // 64B = 7 slots
    char           pad[...];       // align to next cache line
  };

Offset 0x1100_3000: OpLog Area (256 MB)                <-- per-node ring
  Node 0: ~80 MB region (only Node 0 writes)
  Node 1: ~80 MB region (only Node 1 writes)
  Node 2: ~80 MB region (only Node 2 writes)

Offset 0x2100_3000: Data Staging Area (~512 MB)        <-- per-node ring
  Node 0 / Node 1 / Node 2 each owns ~170 MB region
```

### BucketLockEntry — the new core structure

Replaces the old ConsensusLogEntry. Each bucket has its own lock + the actual
bucket data colocated (fits in ~2 cache lines).

```c
struct BucketLockEntry {
    shm_mutex_t   lock;            // LFM, owns 5 cache lines
    RaceHashBucket bucket;         // 64 B (7 × 8B slots)
};
```

### OpLog and Staging — single-producer rings

To avoid concurrent writes to OpLog and Staging Buffer, **partition by writer**:
- Each node owns one ring buffer region in each (OpLog and Staging)
- Only the owner writes (single producer) → no lock, no contention
- Other nodes are read-only consumers, each tracking its own `local_head`
- GC: owner reclaims entry when `min(all consumers' local_head) > entry_offset`

```c
struct OpLogEntry {            // 128 B fixed, cache-line aligned
    uint64_t epoch;
    uint8_t  op_type;          // INSERT / UPDATE / DELETE
    uint8_t  status;           // IN_PROGRESS / COMMITTED / DONE
    uint8_t  node_id;
    uint64_t key_hash;
    uint64_t bucket_idx;
    uint16_t slot_idx;
    uint64_t old_slot_value;
    uint64_t new_slot_value;
    uint64_t staging_offset;   // pointer into Staging Buffer
    uint32_t payload_size;
    uint32_t crc;
    char     pad[...];
};

struct StagingEntry {
    uint64_t entry_id;
    uint32_t size;
    uint32_t flags;            // READY / IN_USE — publish barrier
    uint64_t crc;
    char     payload[];        // KV block
};
```

## R2.4 Revised KV Operation Protocol (6 sync steps + 1 async)

### INSERT
```
Step 1: bucket_idx = hash(key) % NUM_BUCKETS               // local
Step 2: shm_mutex_lock(&CXL.BucketLock[bucket_idx].lock)   // ~5 ops
Step 3: scan CXL.BucketLock[bucket_idx].bucket for fp=0    // 1 read
        - if no empty slot: try next candidate bucket
        - if all 4 candidate buckets full: TABLE_FULL
        - if duplicate key found: KEY_EXISTS
Step 4: write KV to local DRAM
        write KV payload to CXL Staging Buffer (Node N's region)
        write OpLog entry: status=IN_PROGRESS              // ~3 ops
Step 5: write CXL.BucketLock[bucket_idx].bucket.slot[i] = A
        update local index cache
        update OpLog: status=COMMITTED                     // 2 ops
        ===== COMMIT POINT =====
Step 6: shm_mutex_unlock(...)                              // ~2 ops
Step 7 (async): replication fibers on other nodes scan BucketLock
        and copy changes to their local DRAM
```

### UPDATE
Same flow, except Step 3 finds the slot with matching `fp + key`, and Step 5
writes the new pointer to that slot.

### DELETE
Same flow, except Step 5 writes 0 to the matching slot.

### SEARCH (no lock needed)
```
Fast path: scan local bucket cache, return if hit
Slow path: CACHELINE_LOAD CXL.BucketLock[idx].bucket
           if found: read KV from CXL Staging (owner's region)
                     update local cache
                     return
           else: KEY_NOT_FOUND
```

### Latency Comparison

| | Old (Propose-Vote-Commit) | New (LFM) |
|---|---|---|
| Sync ops | ~10 + voter wait | ~12, no wait |
| Latency (uncontended) | 15-50 μs | **5-10 μs** |
| Conflict handling | ABORT + retry whole op | spin-wait on lock |
| Claim race | Yes (correctness bug) | Impossible |

## R2.5 Crash Recovery (Revised)

```
On heartbeat timeout for Node X:

1. Surviving nodes detect Node X DEAD via Heartbeat & Membership table
2. Force-release locks held by Node X:
   for each BucketLockEntry:
     if lock owner == X: reset lock state (y=0, b[X]=0)
3. Replay Node X's OpLog (read-only, single producer guarantees
   no torn writes thanks to flags=READY publish barrier):
   for each OpLog entry of Node X:
     if status == COMMITTED: ensure applied locally
     if status == IN_PROGRESS:
       check current BucketLock state
       if new_value matches: redo (treat as committed)
       else: rollback (free allocated KV block)
4. Mark Node X DEAD in membership; resume normal operation
```

Per-node OpLog simplifies recovery dramatically: each survivor node only needs
to read one OpLog (the dead node's) — no global log parsing.

## R2.6 Revised File Layout

| File | Purpose |
|---|---|
| `src/cxl_node.h/c` | Main node class (replaces `client.h/c` + `server.h/c`) |
| `src/cxl_bucket_lock.h/c` | BucketLock table init + LFM wrapper |
| `src/cxl_oplog.h/c` | Per-node OpLog ring buffer (read/write) |
| `src/cxl_staging.h/c` | Per-node Staging Buffer ring + GC |
| `src/cxl_kv_ops.h/c` | INSERT / SEARCH / UPDATE / DELETE 6-step protocol |
| `src/cxl_replication.h/c` | Background replication fiber |
| `src/cxl_recovery.h/c` | Heartbeat-driven crash recovery |
| `external/cxl_shm_profiling/` | Imported as submodule for `shm_mutex_t` etc. |

## R2.7 Implementation Roadmap (Revised Phases)

### Phase A: Bring up `cxl_shm_profiling` integration
- [ ] Add `cxl_shm_profiling` as submodule
- [ ] Build `libglobal_allocator.a` and link from FUSEE build
- [ ] Smoke test `shm_mutex_t` with 3 processes on `tmpfs` mock

### Phase B: CXL memory layout & init
- [ ] Allocate CXL region via `shm_malloc_id`
- [ ] Initialize GlobalHeader, BucketLock table, OpLog/Staging rings
- [ ] Sanity test: lock/unlock a BucketLock from 3 processes

### Phase C: KV operations under lock
- [ ] `cxl_kv_insert` — 6 step flow
- [ ] `cxl_kv_search` — fast path + slow path with CXL fallback
- [ ] `cxl_kv_update` / `cxl_kv_delete`
- [ ] Multi-process correctness test (3 nodes, concurrent INSERT)

### Phase D: Background fibers
- [ ] Replication fiber: scan BucketLock for changes, pull from Staging
- [ ] GC fiber: reclaim Staging entries when all consumers caught up
- [ ] Heartbeat fiber

### Phase E: Crash recovery
- [ ] Heartbeat timeout detection
- [ ] Force-release stuck LFM locks
- [ ] OpLog replay (redo committed, rollback in-progress)

### Phase F: Benchmarks & tuning
- [ ] Port micro-benchmarks (latency / throughput)
- [ ] Port YCSB workloads A-D
- [ ] Compare with FUSEE RDMA baseline
- [ ] Tune Staging Buffer size, replication poll interval

## R2.8 Open Questions (Revised)

- [ ] Bucket lock granularity: per-bucket vs per-bucket-pair (4-bucket lock for INSERT)?
- [ ] Should SEARCH always check CXL value (linearizable) or rely on lazy refresh (eventual)?
- [ ] Replication fiber poll interval (tradeoff: read miss latency vs CPU overhead)
- [ ] Staging Buffer size sufficient for burst write workloads?
- [ ] What if all 4 candidate buckets are locked? (try-lock + back off vs block)
- [ ] How to handle the case where a node is slow (not crashed) and holds a lock too long?

## R2.9 Reference Materials

- **Lock implementations**: `cxl_shm_profiling/locks/lfm_lock.c`
- **Cacheline primitives**: `cxl_shm_profiling/common.h`
- **Step-by-step diagrams**: `docs/cxl_lfm_consensus_steps.pptx` (7 slides)
- **LFM internal walkthrough**: `docs/lfm_substeps.pptx` (8 slides)
- **CXL Staging/OpLog layout**: `docs/cxl_staging_oplog.pptx` (4 slides)
- **Hashtable architecture**: `docs/hashtable_architecture.pptx` (5 slides)
- **3-way design comparison**: `docs/consensus_comparison.pptx` (1 slide)
- **Lock algorithm comparison**: `cxl_shm_profiling/docs/lock_comparison.pptx`
