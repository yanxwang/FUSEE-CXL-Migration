# iter-10A constraint document — TLS cache + lock-free shared cache_pool + sender batching exploration + 5-workload path_decomp

**Author**: Claude (per user instruction 2026-05-10, post iter-9A redo completion)
**Date drafted**: 2026-05-10
**Deadline**: **不设硬 deadline** (per user 2026-05-10: "时间充裕。所有 phase 都要按照计划仔细展开推进，不允许出现 drop，descale 等问题")
**Branch**: `feat/cxl-migration`
**Predecessor**: `docs/iters/iter9A_redo_summary_20260510.md` (iter-9A redo COMPLETE 7/7 hard constraints, 12 commits)
**Spec refs**: `docs/design_goals.md §I-XIII` (esp. §I3 cache_pool, §I9 strict-A linearizability, §I11 N:1:1:N), `docs/path_decomp_spec.md`, `docs/scaling_ycsb_spec.md`, `docs/protocol_a_architecture_blueprint.md`
**CLAUDE.md cautionary precedents**: #1 (iter-2A pre-flight descope), #2 (iter-6A scope creep + outlier dismissal), **#3 (iter-9A silent minimal-version substitution)** — `Phase delivery audit gate` MUST appear in iter-10A summary

---

## TL;DR

iter-9A redo Phase 3 path_decomp 把 workload-A KV=1024 T=64 cache=on 的 9.802 Mops/s 拆解成两个 dominant cost:

- **W10 = 3.97 µs** = `cache_pool_insert` 的 per-bucket spinlock contention on Zipf hot key (4× over Expected)
- **R1 = 7.16 µs** = `cache_pool_lookup` 的 MESI cacheline ping-pong on 1024-B inline `value_bytes` 在 hot key entry 上 (~70× over ideal)

两者**同根** — Zipf same-key contention — **不同微架构呈现** — 一个走 lock，一个走 lock-free 但 data layout
触发 cross-core fetch。

iter-10A 用 4 个相互正交的阶段一次性把这两个 + 顺手把 message-passing 路径打平：

1. **Phase 1 — TLS cache layer** (per-worker private hot-key cache)
   - 顶在 shared `KvCachePool` 之上，hot key 命中 0 cross-core
   - 解 R1 MESI bouncing 的 dominant cost
   - 目标：workload-A T=64 → ≥ 20 Mops/s
2. **Phase 2 — Lock-free CAS shared cache_pool** (replace per-bucket spinlock)
   - TLS miss 时 fall through 到 shared cache_pool；shared 上的 W10 contention 也消掉
   - 目标：workload-A T=64 → ≥ 30 Mops/s
3. **Phase 3 — Sender batching exploration** (3 policies + direct-path baseline)
   - P1 fixed K + timeout, P2 adaptive drain-all, P3 per-destination
   - vs direct-path (T-worker MPSC `fetch_add` baseline = 当前 default)
   - 找出 throughput 最高的 message-passing 模型 + 把 winner 设为 default
4. **Phase 4 — 5-workload path_decomp** (best + worst per workload = 10 cells)
   - 在新架构 (TLS + CAS + best batch policy) 上测每个 workload 的 best 和 worst cell
   - 找出每个 workload 的 dominant bottleneck + 具体 CPU 时间归因
   - **C6 expanded**: 5 workload × 2 cell × 14 stage = 140 stage-row 全覆盖
5. **Phase 5 — Full 210-cell sweep + summary + iter-11A backlog**

**核心约束**：
- 所有 phase 完整执行，**不允许 descope**（CLAUDE.md precedent #1/2/3 全适用）
- TLS cache 跟 shared cache_pool 用 epoch-based invalidation 保持 §I9 strict-A 不变性
- 3 batch policy + direct baseline 都要过 G1 hash-diff
- 5-workload path_decomp 每个 workload 的 best 跟 worst cell 都做（spec §13 "best 显示什么是健康，worst 显示什么是 hot"）
- TLS cache size 走 sweep（**256 / 512 / 1024 / 2048 / 4096 / 8192**），找 sweet spot

---

## Scope

### In scope (4 个 phase 全 mandatory)

- TLS cache layer (per-worker private storage + epoch-based invalidation)
- TLS size sweep: 256 → 8192 entries per worker
- Lock-free CAS-based shared `cache_pool_insert` / `cache_pool_evict`
- Sender batching: 3 policies (P1 fixed-K+timeout, P2 adaptive drain-all, P3 per-destination grouped)
- 4-build A/B perf compare on workload-A T=64 cache=on KV=1024
- 5-workload × 2-cell path_decomp (10 cells: best + worst per workload from sweep)
- Full 210-cell sweep on the post-Phase-3 architecture
- Living docs (C7) 实时更新

### Out of scope (defer to iter-11A+)

- **Forwarder-pool-direct + cross-host pool generation**（iter-9A redo backlog #3, recovers read-path -37% regression）— 单独 iter-11A first task
- Hot-bucket sharding within owner（iter-5C 长遗留, iter-9A redo backlog #6）
- Variable-length keys（iter-9A redo backlog #7）
- BucketLockTable removal (-2.5 GB CXL, iter-7A backlog)
- Living-docs full §II.3-II.6 narrative rewrite (iter-9A redo carried as amendment)
- Per-entry lock as alternative to lock-free CAS (was option A in cache_pool 5-方案 analysis — 跟 lock-free CAS 互斥, 选 CAS 更彻底)
- Hot-key replication in shared cache_pool (option D, 跟 TLS cache 冗余)
- RCU + seqlock as alternative (option E, lock-free CAS 是 superset)

### Memo for future

- TLS cache 配 epoch invalidation 后, `cache_pool_set_stale`（来自 InvalReceiver）需要同时 bump shared epoch counter — TLS readers 下次 access 检测 epoch 不匹配视为 miss，重新走 shared / CXL fetch
- iter-11A first task 是 forwarder-pool-direct，跟 read-path regression 相关；iter-10A 的 TLS cache 应该把 read-path 性能也带上去，部分缓解但不完全

---

## Hard constraints (这些违反 = iter 重做 — 同 iter-9A C1-C7 + iter-10A 新增 C8-C10)

| 约束 | 验证手段 |
|---|---|
| **C1-C7** | 同 iter-9A redo（全部继承且必须保持）：变长 KV、no value bytes on message ring、全线程 CPU pin、N:1:1:N runtime active assert、G1 hash-diff at 全 KV×workload、path_decomp 全跑、living docs realtime |
| **C8** TLS cache 跟 shared cache_pool 强一致 (§I9 strict-A 不变) | (a) `cache_pool_set_stale` 每次调用 bump shared `cache_epoch[bucket]` 计数；(b) TLS entry 存 `(key, value, observed_epoch)`；(c) TLS lookup 命中后 reload `cache_epoch[bucket]` 比较, 不匹配则 evict TLS 条目 + fall through。G1 hash-diff battery 在 Phase 1 后 + Phase 2 后 + Phase 3 winner 后**各跑一次**, 任一不 PASS = iter 重做 |
| **C9** 3 batch policy + direct baseline 4 build 都过 G1 hash-diff at 全 KV × workload (= 80 cells per build × 4 build = 320 cells) | Phase 3.B 强制：每个 build 各跑 batter 一次，4 × 20 PASS。任一 build FAIL = batch policy invalid, 找 root cause |
| **C10** 5-workload path_decomp = 10 cell, 每 cell 14 stage 全覆盖, 每 cell 0 unjustified `✱ no data` | Phase 4：5 workload × 2 cell (best + worst) × 14 stage = 140 stage-row table；any unjustified `✱ no data` 行 = Phase 4 不 exit |
| **C11** TLS cache size sweep 至少跑 6 个 size (256/512/1024/2048/4096/8192) on workload-A T=64 cache=on KV=1024 | Phase 1.E 强制：6 size × 5 rep = 30 runs, 找出 hit rate vs latency 曲线 + sweet spot |

---

## Living docs to update (per C7, 实时随每 sub-phase 同 commit 落地)

| 文档 | 改的章节 | 触发 phase |
|---|---|---|
| `docs/protocol_a_architecture_blueprint.md` | Part I.2/I.3 加 TLS cache layer | Phase 1.A 完 |
| `docs/protocol_a_architecture_blueprint.md` | Part II §II.2 (read path R1-R6) 加 TLS hit / TLS miss / cache_pool fall-through 三级路径 | Phase 1.B wire 完 |
| `docs/protocol_a_architecture_blueprint.md` | Part II §II.1 (write path W1-W12) W11 (cache update) 加 TLS invalidate broadcast 描述 | Phase 1.B wire 完 |
| `docs/protocol_a_architecture_blueprint.md` | Part II §II.3-II.6 narrative full rewrite (iter-9A redo carried) | Phase 5 一次性补 |
| `docs/protocol_a_architecture_blueprint.md` | Part III.1 cheat sheet：加 TLS region per host 物理布局 | Phase 1.A 完 |
| `docs/design_goals.md` | §I3 + §I4 加 TLS cache invariant：per-worker hot key cache, epoch-validated, MUST be coherent with shared cache_pool | Phase 1.A 完 |
| `docs/design_goals.md` | §VI-A.bis 加 lock-free CAS hashmap implementation pattern | Phase 2.A 完 |
| `docs/design_goals.md` | §X H6 加 sender batching policy invariant (winner 是 default) | Phase 3.D 完 |
| `docs/scaling_ycsb_spec.md` | §3 加 TLS cache 测试参数 (FUSEE_TLS_SIZE env) | Phase 1.D 完 |
| `docs/path_decomp_spec.md` | §11 reference instances 加 iter-10A 5-workload decomp 输出条目 | Phase 4 完 |
| `CLAUDE.md` | 加新 cautionary precedent (if iter-10A 暴露 anything novel) | Phase 5 |
| `docs/fusee_cxl_progress.md` | iter-10A 进度行 | 每 phase 完 |

强制规则：每个 commit staged file 列表必须包含上面表格中**触发 phase 匹配**的 doc 改动 (per iter-9A C7)。Phase 5 不补遗 — 只做最终一致性 review。

---

## Phase 0 — pre-flight

**目的**：确认 iter-9A redo 末状态健康可重建。

**Steps**:
1. `git status` clean，rebase 到 main
2. `bootstrap_slave.sh g3 g4` 重建 g3/g4 build
3. 跑 quick smoke: `protocol_a_ycsb workload-d kv=8 T=4 cache=on` 健康 (target: ≥ 1.5 Mops/s, iter-9A redo baseline 1.85)
4. confirm perf + bpftrace + bpfcc-tools 仍可用
5. confirm `-DFUSEE_PROBE=1` 编译路径仍可激活 (iter-9A redo CMakeLists 修过)
6. 跑 path_decomp Phase 0 µbench baseline → 落 `docs/path_decomp_iter10A_pre_<ts>/baseline.md`

**Exit**: smoke PASS + baseline.md 保存 + 3 个 build 后续可加 (TLS / CAS / 4 batch builds)

---

## Phase 1 — TLS cache layer (per-worker private hot-key cache)

**目的**：解 R1 7.16 µs MESI bouncing 的 dominant cost。Per-worker 私有 storage 使 hot key 在每个 worker 的 L1/L2 cache 命中, 0 cross-core fetch。

### 1.A — TLS cache 数据结构

新建 `src/cxl_tls_cache.h`:

```cpp
constexpr int kTlsMaxEntries = 8192;  // env-tunable, swept

struct alignas(64) TlsCacheEntry {
  uint64_t key;                                // 0 = empty slot
  uint64_t observed_cache_epoch;               // for invalidation check
  uint32_t value_size;
  uint32_t _pad;
  uint8_t  value_bytes[kCacheValueMaxBytes];  // 1024 B inline
  // total per entry = 8 + 8 + 4 + 4 + 1024 = 1048 B
};

// One TLS region per worker thread (NOT shared, NOT mmap'd between workers).
// Allocated post-fork via posix_memalign(64, ...) so its base lives in
// THIS worker's anonymous VMA only. No cross-worker MESI traffic.
struct TlsCache {
  uint32_t num_entries;            // env-driven; swept 256..8192
  uint32_t mask;                   // num_entries - 1 (must be power of 2)
  TlsCacheEntry *entries;          // posix_memalign'd 64-aligned
  // LRU stub: linear probe + replace oldest on insert
};
```

**Per-worker memory**: kTlsMaxEntries × 1048 B (max 8192 × 1048 = 8.6 MB / worker; min 256 × 1048 = 268 KB / worker). At T=64 worst case: 64 × 8.6 MB = 550 MB DRAM 总。

### 1.B — Epoch-based invalidation (C8 hard requirement)

Add to `KvCachePool` (shared):
```cpp
struct alignas(64) CacheEpoch {
  std::atomic<uint64_t> epoch;     // bump on any insert/evict/set_stale
};
struct KvCachePool {
  // ... existing fields ...
  CacheEpoch *bucket_epoch;        // num_buckets × 64 B in DRAM
};
```

**Coherence rule**:
- `cache_pool_insert(key, ...)`: bump `bucket_epoch[hash(key)]`. ANY worker's TLS entry observing OLD epoch knows its copy is stale.
- `cache_pool_evict(key)`: bump same.
- `cache_pool_set_stale(key)` (from InvalReceiver, peer-host invalidate): bump same.
- TLS lookup of key X:
  1. find TLS entry by `key`
  2. if found, reload `bucket_epoch[hash(X)]` (single atomic load)
  3. if `epoch == entry.observed_cache_epoch` → TLS HIT, return value (0 cross-core except 1 atomic load on epoch)
  4. else → TLS MISS, fall through to shared `cache_pool_lookup`, on hit re-cache TLS with current epoch

**Why this preserves §I9 strict-A**: any cross-host invalidate or local CoW write bumps the epoch. TLS reader sees stale epoch on next lookup → forced re-fetch from shared cache_pool / CXL → linearizability preserved.

**Cache_epoch cacheline contention**: bucket_epoch[B] is per-bucket atomic. Hot key K's bucket has 32 workers reading + writers bumping. The atomic load itself is contended (cacheline ping-pong), but it's 1 cacheline, not 17 — 16× reduction vs current MESI cost.

Alternatively, single global `cache_epoch` (1 atomic for all keys) — coarser invalidation (any insert evicts all TLS), simpler, much less memory, possibly faster. Phase 1.B compare both designs.

### 1.C — Wire into search() / execute_write_local()

`tests/protocol_a_ycsb.cc` per-worker post-fork:
```cpp
TlsCache tls;
tls_cache_init(&tls, getenv("FUSEE_TLS_SIZE") ?: "1024");
CxlKvStoreA::set_thread_tls_cache(&tls);
```

`src/cxl_kv_ops_A.cc::search()`:
```cpp
// NEW: TLS L1 lookup first
if (g_tls_cache) {
  if (tls_lookup(g_tls_cache, key, out_buf, buf_len, out_len)) {
    PROBE_OP("R0_tls_hit", key);
    return 0;
  }
}
// EXISTING: shared cache_pool L2
PROBE_OP("R1", key);
if (cache_pool_lookup(...)) {
  // populate TLS L1 with current epoch
  if (g_tls_cache) tls_insert(g_tls_cache, key, buf, sz, current_epoch);
  ...
}
// EXISTING: CXL L3 fetch
```

`execute_write_local()` Step 7 (after CoW publish):
```cpp
cache_pool_insert(...);  // bumps epoch
if (g_tls_cache) tls_insert(g_tls_cache, key, value, value_len, new_epoch);
```

### 1.D — G1 hash-diff verify (C5/C8 enforce)

```bash
scripts/iter10A_hash_diff_battery.sh docs/hash_diff_iter10A_phase1_<ts>/
```
Expected: 20/20 PASS (5 workload × 4 KV size).

### 1.E — TLS size sweep (C11 enforce)

`scripts/iter10A_tls_size_sweep.sh`:
```
for size in 256 512 1024 2048 4096 8192; do
  for rep in 1..5; do
    FUSEE_TLS_SIZE=$size run workload-A T=64 cache=on KV=1024
  done
done
```

Target: identify sweet spot (highest Mops/s with reasonable memory). Likely 1024-2048 (coverage of Zipf top + L2 cache fit).

### Phase 1 Exit Criteria

- [ ] G1 hash-diff PASS 20/20
- [ ] TLS size sweep 6 × 5 = 30 runs done; sweet spot identified + documented
- [ ] workload-A KV=1024 T=64 cache=on healthy ≥ **20 Mops/s** (target; was 9.8 pre-Phase-1)
- [ ] **C7 living doc**: blueprint Part I.2/I.3 + Part II §II.2 + design_goals §I3/§I4 同 commit 落地
- [ ] Per-stage decomp R1 stage 从 7.16 µs → 验证 ≤ 1 µs (TLS hit case)

**预期 LOC**: ~250 (TlsCache impl ~150 + epoch wire ~50 + test runner ~30 + sweep script ~30)

---

## Phase 2 — Lock-free CAS shared cache_pool

**目的**：解 W10 3.97 µs spinlock contention。TLS miss 时 fall through 到 shared，shared 上不能再有 4 µs spinlock cost。

### 2.A — CAS-based cache_pool_insert / evict

Replace per-bucket `pthread_spin_lock` with **versioned 16-byte CAS** on entry.key:

```cpp
struct VersionedKey {
  uint64_t key;
  uint64_t version;  // bump on every CAS update; even = published, odd = reserved (in-progress)
};

struct alignas(64) KvCacheEntry {
  std::atomic<VersionedKey> key128;   // CMPXCHG16B atomic
  std::atomic<uint8_t> stale;
  uint32_t value_size;
  std::atomic<uint64_t> lru_epoch;
  uint8_t value_bytes[1024];
  // ... cacheline align ...
};
```

`cache_pool_insert(key X)`:
```cpp
retry:
  // Phase 1: scan for match
  for entry in bucket {
    snap = entry.key128.load(acquire)
    if snap.key == X {
      reserved = {snap.key, snap.version | 1}  // odd = reserved
      if !entry.key128.cas(snap, reserved) goto retry
      // update payload, then publish:
      entry.value_size = ...
      memcpy(entry.value_bytes, ...)
      entry.lru_epoch.store(...)
      entry.stale.store(0, release)
      published = {X, snap.version + 2}  // even = published, version bumped
      entry.key128.store(published, release)
      bump bucket_epoch (for TLS)
      return 0
    }
  }

  // Phase 2: scan for empty/tomb
  for entry in bucket {
    snap = entry.key128.load(acquire)
    if snap.key == EMPTY || snap.key == TOMB {
      reserved = {RESERVED, snap.version + 1}
      if !entry.key128.cas(snap, reserved) goto retry
      // update payload + publish (same as Phase 1)
    }
  }

  // Phase 3: scan for LRU min
  ... 同 Phase 2 with LRU select ...

  // Phase 4: bucket full - return -1 (caller handles)
```

`cache_pool_lookup` (already lock-free) needs to handle RESERVED entries:
```cpp
snap = entry.key128.load(acquire)
if snap.version & 1 != 0 return MISS  // entry mid-update, treat as miss
if snap.key != target_key continue
if entry.stale.load(acquire) return MISS
// safe to read value bytes
memcpy(out, entry.value_bytes, entry.value_size)
re-check entry.key128 — if version changed mid-read, retry or treat as miss
```

`cache_pool_evict`: same CAS pattern, but writes RESERVED → TOMB key.

### 2.B — G1 hash-diff verify (C5/C8/C9 enforce)

20/20 PASS on lock-free build (with TLS cache enabled, hit miss-path).

### 2.C — workload-A T=64 cache=on KV=1024 smoke

Target: ≥ **30 Mops/s** (iter-9A redo 9.8 → Phase 1 TLS ~25 → Phase 2 CAS ~30+).

### Phase 2 Exit Criteria

- [ ] G1 hash-diff PASS 20/20 with TLS+CAS active
- [ ] workload-A T=64 cache=on KV=1024 healthy ≥ 30 Mops/s
- [ ] Per-stage W10 ≤ 0.3 µs (was 3.97)
- [ ] **C7 living doc**: design_goals §VI-A.bis 加 lock-free CAS pattern; blueprint §I.3 cache_pool 行 update

**预期 LOC**: ~300 (CAS rewrite + version handling + retry + lookup mid-update edge case)

---

## Phase 3 — Sender batching exploration (3 policies + direct baseline)

**目的**：Phase 2.C iter-9A redo 用 single-sender unbatched, 在高 T 下 0.5 Mops/s 反而是新瓶颈。Phase 3 找出哪种 batch policy 最优, 把 winner 设为 default。

### 3.A — Policy implementations

每个 policy 是一个独立 build (CMake flag), 使能 sender 但批量策略不同。Direct-path baseline 仍是 `FUSEE_USE_AGGREGATOR=0` (current default)。

#### Baseline B0: Direct (T-worker MPSC)
- 当前 default
- Worker 直接 CXL atomic `WriteRing.tail.fetch_add(1)`
- T 个 worker 在同一 atomic 上 contention

#### Policy P1: Fixed K + timeout
```cpp
constexpr int kBatchK = 16;          // configurable via env
constexpr int kBatchTimeoutUs = 100;  // configurable via env

void write_sender_loop_p1() {
  WriteAggrEntry batch[kBatchK];
  while (!stop) {
    int n = 0;
    timespec t_start; clock_gettime(CLOCK_MONOTONIC, &t_start);
    while (n < kBatchK) {
      n += scan_pending_into_batch(&batch[n], kBatchK - n);
      if (n == kBatchK) break;
      if (elapsed_us(t_start) >= kBatchTimeoutUs) break;
      __builtin_ia32_pause();
    }
    if (n == 0) continue;
    // single fetch_add(N) reserves N consecutive ring slots
    uint64_t base = ring_tail.fetch_add(n, release);
    flush_line(&ring_tail); sfence;
    // fill N entries + single sfence at end
    for (int i = 0; i < n; i++) fill_entry(base + i, &batch[i]);
    flush_region(entries[base..base+n]);
    sfence;
    // spin on N resp_op_ids; flip ack as each comes back
    spin_acks_p1(batch, n);
  }
}
```

Tunable: K ∈ {1, 4, 8, 16, 32}; T_us ∈ {10, 50, 100, 500}.

#### Policy P2: Adaptive drain-all (no timeout, no K cap)
```cpp
void write_sender_loop_p2() {
  WriteAggrEntry batch[kMaxAggrWorkers];  // up to T pending
  while (!stop) {
    int n = scan_pending_into_batch(batch, kMaxAggrWorkers);
    if (n == 0) { __builtin_ia32_pause(); continue; }
    uint64_t base = ring_tail.fetch_add(n, release);
    flush_line(&ring_tail); sfence;
    for (int i = 0; i < n; i++) fill_entry(base + i, &batch[i]);
    flush_region(entries[base..base+n]);
    sfence;
    spin_acks_p2(batch, n);
  }
}
```

Self-tuning: 低负载 n=1 (跟 baseline 几乎一样); 高负载 n→T (最大化 batch 收益)。

#### Policy P3: Per-destination grouped batching
```cpp
void write_sender_loop_p3() {
  WriteAggrEntry batch_per_dst[kMaxHosts][kAggrSize];
  int n_per_dst[kMaxHosts] = {0};
  while (!stop) {
    // Phase 1: scan all worker slots, group by dst_host
    for (int w = 0; w < num_workers; w++) {
      AggrSlot *s = aggr->slots[Write][w];
      if (s->state.load(acquire) != kAggrPending) continue;
      int dst = s->dst_host;
      batch_per_dst[dst][n_per_dst[dst]++] = snapshot(s);
    }
    // Phase 2: per dst, single fetch_add(n_per_dst[dst]) on per-dst ring
    for (int dst = 0; dst < num_hosts; dst++) {
      if (n_per_dst[dst] == 0) continue;
      WriteRing *ring = &wr->rings[me][dst];
      uint64_t base = ring->tail.fetch_add(n_per_dst[dst], release);
      flush_line(&ring->tail); sfence;
      for (int i = 0; i < n_per_dst[dst]; i++) fill_entry(...);
      flush_region(...);
      sfence;
      spin_acks(batch_per_dst[dst], n_per_dst[dst]);
      n_per_dst[dst] = 0;
    }
  }
}
```

每 dst 独立 fetch_add, dst 之间不会 interleave 抢同一 ring tail。

### 3.B — Build matrix + G1 hash-diff (C9 enforce)

4 build × 20-cell hash-diff = 80 cells. CMake flag: `-DFUSEE_BATCH_POLICY=NONE/P1/P2/P3` (default NONE = direct).

Each build 必须 PASS 20/20。FAIL = policy invalid, root cause investigate before proceeding.

### 3.C — Per-policy throughput sweep on 1 cell

`scripts/iter10A_batch_policy_sweep.sh`:
- workload-A T=64 cache=on KV=1024, FUSEE_USE_AGGREGATOR=1
- 5 rep per policy
- For P1: also sweep K ∈ {4, 16, 32} × T_us ∈ {50, 100} = 6 configs

Output: `docs/iter10A_batch_compare_<ts>/SUMMARY.md` with table:
```
policy    | K | T_us | mean Mops/s | p99 µs | hash-diff
B0 direct | - | -    | XX          | YY     | PASS
P1 fixed  | 4 | 50   | ...         | ...    | ...
P1 fixed  | 4 | 100  | ...         | ...    | ...
P1 fixed  |16 | 50   | ...         | ...    | ...
P1 fixed  |16 | 100  | ...         | ...    | ...
P1 fixed  |32 | 100  | ...         | ...    | ...
P2 adapt  | - | -    | ...         | ...    | ...
P3 perdst | - | -    | ...         | ...    | ...
```

### 3.D — Name winner + set as default

Winner = highest mean Mops/s + p99 ≤ 50 ms. Set as compile-time default:
```cmake
set(FUSEE_BATCH_POLICY "P2" CACHE STRING "Default sender batch policy")  # update with winner
```

Update `FUSEE_USE_AGGREGATOR` default → ON (winner enables it by default since unbatched cost solved).

### Phase 3 Exit Criteria

- [ ] 4 build (B0 + P1 + P2 + P3) all compile + G1 hash-diff PASS 20/20 each (= 80 cells total)
- [ ] Per-policy sweep complete; 1 winner named
- [ ] Winner set as compile-time + runtime default
- [ ] workload-A T=64 cache=on KV=1024 ≥ winner's Mops/s (and ≥ baseline B0)
- [ ] **C7 living doc**: design_goals §X H6 加 winner as policy + blueprint §I.2 sender 行 update

**预期 LOC**: ~400 (3 sender variants + spin_acks for batch + cmake conditionals + sweep script + analysis script)

---

## Phase 4 — 5-workload × 2-cell path_decomp (best + worst per workload)

**目的**：找出**每个 workload** 的 dominant bottleneck + 具体 CPU 时间归因。iter-9A redo Phase 3 只做了 workload-A (best cell). iter-10A 把另外 4 个 workload 补上, 同时每个 workload 加 worst cell 看异常。

### 4.A — Cell selection (per user direction "best + worst per workload")

Use Phase 5 sweep results 选 cell:

| Workload | Best cell (max Mops/s) | Worst cell (min Mops/s, 排除 anomaly carve-out) |
|----------|------------------------|--------|
| a | TBD (likely T=64 cache=off kv=512) | TBD (likely T=1 / T=2) |
| b | TBD | TBD |
| c | TBD | TBD |
| d | TBD | TBD |
| f | TBD | TBD |

Phase 4 在 Phase 5 sweep 之后做（需要 sweep 数据来选 cell）。

### 4.B — Per-cell capture (per `path_decomp_spec.md` Phase 1)

For each of 10 cells:
- Healthy capture (5 try budget, ≥ healthy threshold)
- Anomaly capture (12 try budget, < healthy / 10 threshold; if no anomaly observed, document)

`scripts/iter10A_5wl_pathdecomp.sh` orchestrates all 10 cells × (5 healthy + 12 anomaly) tries.

### 4.C — Per-cell per_stage_decomp.md

Output structure:
```
docs/path_decomp_iter10A_<ts>/
├── baseline.md                         # Phase 0 µbench (shared by all 10)
├── per_cell/
│   ├── workloada_best/per_stage_decomp.md
│   ├── workloada_worst/per_stage_decomp.md
│   ├── workloadb_best/per_stage_decomp.md
│   ├── ...
│   └── workloadf_worst/per_stage_decomp.md
└── consolidated_bottleneck_table.md    # cross-workload summary
```

Each `per_stage_decomp.md` covers **all 14 stages** (W1/W2/W3/W4/W6/W7/W8/W9/W10/W12 + R1/R2hit/R2miss/R3/R4/R6 + I1/I2/I3/I4/I5/I6/I7/I8) — workload-specific stage skips OK if explicitly cited.

### 4.D — Consolidated bottleneck table

`consolidated_bottleneck_table.md` cross-workload comparison:

```
| Workload | Cell type | Headline thpt | Top-1 bottleneck stage | Top-1 µs | Top-2 stage | Top-2 µs | Notes |
|----------|-----------|---------------|------------------------|----------|-------------|----------|-------|
| a | best  | XX Mops/s | (e.g., R1 TLS hit ~0.05) | 0.05 | W10 (CAS retry) | 0.2 | ... |
| a | worst | YY        | ...                       | ...  | ...           | ... | ... |
| b | best  | ...       | ...                       | ...  | ...           | ... | ... |
...
```

Goal: 找出哪些 stage 是 cross-workload 一致 bottleneck (universal fix candidate) vs哪些是 workload-specific (e.g., workload-c read-heavy 看 forward_read pool->read 是 hot；workload-a write-heavy 看 cross-host invalidate 是 hot)。

### Phase 4 Exit Criteria

- [ ] All 10 cells captured (5 wl × 2 cell)
- [ ] Per-cell per_stage_decomp.md exists, 14 stages 全覆盖, 0 unjustified `✱ no data`
- [ ] consolidated_bottleneck_table.md written + named top-3 cross-workload bottlenecks
- [ ] Each named bottleneck → either (a) iter-11A backlog item with measurement evidence OR (b) immediate Phase 4.1 fix (≤ 200 LOC)
- [ ] **C7 living doc**: path_decomp_spec §11 reference instances 加 iter-10A 5-cell entries; blueprint Part III.3 加 cross-workload bottleneck pattern

**预期 LOC**: ~150 (capture script + parser tweaks for cross-cell aggregation)

---

## Phase 5 — Full sweep + summary + iter-11A backlog

### 5.A — Full 210-cell scaling_ycsb sweep on post-Phase-3 architecture

```bash
scripts/iter10A_sweep.sh docs/g34_scaling_ycsb_iter10A_<ts>/
```

5 workload × 7 T × 2 cache × 3 KV = 210 cells × 1 rep (per scaling_ycsb_spec §3 standing default).

Build: TLS cache ON + lock-free CAS + Phase 3 winner batch policy.

### 5.B — Anomaly scan + doubling-ratio gate (per CLAUDE.md 2026-05-03)

`scripts/probe_anomaly_scan.py` on `gap_to_target.md`. Anomaly cells must be either (a) 5-rep verified OR (b) explicit carve-out with iter-11A backlog entry.

Doubling-ratio gate ≥ 1.5× per pre-saturation T-doubling, **per all 5 workloads** (per iter-7A 2026-05-03 update).

### 5.C — iter10A_summary doc

Per `iter9A_redo_summary_20260510.md` template:
- TL;DR
- **Phase delivery audit table** (per CLAUDE.md precedent #3 gate, MANDATORY)
- **Hard constraint compliance audit (C1-C11)**
- Headline measurements (per-workload best, distance to 20 Mops/s, comparison vs iter-9A redo)
- Cross-workload bottleneck table (from Phase 4)
- TLS cache size sweep curve
- Sender batch policy comparison table (from Phase 3.C)
- iter-11A backlog (genuine deferrals only — no relabeled in-scope work per precedent #3)
- Process retrospective

### 5.D — scaling_ycsb_spec.md gate updates

If new bottleneck pattern emerged + worth codifying → add new gate.

### 5.E — iter11A_backlog_memo.md

Genuine deferrals:
- Forwarder-pool-direct (read-path -37% recovery; iter-10A's TLS cache 部分缓解)
- Hot-bucket sharding within owner
- Variable-length keys
- BucketLockTable removal
- Living-docs full §II.3-II.6 narrative rewrite
- Anything new from Phase 4 cross-workload analysis

### Phase 5 Exit Criteria

- [ ] 210-cell sweep done, 0 unexplained FAILs
- [ ] Anomaly scan zero unexplained outliers (carve-out OK with iter-11A entry)
- [ ] Doubling-ratio gate PASS for all 5 workloads
- [ ] iter-11A backlog has only genuine deferrals
- [ ] Phase delivery audit table complete + all 5 phase ✅ FULL (no PARTIAL/NOT-DONE without prior user descope)
- [ ] **C7 living doc**: fusee_cxl_progress.md iter-10A 进度行就位; blueprint snapshot version 推到 "end of iter-10A"

---

## Risks & cautionary precedents

### CLAUDE.md precedent #3 复发风险 (HIGHEST)

iter-9A original silently descoped Phase 2 from "完整 3-ring + staging + 6 named threads" 到 "CPU pin + 2 named threads"。iter-10A 同样有"显然太复杂/可分阶段"的 sub-phase（特别 Phase 3 batch policy compare 跟 Phase 4 5-workload decomp）。**Phase delivery audit table 是这次唯一的护栏** — 任何 ⚠ PARTIAL / ❌ NOT DONE 行没有 user 预批 descope = iter 不 complete。

User 已明确：**"不允许出现 drop, descale 等问题"**。

### TLS cache invalidation race (C8)

TLS reader 看 epoch 是 outdated → re-fetch from shared. 但 between 看 epoch 跟 re-fetch 之间, 又被 invalidate 了 → 还得 retry. 可能 livelock 在高 churn 下。
- 缓解：retry budget (5 次后 fall through 直接 CXL fetch)
- G1 hash-diff battery 是 catch-all 验证

### Lock-free CAS ABA on key field

`key.cas(snap, reserved)` 可能 ABA: 别 thread 把 key X → tomb → X 又写回 → 我看 snap.key=X 还是没变。Version field 防 ABA — version 单调递增。
- 16-byte CMPXCHG16B atomic 必须 supported (g3/g4 Intel Xeon 6787P 都支持, 但要 verify)

### Sender batch policy 引入 latency

P1 fixed K + timeout 在低负载下 worst case 等 timeout (100 µs) 才 flush. 对 trans 长 wall 影响。
- P2 adaptive drain-all 自动避开 (低负载 n=1 立即 flush)
- 如果 P1 winner 但 timeout 暴露 latency 问题, 调小 timeout 至 10 µs

### TLS size 太大爆 DRAM

8192 entry × 1048 B × 64 worker = 550 MB DRAM 在 1 host. g3/g4 各有 96 GB DRAM, 0.5% 不是问题。但要确认 + log 实际 RSS。

---

## Open questions for user — RESOLVED 2026-05-10

| QR | 问题 | User 回复 |
|---|---|---|
| QR1 | 3 batch policies | **P1 (fixed K+timeout) + P2 (adaptive drain-all) + P3 (per-destination grouped)** vs B0 direct baseline = 4 builds |
| QR2 | 5 workload decomp cell | **best + worst per workload = 10 cells** (covers both healthy + hot pattern) |
| QR3 | TLS cache size | **sweep 256 → 512 → 1024 → 2048 → 4096 → 8192**, 找 sweet spot；可以再加大 if 8192 仍未饱和 |
| QR4 | Deadline | **不设硬 deadline, 时间充裕。所有 phase 完整推进, 不允许 drop/descale** (CLAUDE.md precedent #3 全适用) |

---

## Phase-by-phase commit prefix convention

| Phase | Commit prefix |
|---|---|
| Phase 0 | `[iter10A-preflight]` |
| Phase 1 | `[iter10A-tls]` |
| Phase 2 | `[iter10A-cas]` |
| Phase 3 | `[iter10A-batch]` |
| Phase 4 | `[iter10A-decomp]` |
| Phase 5 | `[iter10A-sweep]` / `[iter10A-summary]` |

每个 commit 必须 reference G6/AP16 invariants per `scripts/git-hooks/pre-commit` H4 hook (TLS cache + CAS hashmap 都 touch protected globs)。

---

## Estimated commit log shape

```
[iter10A-plan][G6][AP16] iter-10A constraint draft
[iter10A-preflight][G6] Phase 0 baseline + smoke
[iter10A-tls][G6][I3][AP16] Phase 1.A-B TLS cache + epoch invalidation
[iter10A-tls][G6][I3] Phase 1.C wire into search/execute_write_local
[iter10A-tls][G6][I9] Phase 1.D G1 hash-diff 20/20 PASS
[iter10A-tls][G6] Phase 1.E TLS size sweep 256..8192
[iter10A-cas][G6][I9][AP16] Phase 2.A lock-free CAS cache_pool_insert/evict
[iter10A-cas][G6][I9] Phase 2.B G1 hash-diff PASS
[iter10A-cas][G6] Phase 2.C workload-A T=64 ≥ 30 Mops/s verify
[iter10A-batch][G6][I11] Phase 3.A 3 sender policies + 1 baseline
[iter10A-batch][G6][I9] Phase 3.B 4-build hash-diff 80/80 PASS
[iter10A-batch][G6] Phase 3.C per-policy sweep + winner named
[iter10A-batch][G6] Phase 3.D winner set as default + cmake update
[iter10A-decomp][G6] Phase 4 5-workload × 2-cell path_decomp captures
[iter10A-decomp][G6] Phase 4 consolidated bottleneck table
[iter10A-sweep][G6] Phase 5 full 210-cell sweep
[iter10A-summary][G6][AP16] iter-10A complete summary + iter-11A backlog
```

Estimated 17 commits.
