# Transaction 模块内部架构关系图

## 架构概览

Transaction 模块是 MementoDB 的事务管理系统，负责提供 ACID 事务保证、并发控制和崩溃恢复能力。

## 核心组件关系图

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         Transaction 模块架构                              │
└─────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────┐
│                        TransactionManager                                │
│  (事务生命周期管理 - 核心协调器)                                           │
│                                                                          │
│  - begin_transaction()                                                   │
│  - commit_transaction()                                                  │
│  - abort_transaction()                                                   │
│  - acquire_read_lock() / acquire_write_lock()                           │
│  - log_begin() / log_commit() / log_abort()                             │
│  - recover()                                                             │
└─────────────────────────────────────────────────────────────────────────┘
                              │
                              │ 管理
                              ├──────────────────┬──────────────────┬──────────────┐
                              ▼                  ▼                  ▼              ▼
                    ┌──────────────┐   ┌──────────────┐   ┌──────────────┐  ┌──────────────┐
                    │Transaction   │   │LockManager   │   │FileWAL       │  │Recovery      │
                    │Context       │   │              │   │              │  │Manager       │
                    │              │   │              │   │              │  │              │
                    │- 事务状态     │   │- 锁获取/释放  │   │- 日志写入    │  │- 崩溃恢复    │
                    │- 隔离级别     │   │- 死锁检测    │   │- 段管理      │  │- 重做/撤销   │
                    │- 时间戳       │   │- 锁升级      │   │- 刷盘        │  │- 检查点      │
                    │- 读写集       │   │              │   │- 统计        │  │              │
                    │- 锁引用       │   │              │   │              │  │              │
                    │- WAL LSN     │   │              │   │              │  │              │
                    │- 快照         │   │              │   │              │  │              │
                    └──────────────┘   └──────────────┘   └──────────────┘  └──────────────┘
                              │                  │                  │              │
                              │ 包含             │ 使用             │ 实现         │ 使用
                              ▼                  ▼                  ▼              ▼
                    ┌──────────────┐   ┌──────────────┐   ┌──────────────┐  ┌──────────────┐
                    │Snapshot      │   │LockTable     │   │WALInterface  │  │Recovery      │
                    │              │   │              │   │              │  │Protocol      │
                    │- 可见性规则   │   │- 锁表项      │   │(抽象接口)    │  │              │
                    │- 时间戳       │   │- 等待队列    │   │              │  │- SimpleRedo  │
                    │- 活跃事务集   │   │- 锁模式      │   │              │  │  UndoProtocol│
                    │              │   │              │   │              │  │              │
                    │              │   │              │   │              │  │- ARIES       │
                    │              │   │              │   │              │  │  Protocol    │
                    └──────────────┘   └──────────────┘   └──────────────┘  └──────────────┘
                                                              │
                                                              │ 使用
                                                              ▼
                                                    ┌──────────────┐
                                                    │LogScanner    │
                                                    │              │
                                                    │- 扫描日志     │
                                                    │- 解析记录     │
                                                    └──────────────┘
```

## 详细组件关系

### 1. TransactionManager (事务管理器)

**职责**：事务生命周期管理、协调各个组件

**依赖关系**：
```
TransactionManager
├── TransactionContext (1:N)     # 管理多个事务上下文
├── LockManager (1:1)              # 锁管理
├── WAL (1:1)                      # 日志管理
└── RecoveryManager (1:1)          # 恢复管理
```

**关键方法**：
- `begin_transaction()` → 创建 TransactionContext
- `commit_transaction()` → 调用 WAL.log_commit() + LockManager.release_all_locks()
- `acquire_read_lock()` → 委托给 LockManager
- `recover()` → 委托给 RecoveryManager

---

### 2. TransactionContext (事务上下文)

**职责**：存储事务的运行时状态和元数据

**依赖关系**：
```
TransactionContext
├── Snapshot (1:1)                 # 事务快照
├── TransactionKey (N)              # 读写集中的键
└── LockReference (N)               # 锁引用信息
```

**关键字段**：
- `state`: TransactionState
- `isolation_level`: IsolationLevel
- `snapshot`: Snapshot::Ptr
- `read_set`: std::unordered_set<TransactionKey>
- `write_set`: std::unordered_map<TransactionKey, ...>
- `lock_references`: std::vector<LockReference>
- `wal_lsns`: std::vector<uint64_t>

---

### 3. LockManager (锁管理器)

**职责**：锁的获取、释放、升级，死锁检测

**依赖关系**：
```
LockManager
├── LockTable (1:1)                # 锁表
├── DeadlockDetector (1:1)         # 死锁检测器
├── LockTableGraphBuilder (1:1)    # 等待图构建器
└── TransactionManager (引用)       # 获取事务信息
```

**关键方法**：
- `acquire_read_lock()` → LockTable.try_acquire() + DeadlockDetector.detect()
- `upgrade_lock()` → LockTable.upgrade()
- `handle_deadlock()` → 选择牺牲者并中止

---

### 4. LockTable (锁表)

**职责**：存储和管理每个键上的锁信息

**依赖关系**：
```
LockTable
└── LockEntry (N)                  # 每个键对应一个锁表项
    ├── 持有锁的事务集合
    └── 等待队列
```

**关键结构**：
- `std::unordered_map<std::string, LockEntry> locks_`
- `LockEntry` 包含：
  - `holders_`: 持有锁的事务集合
  - `wait_queue_`: 等待队列

---

### 5. DeadlockDetector (死锁检测器)

**职责**：检测事务间的死锁

**依赖关系**：
```
DeadlockDetector
├── WaitForGraph (1:1)             # 等待图
├── CycleDetector (1:1)            # 环检测算法
└── VictimSelector (1:1)           # 牺牲者选择策略
```

**关键方法**：
- `detect()` → 构建等待图 → 检测环 → 返回 DeadlockInfo
- `detect_cycles()` → 使用 DFS 或 Tarjan 算法

**支持的检测算法**：
- DFS (深度优先搜索)
- Tarjan (强连通分量)
- 定期检测
- 按需检测

---

### 6. LockTableGraphBuilder (等待图构建器)

**职责**：从 LockTable 构建等待图

**依赖关系**：
```
LockTableGraphBuilder
├── LockTable (引用)               # 读取锁状态
└── TransactionContext (引用)      # 获取事务信息
```

**关键方法**：
- `build_wait_for_graph()` → 遍历 LockTable，构建边

**构建逻辑**：
```
对于每个 LockEntry:
  对于每个等待的事务 T_wait:
    对于每个持有锁的事务 T_hold:
      添加边: T_wait -> T_hold
```

---

### 7. FileWAL (文件WAL实现)

**职责**：将事务操作持久化到磁盘

**依赖关系**：
```
FileWAL
└── WALInterface (实现)            # 实现 WAL 抽象接口
```

**关键功能**：
- `append_log_record()` → 写入日志记录
- `flush()` → 刷盘
- `rotate_segment()` → 段切换
- `get_statistics()` → 统计信息

**日志类型**：
- BEGIN: 事务开始
- COMMIT: 事务提交
- ABORT: 事务中止
- PHYSICAL_MUTATION: 物理修改

---

### 8. RecoveryManager (恢复管理器)

**职责**：崩溃恢复、事务回滚

**依赖关系**：
```
RecoveryManager
├── WAL (引用)                     # 读取日志
├── RecoveryProtocol (1:N)         # 恢复协议
│   ├── SimpleRedoUndoProtocol
│   └── ARIESRecoveryProtocol
├── StorageAccessor (1:1)          # 存储访问器
├── CheckpointManager (1:1)        # 检查点管理器
└── RecoveryMonitor (N)             # 恢复监控器
```

**恢复阶段**：
1. ANALYSIS: 分析阶段（确定活跃事务）
2. REDO: 重做阶段（重放已提交事务）
3. UNDO: 撤销阶段（回滚未提交事务）
4. CLEANUP: 清理阶段

---

### 9. MVCCEngine (MVCC引擎)

**职责**：多版本并发控制，管理数据版本

**依赖关系**：
```
MVCCEngine
├── Snapshot (1:N)                 # 为事务创建快照
└── Version (N)                    # 数据版本
```

**关键方法**：
- `create_snapshot()` → 创建快照
- `read_version()` → 根据快照读取版本
- `write_version()` → 写入新版本
- `commit_version()` → 提交版本
- `cleanup_old_versions()` → 垃圾回收

**版本结构**：
```
Version {
    timestamp
    value
    transaction_id
    committed
    create_txid
    delete_txid
    begin_timestamp
    end_timestamp
    deleted
}
```

---

### 10. Snapshot (快照)

**职责**：定义数据可见性规则

**依赖关系**：
```
Snapshot (抽象基类)
├── ReadCommittedSnapshot          # 读已提交
├── RepeatableReadSnapshot         # 可重复读
├── SnapshotIsolationSnapshot      # 快照隔离
└── SerializableSnapshot           # 串行化
```

**关键方法**：
- `is_transaction_visible()` → 判断事务是否可见
- `is_timestamp_visible()` → 判断时间戳是否可见
- `is_version_visible()` → 判断版本是否可见

---

## 数据流图

### 事务提交流程

```
1. TransactionManager.commit_transaction(tid)
   │
   ├─→ 2. LockManager.release_all_locks(tid)
   │      │
   │      └─→ LockTable.release_all(tid)
   │
   ├─→ 3. FileWAL.log_commit(tid)
   │      │
   │      └─→ append_log_record(COMMIT)
   │      └─→ flush() [如果 sync_on_commit]
   │
   ├─→ 4. MVCCEngine.commit_version(tid)
   │      │
   │      └─→ 标记版本为 committed
   │
   └─→ 5. TransactionContext 状态 → COMMITTED
```

### 锁获取流程

```
1. TransactionManager.acquire_read_lock(tid, key)
   │
   └─→ 2. LockManager.acquire_read_lock(key, tid)
          │
          ├─→ 3. LockTable.try_acquire(key, tid, SHARED)
          │      │
          │      └─→ LockEntry.try_acquire(tid, SHARED)
          │             │
          │             ├─→ 成功 → 返回 SUCCESS
          │             │
          │             └─→ 失败 → 加入等待队列
          │
          └─→ 4. DeadlockDetector.detect()
                 │
                 ├─→ LockTableGraphBuilder.build_wait_for_graph()
                 │
                 └─→ CycleDetector.detect_cycles()
                      │
                      ├─→ 无死锁 → 返回空
                      │
                      └─→ 有死锁 → 返回 DeadlockInfo
                                   │
                                   └─→ LockManager.handle_deadlock()
                                        └─→ 选择牺牲者并中止
```

### 恢复流程

```
1. RecoveryManager.perform_crash_recovery()
   │
   ├─→ 2. ANALYSIS 阶段
   │      │
   │      └─→ 扫描 WAL，确定活跃事务
   │
   ├─→ 3. REDO 阶段
   │      │
   │      └─→ RecoveryProtocol.recover()
   │             │
   │             └─→ 重放所有已提交事务的操作
   │
   ├─→ 4. UNDO 阶段
   │      │
   │      └─→ 回滚所有未提交事务的操作
   │
   └─→ 5. CLEANUP 阶段
          │
          └─→ 清理恢复状态
```

## 组件交互时序图

### 事务提交时序

```
TransactionManager    LockManager    FileWAL    MVCCEngine    TransactionContext
      │                    │            │            │                │
      │─begin_transaction()│            │            │                │
      │──────────────────────────────────────────────────────────────>│
      │                    │            │            │                │
      │                    │            │            │                │
      │─acquire_write_lock()│            │            │                │
      │───────────────────>│            │            │                │
      │                    │─try_acquire()│            │                │
      │                    │<───────────│            │                │
      │<───────────────────│            │            │                │
      │                    │            │            │                │
      │─log_begin()        │            │            │                │
      │────────────────────────────────>│            │                │
      │                    │            │─append()   │                │
      │                    │            │<───────────│                │
      │<────────────────────────────────│            │                │
      │                    │            │            │                │
      │─write_version()    │            │            │                │
      │──────────────────────────────────────────────>│                │
      │                    │            │            │                │
      │─commit_transaction()│            │            │                │
      │──────────────────────────────────────────────────────────────>│
      │                    │            │            │                │
      │                    │─release_all()│            │                │
      │                    │<───────────│            │                │
      │                    │            │            │                │
      │─log_commit()       │            │            │                │
      │────────────────────────────────>│            │                │
      │                    │            │─append()   │                │
      │                    │            │─flush()    │                │
      │                    │            │<───────────│                │
      │<────────────────────────────────│            │                │
      │                    │            │            │                │
      │                    │            │─commit_version()│                │
      │                    │            │<───────────│                │
      │                    │            │            │                │
      │                    │            │            │                │
      │                    │            │            │                │
```

## 关键设计模式

### 1. 管理器模式 (Manager Pattern)
- `TransactionManager`: 统一管理事务生命周期
- `LockManager`: 统一管理锁操作
- `RecoveryManager`: 统一管理恢复流程

### 2. 上下文模式 (Context Pattern)
- `TransactionContext`: 存储事务运行时状态

### 3. 策略模式 (Strategy Pattern)
- `DeadlockDetector`: 支持多种检测算法
- `RecoveryProtocol`: 支持多种恢复协议
- `Snapshot`: 支持多种隔离级别

### 4. 工厂模式 (Factory Pattern)
- `Snapshot::create()`: 创建不同类型的快照
- `TransactionContext::create()`: 创建事务上下文

### 5. 观察者模式 (Observer Pattern)
- `WALEventListener`: WAL 事件监听
- `RecoveryMonitor`: 恢复过程监控

## 模块依赖层次

```
┌─────────────────────────────────────────┐
│          TransactionManager             │  ← 顶层：协调器
└─────────────────────────────────────────┘
              │
              ├──────────────────────────────────────┐
              │                                      │
┌─────────────────────────┐  ┌──────────────────────────────┐
│   TransactionContext    │  │      LockManager            │  ← 中层：核心组件
│   MVCCEngine           │  │      RecoveryManager        │
│   Snapshot             │  │      FileWAL                │
└─────────────────────────┘  └──────────────────────────────┘
              │                                      │
              ├──────────────────────────────────────┘
              │
┌─────────────────────────────────────────────────────────┐
│  LockTable                                               │  ← 底层：基础数据结构
│  DeadlockDetector                                        │
│  LockTableGraphBuilder                                   │
│  VectorCharHash                                          │
└─────────────────────────────────────────────────────────┘
```

## 总结

Transaction 模块采用分层架构设计：

1. **顶层**：`TransactionManager` 作为统一入口，协调所有组件
2. **中层**：核心功能组件（锁管理、MVCC、WAL、恢复）
3. **底层**：基础数据结构和工具类

这种设计实现了：
- ✅ **高内聚**：每个组件职责明确
- ✅ **低耦合**：组件间通过接口交互
- ✅ **可扩展**：支持多种算法和策略
- ✅ **可测试**：组件可独立测试

