# MVCCEngine 模块内部架构关系图

## 架构概览

MVCCEngine（多版本并发控制引擎）是 MementoDB 实现并发控制的核心组件，通过维护数据的多个版本来实现无锁读取和高并发性能。

## 核心组件关系图

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         MVCCEngine 架构                                  │
└─────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────┐
│                        MVCCEngine                                        │
│  (多版本并发控制引擎)                                                     │
│                                                                          │
│  核心功能：                                                              │
│  - create_snapshot()     创建事务快照                                    │
│  - read_version()        基于快照读取版本                                 │
│  - write_version()       写入新版本                                      │
│  - commit_version()      提交版本                                        │
│  - abort_version()       回滚版本                                        │
│  - cleanup_old_versions() 垃圾回收旧版本                                 │
└─────────────────────────────────────────────────────────────────────────┘
                              │
                              │ 使用/管理
                              ├──────────────────┬──────────────────┬──────────────┐
                              ▼                  ▼                  ▼              ▼
                    ┌──────────────┐   ┌──────────────┐   ┌──────────────┐  ┌──────────────┐
                    │Snapshot      │   │Version       │   │WriteSet      │  │Timestamp     │
                    │              │   │(版本链)      │   │(写集合)      │  │Manager      │
                    │- 可见性规则   │   │              │   │              │  │              │
                    │- 时间戳       │   │- timestamp   │   │- tid -> keys │  │- 原子递增    │
                    │- 活跃事务集   │   │- value       │   │              │  │- 时间戳生成  │
                    │              │   │- txid        │   │              │  │              │
                    │              │   │- committed   │   │              │  │              │
                    │              │   │- metadata    │   │              │  │              │
                    └──────────────┘   └──────────────┘   └──────────────┘  └──────────────┘
                              │                  │                  │              │
                              │ 创建             │ 存储             │ 跟踪         │ 使用
                              ▼                  ▼                  ▼              ▼
                    ┌──────────────┐   ┌──────────────┐   ┌──────────────┐  ┌──────────────┐
                    │Snapshot      │   │Version       │   │TransactionID │  │current_      │
                    │Factory       │   │Chain         │   │              │  │timestamp_    │
                    │              │   │              │   │              │  │(原子变量)    │
                    │Snapshot::    │   │key ->        │   │              │  │              │
                    │create()      │   │[Version...]   │   │              │  │              │
                    └──────────────┘   └──────────────┘   └──────────────┘  └──────────────┘
```

## 详细组件关系

### 1. MVCCEngine (核心引擎)

**职责**：管理所有数据版本，提供 MVCC 操作接口

**内部数据结构**：
```cpp
MVCCEngine {
    // 版本存储：key -> vector<Version>
    std::unordered_map<
        std::vector<char>, 
        std::vector<Version>, 
        VectorCharHash
    > versions_;
    
    // 事务写集合：tid -> [keys]
    std::unordered_map<
        TransactionID, 
        std::vector<std::vector<char>>
    > write_sets_;
    
    // 时间戳生成器
    std::atomic<uint64_t> current_timestamp_;
    
    // 配置
    MVCCConfig config_;
}
```

**依赖关系**：
```
MVCCEngine
├── Snapshot (创建)              # 为事务创建快照
├── Version (管理)               # 管理数据版本
├── VectorCharHash (使用)        # 键的哈希函数
└── TransactionContext (引用)    # 获取事务信息
```

---

### 2. Version (版本结构)

**职责**：存储单个数据版本的所有信息

**结构定义**：
```cpp
struct Version {
    uint64_t timestamp;              // 版本时间戳
    std::vector<char> value;         // 数据值
    TransactionID transaction_id;    // 创建版本的事务ID
    bool committed;                  // 是否已提交
    
    // 扩展元信息
    uint64_t create_txid;           // 创建事务ID
    uint64_t delete_txid;            // 删除事务ID（如果被删除）
    uint64_t begin_timestamp;        // 版本开始时间戳
    uint64_t end_timestamp;         // 版本结束时间戳（默认最大值）
    bool deleted;                    // 是否被删除
    
    // 可见性判断
    bool is_visible(uint64_t snapshot_ts) const;
};
```

**版本链结构**：
```
Key: "user:123"
  └─ Version Chain:
     ├─ Version[0]: timestamp=100, value="Alice", committed=true
     ├─ Version[1]: timestamp=200, value="Bob", committed=true
     └─ Version[2]: timestamp=300, value="Charlie", committed=false (当前写入)
```

**可见性规则**：
```cpp
bool Version::is_visible(uint64_t snapshot_ts) const {
    if (!committed || deleted) return false;
    uint64_t begin_ts = (begin_timestamp == 0) ? timestamp : begin_timestamp;
    uint64_t end_ts = end_timestamp;
    return begin_ts <= snapshot_ts && snapshot_ts < end_ts;
}
```

---

### 3. Snapshot (快照)

**职责**：定义数据可见性规则

**MVCCEngine 与 Snapshot 的关系**：
```
MVCCEngine.create_snapshot(tid)
  └─→ Snapshot::create(
        SnapshotType::REPEATABLE_READ,
        snapshot_timestamp,
        active_transactions,
        tid
      )
```

**快照类型**：
- `ReadCommittedSnapshot`: 读已提交（语句级快照）
- `RepeatableReadSnapshot`: 可重复读（事务级快照）
- `SnapshotIsolationSnapshot`: 快照隔离（写写冲突检测）
- `SerializableSnapshot`: 串行化

**快照包含的信息**：
```cpp
Snapshot {
    uint64_t snapshot_timestamp;        // 快照时间戳
    std::set<TransactionID> active_txns;  // 活跃事务集合
    TransactionID creator_txid;        // 创建者事务ID
}
```

---

### 4. WriteSet (写集合)

**职责**：跟踪每个事务的写操作

**结构**：
```cpp
std::unordered_map<
    TransactionID,                    // 事务ID
    std::vector<std::vector<char>>    // 该事务修改的所有键
> write_sets_;
```

**用途**：
1. **提交时**：快速找到事务的所有写操作，批量标记为已提交
2. **回滚时**：快速找到事务的所有写操作，批量删除未提交版本
3. **统计**：跟踪活跃事务数量

---

### 5. Timestamp Manager (时间戳管理器)

**职责**：生成全局唯一的时间戳

**实现**：
```cpp
std::atomic<uint64_t> current_timestamp_{1};

// 生成新时间戳
uint64_t new_ts = current_timestamp_.fetch_add(1);
```

**时间戳用途**：
- 版本时间戳：标识版本的创建时间
- 快照时间戳：标识快照的创建时间
- GC 时间戳：用于判断版本是否过期

---

## 核心操作流程

### 1. 创建快照流程

```
TransactionManager.begin_transaction(tid)
  │
  └─→ MVCCEngine.create_snapshot(tid)
         │
         ├─→ 1. 获取当前时间戳
         │      current_timestamp_.load()
         │
         ├─→ 2. 创建快照对象
         │      Snapshot::create(
         │        SnapshotType::REPEATABLE_READ,
         │        snapshot_timestamp,
         │        active_transactions,
         │        tid
         │      )
         │
         └─→ 3. 返回快照指针
                Snapshot::Ptr
```

### 2. 读取版本流程

```
TransactionManager.read(key, snapshot)
  │
  └─→ MVCCEngine.read_version(key, snapshot)
         │
         ├─→ 1. 获取读锁（shared_lock）
         │      std::shared_lock lock(versions_mutex_)
         │
         ├─→ 2. 查找键的版本链
         │      versions_.find(key)
         │
         ├─→ 3. 查找可见版本
         │      find_readable_version(key, snapshot.timestamp())
         │      │
         │      └─→ 从最新版本开始遍历
         │            for (auto rit = versions.rbegin(); ...)
         │              if (version.is_visible(snapshot_ts))
         │                return version;
         │
         └─→ 4. 返回版本值
                return version.value;
```

**可见性判断示例**：
```
当前时间戳: 500
快照时间戳: 300

Key: "user:123"
  Version[0]: timestamp=100, committed=true  ✅ 可见 (100 <= 300)
  Version[1]: timestamp=200, committed=true  ✅ 可见 (200 <= 300)
  Version[2]: timestamp=400, committed=true   ❌ 不可见 (400 > 300)
  Version[3]: timestamp=350, committed=false  ❌ 不可见 (未提交)

结果：返回 Version[1] (timestamp=200)
```

### 3. 写入版本流程

```
TransactionManager.write(key, value, tid)
  │
  └─→ MVCCEngine.write_version(tid, key, value)
         │
         ├─→ 1. 生成写时间戳
         │      write_timestamp = current_timestamp_.fetch_add(1)
         │
         ├─→ 2. 获取写锁
         │      std::lock_guard lock(versions_mutex_)
         │
         ├─→ 3. 创建新版本
         │      Version new_version;
         │      new_version.timestamp = write_timestamp;
         │      new_version.value = value;
         │      new_version.transaction_id = tid;
         │      new_version.committed = false;
         │      new_version.create_txid = tid;
         │      new_version.begin_timestamp = write_timestamp;
         │
         ├─→ 4. 添加到版本链
         │      versions_[key].push_back(new_version);
         │
         ├─→ 5. 限制版本数量
         │      if (versions_[key].size() > max_version_count)
         │        versions_[key].erase(versions_[key].begin());
         │
         └─→ 6. 更新写集合
                write_sets_[tid].push_back(key);
```

**版本链更新示例**：
```
写入前：
Key: "user:123"
  └─ [Version(ts=100), Version(ts=200)]

写入后 (tid=5, ts=300):
Key: "user:123"
  └─ [Version(ts=100), Version(ts=200), Version(ts=300, tid=5, committed=false)]
```

### 4. 提交版本流程

```
TransactionManager.commit_transaction(tid, commit_ts)
  │
  └─→ MVCCEngine.commit_version(tid, commit_ts)
         │
         ├─→ 1. 获取写集合
         │      write_sets_.find(tid)
         │
         ├─→ 2. 遍历写集合中的所有键
         │      for (const auto& key : write_sets_[tid])
         │
         ├─→ 3. 标记版本为已提交
         │      for (auto& version : versions_[key])
         │        if (version.transaction_id == tid && !version.committed)
         │          version.committed = true;
         │          version.timestamp = commit_ts;
         │
         └─→ 4. 清除写集合
                write_sets_.erase(tid);
```

**提交前后对比**：
```
提交前：
Key: "user:123"
  └─ [Version(ts=300, tid=5, committed=false)]

提交后 (commit_ts=350):
Key: "user:123"
  └─ [Version(ts=350, tid=5, committed=true)]  ✅ 已提交
```

### 5. 回滚版本流程

```
TransactionManager.abort_transaction(tid)
  │
  └─→ MVCCEngine.abort_version(tid)
         │
         ├─→ 1. 获取写集合
         │      write_sets_.find(tid)
         │
         ├─→ 2. 遍历写集合中的所有键
         │      for (const auto& key : write_sets_[tid])
         │
         ├─→ 3. 删除未提交版本
         │      versions.erase(
         │        std::remove_if(versions.begin(), versions.end(),
         │          [tid](const Version& v) {
         │            return v.transaction_id == tid && !v.committed;
         │          }),
         │        versions.end()
         │      );
         │
         └─→ 4. 清除写集合
                write_sets_.erase(tid);
```

**回滚前后对比**：
```
回滚前：
Key: "user:123"
  └─ [Version(ts=100), Version(ts=200), Version(ts=300, tid=5, committed=false)]

回滚后：
Key: "user:123"
  └─ [Version(ts=100), Version(ts=200)]  ✅ 未提交版本已删除
```

### 6. 垃圾回收流程

```
定期调用 / 手动触发
  │
  └─→ MVCCEngine.cleanup_old_versions(current_timestamp)
         │
         ├─→ 1. 计算截止时间戳
         │      cutoff_ts = current_timestamp - retention_time_ms
         │
         ├─→ 2. 遍历所有键的版本链
         │      for (auto& [key, versions] : versions_)
         │
         ├─→ 3. 删除过期版本
         │      remove_old_versions(versions, cutoff_ts, removed_count, reclaimed_bytes)
         │      │
         │      └─→ 删除条件：
         │            version.committed && version.timestamp < cutoff_ts
         │
         ├─→ 4. 删除空版本链
         │      if (versions.empty())
         │        versions_.erase(key);
         │
         └─→ 5. 更新 GC 统计
                gc_stats_.collected_versions = removed_count;
                gc_stats_.memory_reclaimed = reclaimed_bytes;
```

**GC 示例**：
```
当前时间戳: 100000
保留时间: 60000ms
截止时间戳: 40000

Key: "user:123"
  Version(ts=10000, committed=true)   ✅ 删除 (10000 < 40000)
  Version(ts=20000, committed=true)   ✅ 删除 (20000 < 40000)
  Version(ts=30000, committed=true)   ✅ 删除 (30000 < 40000)
  Version(ts=50000, committed=true)   ❌ 保留 (50000 >= 40000)
  Version(ts=60000, committed=true)   ❌ 保留 (60000 >= 40000)
```

---

## 数据结构详细设计

### 版本存储结构

```
versions_: unordered_map<key, vector<Version>>

┌─────────────────────────────────────────────────────────────┐
│ Key: "user:123"                                              │
│ └─ Version Chain (按时间戳排序，新版本在末尾)                  │
│    ├─ Version[0]: ts=100, value="Alice", committed=true     │
│    ├─ Version[1]: ts=200, value="Bob", committed=true       │
│    └─ Version[2]: ts=300, value="Charlie", committed=false   │
├─────────────────────────────────────────────────────────────┤
│ Key: "user:456"                                              │
│ └─ Version Chain                                             │
│    ├─ Version[0]: ts=150, value="David", committed=true     │
│    └─ Version[1]: ts=250, value="Eve", committed=true       │
└─────────────────────────────────────────────────────────────┘
```

### 写集合结构

```
write_sets_: unordered_map<tid, vector<key>>

┌─────────────────────────────────────────────────────────────┐
│ TransactionID: 5                                            │
│ └─ Keys: ["user:123", "user:456", "user:789"]              │
├─────────────────────────────────────────────────────────────┤
│ TransactionID: 7                                            │
│ └─ Keys: ["user:123"]                                       │
└─────────────────────────────────────────────────────────────┘
```

---

## 并发控制

### 读写锁策略

```cpp
// 读操作：共享锁（多个读可以并发）
std::shared_lock<std::shared_mutex> lock(versions_mutex_);

// 写操作：独占锁（写时互斥）
std::lock_guard<std::shared_mutex> lock(versions_mutex_);
```

### 锁粒度

- **版本存储锁** (`versions_mutex_`): 保护整个版本存储结构
- **写集合锁** (`write_sets_mutex_`): 保护写集合结构

### 并发场景

**场景1：多个事务并发读取**
```
T1: read(key, snapshot1)  ─┐
T2: read(key, snapshot2)  ──┼─→ 共享锁，可以并发
T3: read(key, snapshot3)  ─┘
```

**场景2：读写并发**
```
T1: read(key, snapshot)   ─┐
T2: write(key, value)    ─┼─→ 互斥，T2 等待 T1 完成
```

**场景3：多个事务并发写入**
```
T1: write(key1, v1)  ─┐
T2: write(key2, v2)  ─┼─→ 互斥，串行执行
T3: write(key3, v3)  ─┘
```

---

## 统计信息

### EngineStats (引擎统计)

```cpp
struct EngineStats {
    size_t total_keys;                    // 不同 key 的数量
    size_t total_versions;                // 所有版本总数
    size_t active_transactions;           // 有未提交写集合的事务数量
    size_t approximate_memory_bytes;     // 版本占用的近似内存
    uint64_t oldest_version_timestamp;    // 最老版本时间戳
    uint64_t newest_version_timestamp;    // 最新版本时间戳
};
```

### GCStats (垃圾回收统计)

```cpp
struct GCStats {
    size_t total_versions;        // 当前剩余版本数
    size_t collected_versions;    // 最近一次 GC 收集的版本数
    size_t memory_reclaimed;      // 近似回收的内存字节数
    uint64_t last_collection_time; // 最近一次 GC 的时间戳
};
```

---

## MVCCEngine 与其他模块的交互

### 与 TransactionManager 的交互

```
TransactionManager
  │
  ├─→ MVCCEngine.create_snapshot(tid)
  │      └─→ 返回 Snapshot::Ptr
  │
  ├─→ MVCCEngine.read_version(key, snapshot)
  │      └─→ 返回 std::optional<value>
  │
  ├─→ MVCCEngine.write_version(tid, key, value)
  │      └─→ 返回 bool
  │
  ├─→ MVCCEngine.commit_version(tid, commit_ts)
  │      └─→ 返回 bool
  │
  └─→ MVCCEngine.abort_version(tid)
         └─→ 返回 bool
```

### 与 Snapshot 的交互

```
MVCCEngine
  │
  ├─→ 创建快照
  │      Snapshot::create(type, timestamp, active_txns, tid)
  │
  └─→ 使用快照判断可见性
         snapshot.is_transaction_visible(txid, commit_ts)
         snapshot.is_timestamp_visible(timestamp)
         snapshot.is_version_visible(version)
```

### 与 TransactionContext 的交互

```
TransactionContext
  │
  └─→ 存储快照
         ctx->set_snapshot(MVCCEngine.create_snapshot(tid))
```

---

## 配置参数

### MVCCConfig

```cpp
struct MVCCConfig {
    bool enable_snapshot_isolation;        // 是否启用快照隔离
    uint64_t version_retention_time_ms;  // 版本保留时间（默认60秒）
    size_t max_version_count;              // 每个key的最大版本数（默认1000）
};
```

**配置影响**：
- `version_retention_time_ms`: 控制 GC 的激进程度
- `max_version_count`: 防止单个键的版本链过长
- `enable_snapshot_isolation`: 控制隔离级别行为

---

## 性能优化

### 1. 版本链查找优化

**当前实现**：从最新版本开始向后查找
```cpp
for (auto rit = versions.rbegin(); rit != versions.rend(); ++rit) {
    if (rit->is_visible(snapshot_ts)) {
        return *rit;  // 通常最新版本就是可见的
    }
}
```

**优化方向**：
- 使用索引加速查找
- 缓存最近访问的版本

### 2. 写集合优化

**当前实现**：使用 vector 存储键
```cpp
std::vector<std::vector<char>> write_sets_[tid];
```

**优化方向**：
- 使用 unordered_set 去重
- 预分配空间

### 3. 内存管理优化

**当前实现**：直接存储 vector<char>

**优化方向**：
- 使用内存池
- 压缩旧版本
- 延迟分配

---

## 总结

MVCCEngine 的核心设计特点：

1. **版本链存储**：每个键维护一个版本链，按时间戳排序
2. **快照隔离**：通过快照时间戳判断版本可见性
3. **写集合跟踪**：快速定位事务的写操作，支持高效提交/回滚
4. **原子时间戳**：使用原子变量生成全局唯一时间戳
5. **读写锁**：读操作并发，写操作互斥
6. **垃圾回收**：定期清理过期版本，控制内存使用

这种设计实现了：
- ✅ **无锁读取**：读操作不需要加锁（基于快照）
- ✅ **高并发**：多个读事务可以并发执行
- ✅ **一致性**：通过快照保证读取一致性
- ✅ **可扩展**：支持多种隔离级别

