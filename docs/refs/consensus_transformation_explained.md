# Consensus Transformation: RDMA → CXL

> 详细解释FUSEE如何从RDMA CAS-based consensus转变为CXL software-based consensus。

---

## 第一部分：FUSEE的RDMA Consensus到底在干什么

### 先理解背景：Hash Table Slot

FUSEE的核心数据结构是一个分布式hash table。每个key对应hash table中的一个**slot（槽位）**。一个slot只有**8个字节**：

```
RaceHashSlot (8 bytes, packed):    // 定义在 src/hashtable.h:36
  fp(1B)        — 指纹（快速匹配用）
  kv_len(1B)    — KV数据占几个subblock
  server_id(1B) — KV数据存在哪个server
  pointer(5B)   — KV数据的远程内存地址
```

**关键点**：修改一个slot就是修改这8个字节。8字节刚好是一个`uint64_t`，所以可以用**一次CAS原子操作**完成。

### FUSEE的系统模型

```
                    Memory Node 0 (Primary)
                    ┌─────────────────────┐
                    │  Hash Table (主副本)   │
 Client A ──RDMA──> │  Slot X: [old_value] │
 Client B ──RDMA──> │  KV Data Area        │
                    └─────────────────────┘

                    Memory Node 1 (Backup)
                    ┌─────────────────────┐
                    │  Hash Table (备份副本)  │
 Client A ──RDMA──> │  Slot X: [old_value] │
 Client B ──RDMA──> │  KV Data Area        │
                    └─────────────────────┘
```

- Hash table有**多个副本**（`num_idx_rep`个），分布在不同的memory node上
- 其中一个是**Primary（主）**，其余是**Backup（备份）**
- 多个Client可能**同时**想修改同一个slot → 需要consensus（共识）

### 什么是CAS？

CAS = Compare-And-Swap（比较并交换）。它的语义是：

```
CAS(地址, 期望值, 新值):
    原子地执行以下操作：
    if 地址上的值 == 期望值:
        地址上的值 = 新值
        return 期望值       // 成功！
    else:
        return 地址上的当前值  // 失败，告诉你现在是什么值
```

**关键特性**：CAS是**原子的**——在RDMA硬件层面保证同一时刻只有一个CAS能修改一个地址。如果两个Client同时对同一个地址做CAS，只有一个会成功。

在代码中，CAS操作通过`IBV_WR_ATOMIC_CMP_AND_SWP`实现（`src/client.cc:891`）：

```cpp
sr->opcode = IBV_WR_ATOMIC_CMP_AND_SWP;
sr->wr.atomic.remote_addr = cas_addr_list[i].r_kv_addr;   // 远程slot地址
sr->wr.atomic.compare_add = cas_addr_list[i].orig_value;  // 期望值
sr->wr.atomic.swap        = cas_addr_list[i].swap_value;  // 新值
```

### FUSEE的INSERT共识协议（完整流程）

假设：2个index副本（`num_idx_rep=2`），即1个Primary + 1个Backup。

**Client A** 想插入 key="hello", value="world"。

对应代码入口是 `Client::kv_insert()`（`src/client.cc:277`）：
```cpp
prepare_request(&ctx);                      // Step 1
kv_insert_read_buckets_and_write_kv(&ctx);  // Step 1
kv_insert_backup_consensus_0(&ctx);         // Step 2
if (num_idx_rep_ > 1) {
    kv_insert_backup_consensus_1(&ctx);     // Step 3
    kv_insert_commit_log(&ctx);             // Step 4
}
kv_insert_cas_primary(&ctx);                // Step 5
```

#### Step 1：写KV数据 + 读Hash Bucket

```
Client A:
  1. 在远程memory上分配一块空间
  2. 通过RDMA WRITE，把 key+value 写到所有replica的memory node上
  3. 通过RDMA READ，读取Primary上的hash bucket（找到目标slot）

  结果：Client A 知道 Slot X 当前值是 0x0（空的）
```

这一步还没有任何竞争，只是准备工作。

#### Step 2：Backup Consensus 0（在备份上抢占）

对应 `kv_insert_backup_consensus_0()`（`src/client.cc:2743`）。

```
Client A 构造：
  old_value = 0x0000000000000000  （当前slot是空的）
  new_value = 0x[fp][len][sid][pointer]  （指向刚写的KV数据）

Client A 通过RDMA CAS发给 Backup Node：
  CAS(Backup的Slot X地址, old_value=0x0, new_value=A的值)
```

**如果只有Client A在操作**：
- Backup上Slot X确实是0x0 → CAS成功 → 返回0x0
- Client A看到返回值 == 期望值 → 知道自己赢了 → `KV_CONSENSUS_WIN_ALL`

**如果Client A和Client B同时操作同一个slot**：
```
Client A: CAS(Backup Slot X, 0x0, A的值) → 返回 0x0 → 成功！
Client B: CAS(Backup Slot X, 0x0, B的值) → 返回 A的值 → 失败！
```

因为CAS是原子的，A先到达，把slot从0x0改成了A的值。当B到达时，slot已经不是0x0了，所以B的CAS失败，返回当前值（A的值）。

如果有多个backup（`num_idx_rep > 2`），代码会同时对所有backup发CAS，然后统计"投票"结果。

**代码中的判断逻辑**（`check_cas_consensus_0`，`src/client.cc:1998-2082`）：

```cpp
// 遍历所有backup的CAS返回值
for (每个backup的CAS结果) {
    swap_back = CAS返回值;

    if (swap_back == expected_value || swap_back == target_value) {
        // CAS成功了（或者之前已经成功过）
        win_count[target_value]++;
    } else {
        // 别人赢了，swap_back是别人写入的值
        win_count[swap_back]++;
        // 把这个backup加入"第二轮修复"列表
    }
}

// 统计投票结果
if (所有backup都投给我)        → WIN_ALL     // 全票通过
if (多数backup投给我)          → WIN_MAJOR   // 多数通过
if (至少一个backup投给我)      → WIN_LITTLE  // 少数通过
if (没有backup投给我)          → FAIL        // 完全失败
```

#### Step 3：Backup Consensus 1（修复不一致的备份）

对应 `modify_backup_idx_consensus_1()`（`src/client.cc:1695`）。

只有在不是WIN_ALL时才需要：

- **WIN_ALL**：所有backup都同意 → 跳过此步
- **WIN_MAJOR**：对那些被别人抢占的backup，再做一次CAS，把别人的值改成自己的
- **FAIL**：放弃，轮询等待primary被别人修改（`src/client.cc:1711-1730`），然后返回

#### Step 4：写Commit Log

对应 `kv_log_commit()`。

```
Client A: RDMA WRITE → 把commit标记写到Primary的log区域
```

这是一个"保险"：如果Client A在下一步之前crash了，recovery进程可以从log中知道"A打算做什么"，然后帮A完成。

#### Step 5：CAS Primary（最终提交）

对应 `modify_primary_idx()`（`src/client.cc:1829`）。

```
Client A: CAS(Primary的Slot X地址, old_value=0x0, new_value=A的值)
```

**这是整个操作的原子提交点（commit point）**。

- 如果Primary的CAS成功 → 操作完成，key "hello" 已经被插入
- 如果Primary的CAS失败（只在`num_idx_rep==1`时可能发生）→ 重试

代码中的验证（`src/client.cc:1872`）：
```cpp
if (num_idx_rep_ > 1 &&
    *(uint64_t *)ctx->kv_modify_pr_cas_list[0].l_kv_addr
    != ctx->kv_modify_pr_cas_list[0].orig_value) {
    printf("cannot have happened!\n");  // 有多副本时primary CAS不应该失败
    exit(1);
}
```

### 为什么要先改Backup再改Primary？

这是FUSEE论文的核心设计。逻辑是：

1. **先在Backup上"投票"**：通过CAS竞争，决定谁有权修改这个slot
2. **赢家再修改Primary**：因为backup已经达成共识，赢家知道自己一定能成功修改primary
3. **如果先改Primary**：两个Client可能同时CAS Primary，一个成功一个失败，但backup上可能不一致。通过先在backup上达成共识，保证了backup和primary的一致性

### 总结：FUSEE的共识本质

```
FUSEE的共识 = 用RDMA CAS的原子性来解决并发冲突

核心依赖：
1. 硬件保证CAS是原子的（同一个slot，同一时间只有一个CAS能成功）
2. CAS返回"之前的值"，让失败者知道谁赢了
3. 通过先backup后primary的顺序，确保多副本一致
```

---

## 第二部分：为什么不能直接在CXL上用CAS？

在RDMA中，CAS是由**网卡硬件（NIC）**保证原子性的。网卡的内存控制器确保同一时刻只有一个CAS能修改一个地址。

但在**多节点共享CXL**的场景下：

```
Node 0 的CPU ──┐
               ├── CXL Switch ── CXL Memory
Node 1 的CPU ──┘

Node 0: CAS(addr, 0, A)  ┐
                          ├── 没有统一的仲裁者！
Node 1: CAS(addr, 0, B)  ┘
```

每个Node的CPU各自执行CAS指令，但CXL Type 3设备**没有跨节点的coherence domain**。这意味着：

- Node 0的CAS和Node 1的CAS可能**同时成功**（都以为自己把0改成了自己的值）
- 结果不确定——可能一个覆盖另一个，也可能产生数据损坏

所以：**我们不能依赖硬件原子操作，必须用软件协议来达成共识。**

---

## 第三部分：CXL-Based Consensus如何实现同样的目的

### 核心思路

FUSEE的共识本质是：**多个writer竞争一个slot，只有一个能赢**。

RDMA用**硬件CAS**一步完成竞争。CXL没有可靠的跨节点CAS，所以我们用**多步load/store协议**来模拟同样的效果——像选举一样，通过**提议-投票-确认**来决定谁赢。

### CXL共享内存上的Consensus Log

在CXL memory中，我们为每个hash slot维护一个**共识槽位（ConsensusLogEntry）**：

```
ConsensusLogEntry (64 bytes，在CXL共享内存上):
  slot_addr        — 正在争夺的是哪个hash slot
  proposer_id      — 谁提议的（Node 0? 1? 2?）
  proposal_epoch   — 提议的序号（越大越新）
  old_slot_value   — 提议者认为slot当前应该是什么值
  new_slot_value   — 提议者想把slot改成什么值
  vote[3]          — 3个Node的投票：0=还没投, 1=同意(ACK), 2=反对(NACK)
  status           — FREE / PROPOSED / COMMITTED / ABORTED
```

### 完整协议流程（以INSERT为例）

假设Node 0想插入key="hello"，对应Hash Table的Slot X。

#### Phase 0 — Claim（抢占提议权）

```
Node 0想修改Slot X，需要先"注册"自己的提议。

Node 0 做的事:
  1. 计算 idx = hash(Slot X的地址) % ConsensusLog大小
  2. 读CXL上 ConsensusLog[idx] 的当前状态
  3. 如果 status == FREE（没人在用）:
     → 写入: proposer_id = 0, epoch = 我的当前epoch
     → sfence（确保写入对其他Node可见）
  4. 等一小会，再读一次
  5. 如果 proposer_id 还是自己 → 抢占成功
     如果被别人覆盖了（别人的epoch更大）→ 重试
```

**这一步的目的**：等价于FUSEE中"准备CAS地址"（`fill_cas_addr`），确定自己有权提议修改这个slot。

**为什么不会两个Node同时抢占成功？**

- 如果Node 0和Node 1同时写入，两边都写了自己的`proposer_id`
- 但CXL memory是物理共享的，最终**只有一个值会留在那里**（后写的覆盖先写的）
- 通过`sfence`后再读回来验证，可以发现自己是否被覆盖了
- 用epoch（序号）做裁决：epoch更大的赢；epoch相同时`node_id`更小的赢

#### Phase 1 — Propose（正式提议）

```
Node 0（抢占成功后）:
  1. 填写完整的ConsensusLogEntry:
     slot_addr = Slot X的地址
     old_slot_value = 0x0（当前slot是空的）
     new_slot_value = A的值（指向Node 0刚写的KV数据）
     status = PROPOSED
     vote[0] = ACK（自己投自己一票）
  2. sfence（确保所有写入对其他Node可见）
```

**这一步等价于FUSEE中**：Client A构造CAS参数（`orig_value`和`swap_value`）。

#### Phase 2 — Vote（投票）

```
每个Node都有一个后台"投票线程"（fiber），不断扫描CXL上的ConsensusLog。

Node 1的投票线程发现 ConsensusLog[idx].status == PROPOSED:
  1. 读取 old_slot_value = 0x0
  2. 检查自己本地DRAM上的 Slot X 的当前值
  3. 如果本地 Slot X == 0x0（和提议的old值一致）:
     → vote[1] = ACK（同意）
     → sfence
  4. 如果本地 Slot X != 0x0（说明有别的操作已经修改了）:
     → vote[1] = NACK（反对）
     → sfence

Node 2 也做同样的事情。
```

**这一步等价于FUSEE中的Backup CAS（`backup_consensus_0`）**：

- FUSEE: CAS返回值 == 期望值 → 成功（等价于ACK）
- FUSEE: CAS返回值 != 期望值 → 失败（等价于NACK）

**核心区别**：

- FUSEE: 一个RDMA CAS操作同时完成"检查+修改"，硬件保证原子性
- CXL: 分成"检查"（读本地slot）+"投票"（写CXL），用软件协议保证正确性

#### Phase 3 — Commit or Abort（确认或放弃）

```
Node 0（提议者）轮询 vote[] 数组:

  if vote[0]==ACK && vote[1]==ACK:  // 2票同意（含自己）= 多数 (2/3)
     → status = COMMITTED
     → sfence
     → 返回成功！

  if vote[0]==ACK && vote[1]==NACK && vote[2]==NACK:  // 只有自己同意
     → status = ABORTED
     → sfence
     → 重试操作

  如果超时还没收到足够投票:
     → status = ABORTED
     → 重试操作
```

**这一步等价于FUSEE中的`check_cas_consensus_0`**（`src/client.cc:1998-2082`）：

- FUSEE: 统计CAS返回值 → `WIN_ALL` / `WIN_MAJOR` / `FAIL`
- CXL: 统计`vote[]`数组 → 达到多数即`COMMITTED`，否则`ABORTED`

#### Phase 4 — Apply（应用到本地）

```
所有Node的后台线程看到 status == COMMITTED:
  → 把 new_slot_value 写到自己本地DRAM的 Slot X 中
  → 如果有KV数据需要复制（INSERT/UPDATE），从CXL staging buffer复制到本地DRAM
```

**这一步等价于FUSEE中**：RDMA CAS成功后，backup和primary的slot都已经被修改了。但在CXL版本中，slot的实际修改是在COMMITTED之后由各节点**自行应用**的。

---

## 第四部分：对比总结

### 一张表看清对应关系

| FUSEE (RDMA) | CXL-FUSEE | 作用 |
|---|---|---|
| 构造CAS参数 (`fill_cas_addr`, `client.cc:924`) | Phase 0: Claim + Phase 1: Propose | 声明"我要修改这个slot，从A改成B" |
| RDMA CAS on Backup (`backup_consensus_0`, `client.cc:2743`) | Phase 2: Vote (背景投票线程) | 检查提议是否和当前状态一致 |
| `check_cas_consensus_0` (`client.cc:1998`) 统计CAS返回值 | Phase 3: Commit/Abort (统计`vote[]`) | 判断是否获得多数同意 |
| `backup_consensus_1` (`client.cc:1695`) 修复不一致backup | 不需要（vote失败直接ABORT重试） | 保证所有副本一致 |
| RDMA WRITE commit log (`kv_log_commit`) | OpLog写入CXL (Phase 1中已完成) | 崩溃恢复依据 |
| RDMA CAS on Primary (`modify_primary_idx`, `client.cc:1829`) | Phase 4: Apply (COMMITTED后各节点自行应用) | 最终生效 |

### 关键差异

```
FUSEE (RDMA):
  原子性来源 = 硬件CAS（NIC保证）
  通信方式   = 一个Client直接修改远程内存（单向）
  竞争解决   = CAS的"比较"步骤自动解决（硬件做）
  延迟       = 1次RDMA CAS往返 ≈ 几微秒

CXL-FUSEE:
  原子性来源 = 软件协议（多数投票）
  通信方式   = 写CXL共享内存 + 其他节点轮询读取（多向）
  竞争解决   = 提议-投票-确认，多步load/store
  延迟       = 多次CXL读写 + 投票等待时间
```

### 为什么CXL版本是正确的？

关键安全性论证：

1. **每个slot同一时间只有一个活跃提议**：Claim phase用epoch排序确保
2. **COMMITTED需要多数（2/3）同意**：即使一个节点crash，另外两个还能达成共识
3. **只用load/store**：不依赖跨节点CAS，只依赖CXL memory的"写了之后别人能读到"（加`sfence`/`lfence`保证）
4. **Vote基于本地状态**：每个节点检查自己本地DRAM上的slot值，如果和提议的`old_value`一致就ACK，这保证了只有正确的提议才能被批准

---

## 第五部分：Propose-Vote-Commit 设计的问题与新方案

### Claim phase 的竞争问题

上面 Phase 0 (Claim) 的设计存在一个**正确性漏洞**——两个 node 可能"都认为自己 claim 成功"：

```
时间线（无 coherence 跨节点共享 CXL）：
  Node 0: store(proposer_id=0, epoch=42)
  Node 0: sfence
  Node 0: load → 看到 proposer_id=0 → "我赢了！"

                                Node 1: store(proposer_id=1, epoch=42)
                                Node 1: sfence
                                Node 1: load → 看到 proposer_id=1 → "我也赢了！"
```

因为 CXL Type 3 没有跨节点 coherence domain，Node 0 和 Node 1 的 store 之间没有原子性保证——后写的覆盖先写的，但每个 node 在 sfence 后读回的可能都是自己写的值。

**这违反了 claim 的语义**。投票阶段虽然能保证最终只有一个 proposer 拿到多数票（safety 仍然成立），但活性会很差：两个 node 都进入 propose 阶段后都拿不到多数 → 都 ABORT → 反复重试。

### 真正的解决方案：CXL 互斥锁

我们 fork 了一个独立 repo `cxl_shm_profiling`，里面实现了三种针对 non-coherent CXL memory 的软件互斥锁：

| Lock | 实现 | 特点 |
|---|---|---|
| **LFM** (Lamport's Fast Mutex) | `locks/lfm_lock.c` | 无竞争 fast path 最快 (~5 ops) |
| **Peterson** (Tournament Tree) | `locks/peterson_lock.c` | 有界等待 (log2(N) levels) |
| **Bakery** (Lamport's Bakery) | `locks/bakery_lock.c` | FCFS，绝对公平 (~3N ops) |

三种锁有相同的 API：

```c
typedef struct shm_mutex_t shm_mutex_t;
void     shm_mutex_init(shm_mutex_t *m);
uint64_t shm_mutex_lock(shm_mutex_t *m, int my_id, int num_hosts);
void     shm_mutex_unlock(shm_mutex_t *m, int my_id);
```

它们**只用 load/store + sfence/mfence**，不依赖跨节点原子操作，是 non-coherent CXL 上正确的互斥原语。

**关键洞察**：有了真正的互斥锁，**就不再需要"投票"达成共识——锁本身就是共识**。

### 为什么用 LFM？

KV 操作的 slot 锁竞争通常很低（key 均匀分布在百万级 bucket 上），LFM 的 fast path 是三种锁里最优的：

| | LFM | Peterson | Bakery |
|---|---|---|---|
| 无竞争延迟 | **最低** | 中等 | 最高 |
| 公平性 | 无保证 | 有界等待 | FCFS |
| 每次 lock 操作数 | ~5 | ~4×log2(N) | ~3N |
| 适合场景 | **低竞争 KV** | 中等竞争 | 高竞争需公平 |

KV store 不需要 FCFS，LFM 是最佳选择。

---

## 第六部分：新设计 Per-Bucket LFM Consensus

### 设计目标

- **高吞吐**：去掉 voter 轮询，直接 lock+modify+unlock
- **低延迟**：~5-10 μs per operation（vs 旧设计 15-50 μs）
- **正确性**：依赖 LFM 互斥语义而非投票

### 关键设计决策：Per-Bucket 而非 Per-Slot

之前 propose-vote-commit 设计是 per-slot，但 INSERT 实际上需要在 bucket 内 scan 找空 slot——单个 slot 锁不住整个"找空位"操作。所以新设计改为 **per-bucket lock**：

```
CXL 上的 BucketLock Table:
  BucketLock[NUM_BUCKETS]:
    shm_mutex_t  lock        // 64B aligned, LFM mutex
    RaceHashBucket bucket    // 64B, 含 7 个 slot
    // 一把锁保护整个 bucket（7 个 slot）
```

锁数量 ≈ 百万级（每个 hash bucket 一把），不同 bucket 上的操作完全并行。同一 bucket 内的操作通过 LFM 串行化。

### 7 步操作流程（INSERT 为例）

```
Node 0 INSERT key="hello" value="world":

Step 1: hash + 定位 BucketLock[idx]
  ▸ idx = hash("hello") % NUM_BUCKETS
  ▸ 纯本地计算，不访问 CXL

Step 2: shm_mutex_lock(&BucketLock[idx].lock, my_id=0, num_hosts=3)
  ▸ LFM fast path: ~5 CACHELINE ops (~5 μs)
  ▸ 拿到锁后，独占整个 bucket

Step 3: 在 bucket 内 scan 7 个 slot（首次读权威值）
  ▸ INSERT: 找 fp=0 的空 slot（如果 4 个 candidate bucket 都满 → TABLE_FULL）
  ▸ UPDATE: 找 fp 匹配 + key verify 的 slot（找不到 → KEY_NOT_FOUND）
  ▸ DELETE: 同 UPDATE
  ▸ 这是首次读，不是"再次确认"——锁外不读，只在锁内读

Step 4: 写 KV 数据 + Staging + OpLog
  ▸ 写本地 DRAM: KV block
  ▸ 写 CXL Staging Buffer (Node 0 的 region): KV payload
  ▸ 写 CXL OpLog (Node 0 的 region): {op_type=INSERT, status=IN_PROGRESS, ...}

Step 5: 写 BucketLock[idx].bucket.slot[empty_idx] = A  ← COMMIT POINT
  ▸ 直接写权威值到 CXL（single source of truth）
  ▸ 顺手更新本地 index 缓存
  ▸ 更新 OpLog: status := COMMITTED

Step 6: shm_mutex_unlock(&BucketLock[idx].lock, my_id=0)
  ▸ LFM release: ~2 CACHELINE ops

Step 7 (异步): 其他 node 的 replication fiber
  ▸ 周期扫描 BucketLock 表，发现变化
  ▸ 从 Staging Buffer 复制 KV 数据到本地 DRAM
  ▸ 更新本地 index 缓存
```

**Step 5 是 commit point**：一旦 `BucketLock[idx].bucket.slot[empty_idx] = A` 写入 CXL 并 flush 完成，操作就算成功——即使 Node 0 立刻 crash，其他 node 也能从 CXL 上看到新值，知道操作已生效。

### 同步路径只有 6 步（Step 7 异步）

|  | 旧设计 (Propose-Vote-Commit) | 新设计 (LFM) |
|---|---|---|
| Step 数 | 7 (含 voter 轮询) | 6 (无 voter) |
| CXL ops (无竞争) | ~10 + voter 等待 | ~12, 无等待 |
| 延迟 | 15-50 μs (voter 轮询瓶颈) | **5-10 μs** (LFM fast path) |
| 冲突处理 | ABORT + retry 整个流程 | spin-wait 自动排队 |
| Claim 竞争 | 有 (两 node 都"赢") | **不可能** (LFM 是真互斥) |

---

## 第七部分：CXL 内存 Layout

```
CXL Type 3 Shared Memory (mmap'd by all nodes):

┌──────────────────────────────────────────────────────┐
│ GlobalHeader (4 KB)                                  │
│   magic, num_nodes, region_size, sub-region offsets   │
├──────────────────────────────────────────────────────┤
│ Heartbeat & Membership (4 KB)                        │
│   per-node: epoch, timestamp_us, status              │
├──────────────────────────────────────────────────────┤
│ RaceHashRoot (4 KB)                                  │
│   subtable_entry[32][replicas] - shared directory    │
├──────────────────────────────────────────────────────┤
│ AllocationDirectory (16 MB)                          │
│   per-node block bitmap                              │
├──────────────────────────────────────────────────────┤
│ BucketLock Table (~256 MB)                           │
│   array of BucketLockEntry, indexed by bucket_idx    │
│                                                       │
│   BucketLockEntry (cache-line aligned):              │
│     shm_mutex_t   lock          // LFM mutex          │
│     RaceHashBucket bucket       // 64B (7 slots)     │
├──────────────────────────────────────────────────────┤
│ OpLog Area (256 MB) — per-node ring buffer            │
│   Node 0: ~80MB (only Node 0 writes)                  │
│   Node 1: ~80MB (only Node 1 writes)                  │
│   Node 2: ~80MB (only Node 2 writes)                  │
│                                                       │
│   OpLogEntry (128B, fixed):                          │
│     epoch, op_type, status, node_id,                 │
│     key_hash, bucket_idx, slot_idx,                  │
│     old_slot_value, new_slot_value,                  │
│     staging_offset, payload_size, crc                │
├──────────────────────────────────────────────────────┤
│ Data Staging Area (~512 MB) — per-node ring buffer    │
│   Node 0: ~170MB                                     │
│   Node 1: ~170MB                                     │
│   Node 2: ~170MB                                     │
│                                                       │
│   StagingHeader: head_off, tail_off, capacity         │
│   StagingEntry: entry_id, size, flags, crc, payload[]│
└──────────────────────────────────────────────────────┘
```

### 为什么 OpLog 和 Staging 用 per-node ring buffer

**单生产者-多消费者 (SPMC) 模式**：
- 每个 node 拥有自己的一段 ring buffer
- 只有 owner 写 (single producer) → 不需要 lock，不需要 LFM
- 其他 node 是只读 consumer，各自维护 local_head 跟踪进度
- Owner 在所有 consumer 都消费完后才能 GC 旧 entry

这避免了所有跨节点写竞争，性能上限就是裸 CXL 写带宽。

---

## 第八部分：读路径设计

### 一致性问题

Step 5 commit 后，CXL 上 `BucketLock[idx].bucket.slot[i] = A`，但其他 node 的本地 DRAM 缓存可能还没同步。如果 Node 1 立刻 SEARCH，怎么办？

### 推荐方案：Lazy Refresh on Miss

```
search(key):
  bucket_idx = hash(key) % NUM_BUCKETS
  fp_target  = compute_fp(key)

  ┌─ Fast Path (本地 DRAM 命中) ─────────────────────┐
  │ 1. scan local bucket cache                      │
  │ 2. 如果 fp 匹配 → 读本地 KV → verify → return    │
  └────────────────────────────────────────────────┘
                       │
                  本地 miss
                       ↓
  ┌─ Slow Path (去 CXL 看权威值) ───────────────────┐
  │ 3. CACHELINE_LOAD CXL BucketLock[idx].bucket    │
  │ 4. 在 CXL bucket 中 scan fp                      │
  │    → 没有 → 真的 KEY_NOT_FOUND                   │
  │ 5. 从 CXL Staging[owner].find(entry_id) 读 KV   │
  │    (Staging GC 保证：所有 consumer 都消费完才回收) │
  │ 6. 更新本地 cache (lazy refresh)                  │
  │ 7. return KV                                     │
  └────────────────────────────────────────────────┘
```

**特点**：
- 命中场景：纯本地，~100 ns
- Miss 场景：1 次 CXL load + 1 次 staging read，~2-3 μs
- 一致性保证：不会漏读已 commit 的 key（KEY_NOT_FOUND 是真的不存在）
- KV 数据始终从 CXL Staging 可达（GC 策略保证）

### 为什么 Staging Buffer 是 reader 的 fallback

Staging Buffer 服务双重目的：
1. **Replication source**：后台 fiber 拉取
2. **Read fallback**：还没拉到数据的 reader 直接从 CXL 读

只要还有任意 consumer 没消费 entry X，X 就一定还在 staging 里。所以 slow path reader 总能读到。

---

## 第九部分：Crash Recovery

### Per-Node OpLog 简化恢复

每个 node 有自己的 OpLog（其他 node 只读）。当一个 node crash 时：

```
1. Heartbeat 检测：survivor node 发现 node X 心跳超时
2. 强制释放锁：扫描 BucketLock 表，找 LFM mutex 中 owner=node X 的
   → 重置 mutex 状态（y=0, b[node_X]=0）
3. 扫描 node X 的 OpLog：
   - 对每个 status=COMMITTED 的 entry → 确保已 apply 到本地（redo）
   - 对每个 status=IN_PROGRESS 的 entry → 检查 BucketLock[idx] 当前状态
     - 如果新值已写入 → COMMITTED，redo
     - 如果还是旧值 → 操作未完成，回滚分配的资源
4. 更新 membership 表：标记 node X 为 DEAD
```

**关键**：因为 OpLog 是 single-producer，crash 时不需要担心"恢复进程读到 partial write"——OpLog entry 用 `flags=READY` 发布栅栏保证原子可见性。

### 实现关键代码引用

| 组件 | 来源 |
|---|---|
| LFM 锁实现 | `cxl_shm_profiling/locks/lfm_lock.c` |
| `shm_mutex_t` 定义 | `cxl_shm_profiling/locks/lfm_lock.h` |
| CACHELINE_STORE / LOAD | `cxl_shm_profiling/common.h` |
| Buddy allocator | `cxl_shm_profiling/shm.c` (block size 2 MB) |
| Multi-host barrier | `cxl_shm_profiling/barrier.c` |

---

## 第十部分：实现 Roadmap

### Phase 1: 基础设施（基于 cxl_shm_profiling）
- [ ] 在 CXL 上分配 shared region (`shm_malloc_id`)
- [ ] 初始化 BucketLock 表（每个 entry 包含 `shm_mutex_t` + `RaceHashBucket`）
- [ ] 初始化 OpLog 和 Staging 的 per-node ring buffer

### Phase 2: 核心 API
- [ ] `cxl_kv_insert(key, value)` — 实现 6 步流程
- [ ] `cxl_kv_search(key)` — 实现 fast path + slow path
- [ ] `cxl_kv_update(key, value)` — 复用 insert 流程
- [ ] `cxl_kv_delete(key)` — 写 slot=0

### Phase 3: 后台 fiber
- [ ] Replication fiber：扫描 BucketLock 表，复制变化到本地 DRAM
- [ ] Heartbeat fiber：周期更新自己的心跳时间戳
- [ ] GC fiber：清理 Staging Buffer 中所有 consumer 已消费的 entry

### Phase 4: Crash Recovery
- [ ] Heartbeat timeout 检测
- [ ] 强制释放 dead node 的锁
- [ ] OpLog 扫描 + redo / rollback

### Phase 5: 优化与测试
- [ ] YCSB benchmark 移植
- [ ] 与 FUSEE RDMA 版本性能对比
- [ ] 多种一致性级别的可配置 SEARCH

### 关键文件结构（计划）

```
src/
  cxl_node.h/c          — 主节点类（替代 client.h/c + server.h/c）
  cxl_bucket_lock.h/c   — BucketLock 表 + LFM 封装
  cxl_oplog.h/c         — OpLog 读写 + crash recovery
  cxl_staging.h/c       — Staging Buffer 读写 + GC
  cxl_kv_ops.h/c        — INSERT/SEARCH/UPDATE/DELETE 6 步流程
  cxl_replication.h/c   — 后台 replication fiber
```

---

## 第十一部分：实验验证（A / B / C 对比）

### 实验设置

**目的**：在真实实现之前，用微基准快速验证 3 个方案在 YCSB A / C workload 上的 latency / throughput 趋势。

**方法**：
- 基础库：`cxl_shm_profiling` 的 LFM 锁 + `CACHELINE_STORE/LOAD` (clflush + fence)
- 代码：`cxl_shm_profiling/bench/abc_bench.c`（每个 node 一个进程）
- 3 个写协议的操作序列都用真实 CACHELINE op 模拟（staging 写 × 4 + commit + 协议特定开销）
- Backing：tmpfs (`/dev/shm`)
- 配置：**4 个模拟 node**，每个 2 threads，3000 ops/thread，8192 buckets

**测量基线**（tmpfs 上）：
```
plain store (cached):           3.8 ns
plain load  (cached):           2.6 ns
CACHELINE_STORE (+flush+sfence): 105.9 ns
CACHELINE_LOAD  (+flush+mfence): 149.0 ns
```

### 结果表

| Option | Workload | 吞吐 (ops/s, aggregate) | w_avg (μs) | w_p99 (μs) | r_avg (μs) | r_p99 (μs) |
|---|---|---|---|---|---|---|
| A | A (50/50) |    7,815  | 1991.7 | 3529.1 | 0.23 | 0.90 |
| A | C (100% read) | 15.5M |   —   |   —   | 0.44 | 4.66 |
| B | A | 2.08M | 4.16 | 11.84 | 1.19 | 7.78 |
| B | C | 12.7M | — | — | 0.53 | 6.22 |
| C | A | **2.40M** | **4.03** | 10.27 | 2.16 | 8.63 |
| C | C | 10.1M | — | — | 0.76 | 6.62 |

### 关键发现

1. **写延迟** (YCSB A)：A = **494×** C
   - A 的 sync-replication 等 ACK 是主导瓶颈
   - B 和 C 的写延迟接近（4-5 μs），B 略慢因为多了 invalidation push
2. **写吞吐** (YCSB A)：C = **307×** A
   - 1 个 node 等 ACK 时，其他 node 的 replicator 也被 CPU 调度压住
   - 4 node 下放大效应比 3 node 更明显
3. **读延迟** (YCSB C, 100% 读)：
   - A: 0.44 μs （本地 cache 命中，无 flush）
   - B: 0.53 μs
   - **C: 0.76 μs** （Lazy RC 的 strict read 每次 flush）
   - 读延迟差距小（~0.3 μs），但在高读压力下是稳定的额外成本
4. **读吞吐** (YCSB C)：A > B > C
   - 但三者都在 10M+ ops/s 级别，差距 ~50%

### tmpfs → 真 CXL 的外推

| Type | Load latency | tmpfs 倍数 |
|---|---|---|
| Local DRAM | ~60-100 ns | ~0.5-1× |
| tmpfs + flush/fence | ~150 ns | 1× (baseline) |
| CXL Type 3 直连 | ~180-250 ns | 1.2-1.7× |
| CXL 2.0 + 1 switch | ~300-500 ns | 2-3× |
| CXL 3.0 multi-hop | ~500-1000 ns | 3-7× |

真 CXL 上：
- 所有 latency × 2-5（Option A 会到 4000-10000 μs，仍是最差）
- 所有 throughput ÷ 2-5
- **相对趋势（A/B/C 谁更好）保持**

### 结论

实验验证了理论分析：

1. **Option A 被排除**：写延迟 2000 μs（真 CXL 上会到 4000-10000 μs），无法接受
2. **Option C (Lazy RC) 最优**：
   - 写延迟 ~4 μs（同量级 RDMA CAS 原始成本）
   - 写吞吐 2.4M ops/s（本测试中）
   - 读延迟略高（每次 0.3 μs 的 flush 代价）但可接受
3. **Option B 和 C 接近**：如果读压力很重，B 的不 flush 读反而更优；但 B 的 invalidation push 带来额外写开销。C 简单且语义清晰，优先选 C。

### 实验产物

| 文件 | 内容 |
|---|---|
| `cxl_shm_profiling/bench/abc_bench.c` | C 基准源代码 |
| `cxl_shm_profiling/bench/run_abc.sh` | 跑矩阵的脚本 |
| `cxl_shm_profiling/bench/plot_abc.py` | 绘图脚本 |
| `cxl_shm_profiling/bench/abc_results.log` | 每 node 原始 RESULT 行 (24 条) |
| `cxl_shm_profiling/bench/abc_summary.txt` | 汇总数据表 + 说明 |
| `cxl_shm_profiling/bench/abc_results.png` | 4 格对比图 |

### 后续验证

- [ ] 扫 write ratio (0%, 10%, 25%, 75%, 100%) 找到 A 和 B/C 的交叉点（如果有）
- [ ] 在真 CXL 硬件上跑相同 benchmark，验证相对趋势是否保持
- [ ] 在 Option C 的实现完成后，用 YCSB 真实 workload 和 FUSEE RDMA baseline 对比
