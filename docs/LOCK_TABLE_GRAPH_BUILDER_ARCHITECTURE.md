# LockTableGraphBuilder 模块内部架构关系图

## 架构概览

LockTableGraphBuilder 是死锁检测系统的核心组件，负责从 LockTable（锁表）构建等待图（Wait-For Graph），为死锁检测器提供图数据。

## 核心组件关系图

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    LockTableGraphBuilder 架构                            │
└─────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────┐
│                  LockTableGraphBuilder                                   │
│  (等待图构建器 - 实现 WaitForGraphBuilder 接口)                          │
│                                                                          │
│  核心功能：                                                              │
│  - get_current_edges()          获取所有等待边                           │
│  - get_edges_for_transaction()  获取特定事务的边                         │
│  - get_active_transactions()    获取所有活跃事务                         │
│  - get_transaction_info()       获取事务信息                             │
│  - build_edges_from_lock_table() 从锁表构建边                            │
└─────────────────────────────────────────────────────────────────────────┘
                              │
                              │ 实现/使用
                              ├──────────────────┬──────────────────┬──────────────┐
                              ▼                  ▼                  ▼              ▼
                    ┌──────────────┐   ┌──────────────┐   ┌──────────────┐  ┌──────────────┐
                    │WaitForGraph  │   │LockTable     │   │Transaction   │  │Deadlock      │
                    │Builder       │   │              │   │Manager       │  │Detector      │
                    │(接口)        │   │              │   │              │  │              │
                    │              │   │- LockEntry   │   │- 获取事务     │  │- 检测死锁    │
                    │- get_current │   │- 持有者集合   │   │  上下文       │  │- 选择牺牲者  │
                    │  _edges()    │   │- 等待队列     │   │              │  │              │
                    │              │   │              │   │              │  │              │
                    │              │   │              │   │              │  │              │
                    └──────────────┘   └──────────────┘   └──────────────┘  └──────────────┘
                              │                  │                  │              │
                              │ 实现             │ 读取             │ 查询         │ 使用
                              ▼                  ▼                  ▼              ▼
                    ┌──────────────┐   ┌──────────────┐   ┌──────────────┐  ┌──────────────┐
                    │WaitForEdge   │   │LockEntry     │   │Transaction   │  │CycleDetector │
                    │              │   │              │   │Context       │  │              │
                    │- waiter      │   │- holders_    │   │              │  │- DFS         │
                    │- holder      │   │- wait_queue_ │   │- 写集合       │  │- Tarjan      │
                    │- resource    │   │              │   │- 锁引用       │  │              │
                    │- wait_time   │   │              │   │              │  │              │
                    └──────────────┘   └──────────────┘   └──────────────┘  └──────────────┘
```

## 详细组件关系

### 1. LockTableGraphBuilder (等待图构建器)

**职责**：从 LockTable 构建等待图，为死锁检测提供数据

**继承关系**：
```cpp
class LockTableGraphBuilder : public WaitForGraphBuilder {
    // 实现 WaitForGraphBuilder 接口
};
```

**依赖关系**：
```
LockTableGraphBuilder
├── LockTable (引用)              # 读取锁状态
├── TransactionManager (引用)     # 获取事务信息
└── WaitForGraphBuilder (继承)    # 实现接口
```

**核心方法**：
- `get_current_edges()`: 获取当前所有等待边
- `get_edges_for_transaction(tid)`: 获取特定事务相关的边
- `get_active_transactions()`: 获取所有活跃事务
- `get_transaction_info(tid)`: 获取事务详细信息
- `build_edges_from_lock_table()`: 从锁表构建边（核心逻辑）

---

### 2. WaitForGraphBuilder (抽象接口)

**职责**：定义等待图构建器的标准接口

**接口定义**：
```cpp
class WaitForGraphBuilder {
public:
    virtual ~WaitForGraphBuilder() = default;
    
    // 获取当前所有等待边
    virtual std::vector<WaitForEdge> get_current_edges() = 0;
    
    // 获取特定事务的等待边
    virtual std::vector<WaitForEdge> get_edges_for_transaction(
        TransactionID tid) = 0;
    
    // 获取所有活跃事务
    virtual std::vector<TransactionID> get_active_transactions() = 0;
    
    // 获取事务信息
    virtual TransactionInfo get_transaction_info(TransactionID tid) = 0;
};
```

**设计模式**：策略模式 + 接口隔离原则

---

### 3. WaitForEdge (等待边)

**职责**：表示等待图中的一条边

**结构定义**：
```cpp
struct WaitForEdge {
    TransactionID waiter;      // 等待锁的事务
    TransactionID holder;       // 持有锁的事务
    void* resource;            // 等待的资源（键）
    uint64_t wait_start_time;  // 等待开始时间
};
```

**边的含义**：
```
WaitForEdge(waiter=T1, holder=T2, resource="key:A")
  表示：事务 T1 等待事务 T2 释放资源 "key:A"
  
图表示：
  T1 ──waiting for──> T2 (on resource "key:A")
```

---

### 4. LockTable (锁表)

**职责**：存储和管理每个键上的锁信息

**关键方法**：
```cpp
class LockTable {
    // 获取所有键
    std::vector<std::string> get_all_keys();
    
    // 获取持有锁的事务
    std::vector<TransactionID> get_holders(const std::string& key);
    
    // 获取等待锁的事务
    std::vector<TransactionID> get_waiting_transactions(const std::string& key);
};
```

**内部结构**：
```
LockTable {
    std::unordered_map<std::string, LockEntry> locks_;
    
    LockEntry {
        std::unordered_set<TransactionID> holders_;      // 持有者集合
        std::queue<LockRequest> wait_queue_;            // 等待队列
        std::optional<TransactionID> exclusive_holder_; // 排他锁持有者
    }
}
```

---

### 5. TransactionManager (事务管理器)

**职责**：提供事务上下文信息

**关键方法**：
```cpp
class TransactionManager {
    // 获取事务上下文
    std::shared_ptr<TransactionContext> get_transaction(TransactionID tid);
};
```

**用途**：
- 获取事务的写集合大小（用于选择牺牲者）
- 获取事务的锁引用数量
- 获取事务的启动时间戳

---

## 核心构建流程

### 1. 构建等待边的流程

```
DeadlockDetector.detect()
  │
  └─→ LockTableGraphBuilder.get_current_edges()
         │
         └─→ build_edges_from_lock_table()
                │
                ├─→ 1. 获取所有键
                │      lock_table_->get_all_keys()
                │
                ├─→ 2. 遍历每个键
                │      for (const auto& key : keys)
                │
                ├─→ 3. 获取等待者
                │      waiters = lock_table_->get_waiting_transactions(key)
                │
                ├─→ 4. 获取持有者
                │      holders = lock_table_->get_holders(key)
                │
                ├─→ 5. 为每个等待者创建边
                │      for (waiter : waiters)
                │        for (holder : holders)
                │          edges.emplace_back(waiter, holder, resource, time)
                │
                └─→ 6. 返回边集合
                       return edges;
```

### 2. 构建示例

**锁表状态**：
```
Key: "A"
  Holders: [T1, T2]  (共享锁)
  Waiters: [T3]

Key: "B"
  Holders: [T2]      (排他锁)
  Waiters: [T4]

Key: "C"
  Holders: [T4]
  Waiters: [T1]
```

**构建的等待图**：
```
等待边：
  T3 -> T1  (等待 "A")
  T3 -> T2  (等待 "A")
  T4 -> T2  (等待 "B")
  T1 -> T4  (等待 "C")

图结构：
  T3 ──> T1 ──> T4 ──> T2 ──> T3  (死锁环！)
```

### 3. 死锁检测流程

```
DeadlockDetector.detect()
  │
  ├─→ 1. 构建等待图
  │      LockTableGraphBuilder.get_current_edges()
  │      └─→ 返回: [T3->T1, T3->T2, T4->T2, T1->T4]
  │
  ├─→ 2. 检测环
  │      CycleDetector.detect_cycles(edges)
  │      └─→ 返回: DeadlockInfo {
  │            cycle: [T1, T4, T2, T3],
  │            edges: [T1->T4, T4->T2, T2->T3, T3->T1]
  │          }
  │
  └─→ 3. 选择牺牲者
         VictimSelector.select_victim(deadlock_info)
         └─→ 返回: T3 (最年轻的事务)
```

---

## 数据结构详细设计

### 等待边集合

```
std::vector<WaitForEdge> edges;

edges = [
    WaitForEdge(waiter=T3, holder=T1, resource="A", time=1000),
    WaitForEdge(waiter=T3, holder=T2, resource="A", time=1000),
    WaitForEdge(waiter=T4, holder=T2, resource="B", time=1500),
    WaitForEdge(waiter=T1, holder=T4, resource="C", time=2000)
]
```

### 事务信息

```cpp
struct TransactionInfo {
    uint64_t start_time;           // 事务启动时间
    size_t modifications_count;    // 修改数量（写集合大小）
    size_t locks_held;             // 持有的锁数量
};
```

**获取事务信息**：
```
get_transaction_info(tid)
  → TransactionManager.get_transaction(tid)
  → TransactionContext {
        start_timestamp
        write_set.size()
        lock_references.size()
      }
```

---

## 关键方法详解

### 1. build_edges_from_lock_table()

**核心逻辑**：
```cpp
std::vector<WaitForEdge> build_edges_from_lock_table() {
    std::vector<WaitForEdge> edges;
    auto keys = lock_table_->get_all_keys();
    auto current_time = get_current_time_ms();
    
    for (const auto& key : keys) {
        auto waiters = lock_table_->get_waiting_transactions(key);
        auto holders = lock_table_->get_holders(key);
        
        // 为每个等待者创建到每个持有者的边
        for (TransactionID waiter : waiters) {
            for (TransactionID holder : holders) {
                void* resource = string_to_resource(key);
                edges.emplace_back(waiter, holder, resource, current_time);
            }
        }
    }
    
    return edges;
}
```

**时间复杂度**：O(K × (W × H))
- K: 键的数量
- W: 平均等待者数量
- H: 平均持有者数量

### 2. get_edges_for_transaction()

**功能**：获取特定事务相关的所有边

**实现**：
```cpp
std::vector<WaitForEdge> get_edges_for_transaction(TransactionID tid) {
    auto all_edges = build_edges_from_lock_table();
    std::vector<WaitForEdge> result;
    
    for (const auto& edge : all_edges) {
        if (edge.waiter == tid || edge.holder == tid) {
            result.push_back(edge);
        }
    }
    
    return result;
}
```

**用途**：
- 分析特定事务的等待关系
- 增量检测（只检测特定事务）

### 3. get_active_transactions()

**功能**：获取所有参与锁操作的事务

**实现**：
```cpp
std::vector<TransactionID> get_active_transactions() {
    std::unordered_set<TransactionID> transactions;
    auto keys = lock_table_->get_all_keys();
    
    for (const auto& key : keys) {
        auto holders = lock_table_->get_holders(key);
        auto waiters = lock_table_->get_waiting_transactions(key);
        
        for (TransactionID tid : holders) {
            transactions.insert(tid);
        }
        for (TransactionID tid : waiters) {
            transactions.insert(tid);
        }
    }
    
    return std::vector<TransactionID>(transactions.begin(), transactions.end());
}
```

### 4. get_transaction_info()

**功能**：获取事务的详细信息（用于选择牺牲者）

**实现**：
```cpp
TransactionInfo get_transaction_info(TransactionID tid) {
    TransactionInfo info;
    
    if (transaction_manager_) {
        auto ctx = transaction_manager_->get_transaction(tid);
        if (ctx) {
            info.start_time = ctx->get_start_timestamp();
            info.modifications_count = ctx->get_write_set().size();
            info.locks_held = ctx->get_lock_references().size();
        }
    }
    
    return info;
}
```

**用途**：
- 选择牺牲者时考虑事务的修改量
- 选择牺牲者时考虑事务的启动时间
- 选择牺牲者时考虑事务持有的锁数量

---

## 资源指针转换

### string_to_resource()

**功能**：将字符串键转换为 void* 资源指针

**实现**：
```cpp
void* string_to_resource(const std::string& key) {
    static std::unordered_map<std::string, void*> resource_map;
    static std::mutex map_mutex;
    
    std::lock_guard<std::mutex> lock(map_mutex);
    auto it = resource_map.find(key);
    if (it != resource_map.end()) {
        return it->second;
    }
    
    // 创建新的资源指针
    void* resource = const_cast<void*>(static_cast<const void*>(key.c_str()));
    resource_map[key] = resource;
    return resource;
}
```

**注意**：这是临时方案，实际应该使用更安全的方式（如资源ID）

---

## 与其他模块的交互

### 与 DeadlockDetector 的交互

```
DeadlockDetector
  │
  ├─→ LockTableGraphBuilder.get_current_edges()
  │      └─→ 返回等待边集合
  │
  ├─→ LockTableGraphBuilder.get_active_transactions()
  │      └─→ 返回活跃事务列表
  │
  └─→ LockTableGraphBuilder.get_transaction_info(tid)
         └─→ 返回事务信息（用于选择牺牲者）
```

### 与 LockManager 的交互

```
LockManager
  │
  ├─→ 创建 LockTableGraphBuilder
  │      LockTableGraphBuilder(lock_table_, transaction_manager_)
  │
  └─→ 传递给 DeadlockDetector
         DeadlockDetector(graph_builder)
```

### 与 LockTable 的交互

```
LockTableGraphBuilder
  │
  ├─→ lock_table_->get_all_keys()
  │      └─→ 获取所有键
  │
  ├─→ lock_table_->get_holders(key)
  │      └─→ 获取持有者
  │
  └─→ lock_table_->get_waiting_transactions(key)
         └─→ 获取等待者
```

---

## 使用场景

### 场景1：定期死锁检测

```
定时器触发 (每1秒)
  │
  └─→ DeadlockDetector.detect()
         │
         └─→ LockTableGraphBuilder.get_current_edges()
                │
                └─→ 构建完整的等待图
                      │
                      └─→ 检测死锁环
```

### 场景2：按需死锁检测

```
LockManager.acquire_lock() 失败
  │
  └─→ DeadlockDetector.detect()
         │
         └─→ LockTableGraphBuilder.get_edges_for_transaction(tid)
                │
                └─→ 只构建相关事务的边
                      │
                      └─→ 增量检测
```

### 场景3：选择牺牲者

```
检测到死锁
  │
  └─→ VictimSelector.select_victim(deadlock_info)
         │
         └─→ 对环中每个事务
               │
               └─→ LockTableGraphBuilder.get_transaction_info(tid)
                      │
                      └─→ 获取事务信息
                            │
                            └─→ 根据策略选择牺牲者
```

---

## 性能考虑

### 1. 构建效率

**当前实现**：每次调用都重新构建所有边

**优化方向**：
- 增量更新：只更新变化的边
- 缓存：缓存最近构建的图
- 并行构建：并行处理多个键

### 2. 内存使用

**当前实现**：构建完整的边集合

**优化方向**：
- 延迟构建：只在需要时构建
- 流式处理：边生成后立即处理，不全部存储

### 3. 资源指针管理

**当前实现**：使用字符串地址作为资源指针

**优化方向**：
- 使用资源ID：为每个键分配唯一ID
- 资源池：管理资源指针的生命周期

---

## 设计模式

### 1. 策略模式 (Strategy Pattern)

```
WaitForGraphBuilder (接口)
  └─ LockTableGraphBuilder (实现)
  
可以替换为其他实现：
  - DistributedGraphBuilder (分布式)
  - OptimizedGraphBuilder (优化版)
```

### 2. 适配器模式 (Adapter Pattern)

```
LockTable (锁表)
  └─ LockTableGraphBuilder (适配器)
      └─ WaitForGraph (等待图)
```

### 3. 依赖注入 (Dependency Injection)

```
LockTableGraphBuilder(
    LockTable* lock_table,              // 注入锁表
    TransactionManager* transaction_mgr // 注入事务管理器
)
```

---

## 总结

LockTableGraphBuilder 的核心设计特点：

1. **接口抽象**：实现 WaitForGraphBuilder 接口，支持多种实现
2. **数据转换**：将 LockTable 的锁状态转换为等待图
3. **信息聚合**：从多个源（LockTable、TransactionManager）聚合信息
4. **灵活查询**：支持全量查询和增量查询
5. **资源管理**：处理字符串到资源指针的转换

这种设计实现了：
- ✅ **解耦**：死锁检测器不直接依赖 LockTable
- ✅ **可扩展**：可以替换不同的图构建器实现
- ✅ **高效**：按需构建，支持增量更新
- ✅ **灵活**：支持多种查询模式

