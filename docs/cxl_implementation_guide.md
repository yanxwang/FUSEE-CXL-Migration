# CXL-FUSEE 实现指南

> 从验证过的 mini-bench (`ycsb_abc_bench.c`) 到完整 FUSEE 替换的工作路径

## 关键文件映射

### Mini-bench (已验证)
`cxl_shm_profiling/bench/ycsb_abc_bench.c`

### 完整 FUSEE 集成 (待实现)

| Mini-bench 对应部分 | 在 FUSEE 中对应文件 |
|---|---|
| `BucketLockEntry` 定义 | `src/hashtable.h` (扩展 RaceHashBucket) |
| `kv_insert_A/B/C()` | `src/client.cc` 中 kv_insert 等的替换 |
| `scan_bucket_for_empty()` | `src/client.cc:find_empty_slot()` |
| `store_kv_local()` | `src/client_mm.cc:mm_alloc()` + local DRAM write |
| `shm_mutex_t` lock | 替代 RDMA CAS |
| `CACHELINE_STORE/LOAD` | 替代 `nm_rdma_write/read` |
| `replicator_thread` | 新增 `src/cxl_replication.cc` |
| YCSB workload loader | 复用 `ycsb-test/ycsb_wl_loader.cc` |

## 改动策略

**策略 A: 从 mini-bench 生长**
优势：代码干净、有实测验证
劣势：不能复用 FUSEE 既有的 crash recovery / YCSB 框架

**策略 B: 原 FUSEE 切换 RDMA → CXL**
优势：复用 YCSB/benchmark 框架、crash recovery、RACE hash
劣势：需要删除大量 RDMA 代码，重构复杂

推荐**策略 B + 参考 mini-bench**.

## 重构步骤（细到可落手）

### Phase 1: CXL 基础设施

1. 创建 `src/cxl_mm.h/cc`:
   ```c
   struct CXLRegion {
       void *base;
       size_t size;
       int fd;
   };
   CXLRegion *cxl_map(const char *dev_path, size_t size);
   void cxl_unmap(CXLRegion *r);
   void *cxl_alloc_aligned(CXLRegion *r, size_t size, size_t align);
   ```

2. 创建 `src/cxl_bucket_lock.h/cc`:
   ```c
   struct BucketLockEntry {
       shm_mutex_t lock;
       RaceHashBucket bucket;
       cacheline_u64 write_epoch;    // Option C
       cacheline_u64 pending_op;     // Option A
       cacheline_u64 pending_slot;
       cacheline_u64 pending_new_value;
       cacheline_u64 ack[MAX_HOST_NUM];
   };
   ```
   搬用 ycsb_abc_bench.c 的结构定义。

3. 加 `cxl_shm_profiling` 作为外部依赖（git submodule 或 vendored）：
   ```bash
   cd FUSEE/
   git submodule add <url> external/cxl_shm_profiling
   ```

4. 更新 `CMakeLists.txt`:
   ```cmake
   # 删除：
   target_link_libraries(... ibverbs ...)
   # 添加：
   add_subdirectory(external/cxl_shm_profiling)
   target_link_libraries(... global_allocator ...)
   # 重要：去掉 -mclflushopt if CPU doesn't support
   ```

### Phase 2: KV Operations

1. 重写 `src/client.cc`:
   - 删除：`nm_rdma_write/read/cas` 调用
   - 替换：`shm_mutex_lock` + `CACHELINE_STORE/LOAD`

2. 提供编译期 A/B/C 选择：
   ```c
   // build with:
   //   make CONSENSUS_OPT=A   (sync replication)
   //   make CONSENSUS_OPT=B   (eager push)
   //   make CONSENSUS_OPT=C   (lazy RC, default)
   
   #ifdef CONSENSUS_A
     #include "cxl_kv_ops_A.c"
   #elif defined(CONSENSUS_B)
     #include "cxl_kv_ops_B.c"
   #else
     #include "cxl_kv_ops_C.c"
   #endif
   ```

3. KV ops 模板参见 `ycsb_abc_bench.c` 的 `kv_insert_A/B/C`.

### Phase 3: Per-node KV storage + OpLog

1. 每个 node 本地 DRAM 装 KV data：
   - 改 `src/client_mm.cc`: `mm_alloc` 只在本地分配
   - 加 staging buffer export to CXL for other nodes to pull

2. OpLog 放 CXL:
   - 每个 node 一个 ring buffer (per-node single-producer)
   - Entry 格式见 `docs/consensus_transformation_explained.md` 第七部分

### Phase 4: Background fibers

Boost.Fiber 配合：
1. **Replication fiber** (A/B only): 见 `replicator_thread` in ycsb_abc_bench.c
2. **Heartbeat fiber**: 周期更新自己 epoch
3. **GC fiber**: Option C 本地 cache LRU

### Phase 5: Crash recovery

参考 `docs/consensus_transformation_explained.md` 第九部分：
1. Heartbeat timeout 检测
2. 强制释放 LFM 锁
3. OpLog 扫描 + redo/rollback

### Phase 6: 测试

复用 `tests/` 目录下的 GTest（去掉依赖 RDMA 的部分）+ `ycsb-test/` 下的 YCSB runner。

## 关键参考文档

| 文件 | 作用 |
|---|---|
| `docs/consensus_transformation_explained.md` | 完整设计 + RC theory + 实验数据（11 部分） |
| `docs/cxl_architecture_plan.md` | Phased roadmap + REVISION 2 LFM section |
| `docs/cxl_abc_results.md` | 本次 g3 实测结果 + CXL 延迟基线 |
| `docs/implementation_log.md` | 本次 session 时间轴 |
| `docs/hashtable_architecture.pptx` | Bucket/slot 结构图 |
| `docs/cxl_lfm_consensus_steps.pptx` | LFM-based KV op 7-step 流程 |
| `docs/rc_discussion.pptx` | Eager vs Lazy RC 对比 |

## 检查清单（开始实现前）

- [ ] 阅读 `consensus_transformation_explained.md` 第 5-10 部分（LFM 设计完整描述）
- [ ] 阅读 `cxl_shm_profiling/bench/ycsb_abc_bench.c` 代码（真实可编译可跑的 A/B/C 实现）
- [ ] 了解 `cxl_shm_profiling/locks/lfm_lock.c` 的 mutex 语义
- [ ] 准备好 dev 机器上有 CXL memory 和 `/dev/dax0.X` 设备
- [ ] 机器 CPU 支持 `clflushopt`（近代 Intel/AMD 都支持）

## 预计工作量

| 阶段 | 估计工作量 | 关键产物 |
|---|---|---|
| Phase 1 (基建) | 2-3 天 | cxl_mm.{h,cc}, cxl_bucket_lock.{h,cc} |
| Phase 2 (KV ops) | 3-5 天 | client.cc 重写 |
| Phase 3 (KV/OpLog) | 3 天 | client_mm.cc + 新增 cxl_oplog.cc |
| Phase 4 (fibers) | 2 天 | cxl_replication.cc |
| Phase 5 (recovery) | 3-5 天 | cxl_recovery.cc + tests |
| Phase 6 (测试) | 3 天 | 单机/双机功能 + YCSB |
| **总计** | **2-3 周** | 一个工程师全职 |
