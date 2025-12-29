# Core 模块文档

## 概述

Core 模块是 MementoDB 的核心存储层，负责数据的物理存储、索引管理和磁盘 I/O。本模块提供了从底层文件操作到上层存储引擎的完整实现。

---

## 文件结构

```
src/core/
├── Record.hpp/cpp      # 记录存储格式
├── Page.hpp/cpp        # 页管理
├── FileManager.hpp/cpp # 文件系统管理
├── BPlusTree.hpp/cpp  # B+树索引
├── DiskEngine.hpp/cpp # 磁盘存储引擎
└── test_core.hpp       # 测试套件
```

---

## 1. Record - 记录存储格式

**文件**: `Record.hpp` / `Record.cpp`

### 作用

定义键值对在页内的存储格式，提供高效的序列化/反序列化功能。

### 主要类

#### `Record`

静态工具类，提供编码/解码功能。

**核心方法**:

- `static size_t ByteSize(uint32_t key_size, uint32_t value_size)`
  - 计算存储指定 key/value 所需的总字节数
  
- `static void Encode(char* dst, const Slice& key, const Slice& value)`
  - 将 key/value 编码到目标缓冲区
  - 缓冲区需至少 `ByteSize` 大小
  
- `static bool Decode(const char* buf, size_t buf_len, Slice* key_out, Slice* val_out)`
  - 从缓冲区解码 Record
  - 返回 key/value 的 Slice（指向原缓冲区，零拷贝）
  - 成功返回 `true`，长度不足返回 `false`

**存储格式**:

```
内存布局：
┌─────────────────────────────────────────────────┐
│ Header (8字节)                                  │
│  ├─ key_size (uint32, 4字节)                    │
│  └─ value_size (uint32, 4字节)                  │
├─────────────────────────────────────────────────┤
│ Key Data (key_size 字节)                        │
├─────────────────────────────────────────────────┤
│ Value Data (value_size 字节)                    │
└─────────────────────────────────────────────────┘
```

**特点**:

- ✅ 无额外对齐要求（按字节紧凑存储）
- ✅ Header 固定 8 字节
- ✅ 支持零拷贝解码（返回 Slice 指向原缓冲区）
- ✅ 高效的内存使用

**使用示例**:

```cpp
// 编码
Slice key("user:123");
Slice value("Alice");
size_t size = Record::ByteSize(key.size(), value.size());
char buffer[size];
Record::Encode(buffer, key, value);

// 解码（零拷贝）
Slice decoded_key, decoded_value;
bool success = Record::Decode(buffer, size, &decoded_key, &decoded_value);
```

---

## 2. Page - 页管理

**文件**: `Page.hpp` / `Page.cpp`

### 作用

数据库页的内存表示，是数据库存储的基本单元（4KB）。每个页包含页头信息和数据区，支持序列化到磁盘。

### 主要组件

#### `PageType` 枚举

页类型定义：

```cpp
enum class PageType : uint8_t {
    INVALID = 0,    // 无效页
    FREE = 1,       // 空闲页
    INTERNAL = 2,   // B+树内部节点
    LEAF = 3,       // B+树叶节点
    OVERFLOW = 4    // 溢出页（处理大记录）
};
```

#### `PageHeader` 结构体

64 字节的页头结构（缓存行对齐）：

```cpp
struct PageHeader {
    PageType type;          // 页类型 (1字节)
    uint64_t page_id;       // 页号 (8字节)
    uint32_t parent_id;     // 父页页号 (4字节)
    uint16_t key_count;     // 键的数量 (2字节)
    uint16_t free_offset;   // 空闲空间起始偏移 (2字节)
    
    // 叶子节点特有
    uint32_t next_leaf;     // 下一个叶节点 (4字节)
    uint32_t prev_leaf;     // 上一个叶节点 (4字节)
    
    // 内部节点特有
    uint32_t first_child;   // 第一个子节点 (4字节)
    
    // 校验和（用于数据完整性）
    uint32_t checksum;      // 页数据校验和 (4字节)
    
    // 填充字节，使结构体大小为64字节
    // 计算：1 + 8 + 4 + 2 + 2 + 4 + 4 + 4 + 4 = 33字节，剩余31字节用于reserved
    uint8_t reserved[31];
};
```

**页结构**:

```
┌─────────────────────────────────────────┐
│ PageHeader (64字节)                     │
│  ├─ type, page_id, parent_id            │
│  ├─ key_count, free_offset              │
│  ├─ next_leaf, prev_leaf (叶子节点)     │
│  ├─ first_child (内部节点)               │
│  └─ checksum                            │
├─────────────────────────────────────────┤
│ Data Area (4032字节)                    │
│  └─ 存储 Record 或其他数据               │
└─────────────────────────────────────────┘
总大小: 4096 字节 (4KB)
```

#### `Page` 类

页的内存表示类。

**核心方法**:

- `size_t GetFreeSpace() const`
  - 获取页内可用空间（字节数）
  
- `void SerializeTo(char* dest) const`
  - 将页序列化为字节流（用于写入磁盘）
  
- `void DeserializeFrom(const char* src)`
  - 从字节流反序列化页（用于从磁盘读取）
  - 自动验证校验和
  
- `uint32_t CalculateChecksum() const`
  - 计算页的校验和（FNV-1a 算法）
  - 用于数据完整性检查

**B+树相关方法**:

- `bool IsLeaf() const` / `bool IsInternal() const`
- `uint16_t GetKeyCount() const` / `void SetKeyCount(uint16_t count)`
- `uint32_t GetNextLeaf() const` / `void SetNextLeaf(uint32_t page_id)`
- `uint32_t GetPrevLeaf() const` / `void SetPrevLeaf(uint32_t page_id)`

**使用示例**:

```cpp
// 创建页
Page page(1);  // 页号 1
page.SetType(PageType::LEAF);
page.SetKeyCount(10);

// 序列化
char buffer[kPageSize];
page.SerializeTo(buffer);

// 反序列化
Page page2;
page2.DeserializeFrom(buffer);
```

---

## 3. FileManager - 文件系统管理

**文件**: `FileManager.hpp` / `FileManager.cpp`

### 作用

封装底层文件 I/O 操作，提供高性能的文件读写、内存映射、异步 I/O 等功能。支持跨平台（Linux/Windows）。

### 主要类

#### `FileManagerConfig`

文件管理器配置结构：

```cpp
struct FileManagerConfig {
    // 文件打开选项
    bool use_direct_io = false;        // 使用直接IO（O_DIRECT）
    bool use_async_io = false;         // 使用异步IO
    bool use_memory_mapping = true;    // 使用内存映射
    bool use_write_through = false;     // 直写模式
    
    // 性能调优
    size_t max_open_files = 1024;      // 最大打开文件数
    size_t io_thread_count = 4;        // IO线程数
    size_t max_mapped_memory = 256MB;  // 最大映射内存
    
    // 文件扩展策略
    size_t default_extent_size = 64MB; // 默认扩展大小
    size_t max_file_size = 4TB;       // 最大文件大小
    
    // 缓存策略
    bool enable_file_cache = true;     // 启用文件缓存
    
    // 恢复配置
    bool enable_atomic_write = true;   // 启用原子写入
    bool enable_data_integrity = true; // 启用数据完整性检查
};
```

#### `FileHandle`

文件句柄类，提供完整的文件操作接口。

**核心方法**:

**基础 I/O**:
- `size_t read(void* buffer, size_t size)` - 读取数据
- `size_t write(const void* buffer, size_t size)` - 写入数据
- `size_t pread(void* buffer, size_t size, uint64_t offset)` - 定位读取
- `size_t pwrite(const void* buffer, size_t size, uint64_t offset)` - 定位写入

**向量 I/O**:
- `size_t readv(const std::span<iovec>& iov)` - scatter 读取
- `size_t writev(const std::span<const iovec>& iov)` - gather 写入

**文件控制**:
- `uint64_t seek(int64_t offset, SeekOrigin origin)` - 移动文件指针
- `void flush()` - 刷新缓冲区
- `void sync()` - 同步到磁盘
- `void truncate(uint64_t size)` - 截断文件
- `void allocate(uint64_t offset, uint64_t length)` - 预分配空间

**内存映射**:
- `void* map(uint64_t offset = 0, size_t length = 0)` - 映射文件到内存
- `void unmap(void* addr, size_t length)` - 取消映射
- `void sync_mapped(void* addr, size_t length, bool async = false)` - 同步映射区域

**文件锁**:
- `bool lock_shared(uint64_t offset, uint64_t length, bool wait)` - 共享锁
- `bool lock_exclusive(uint64_t offset, uint64_t length, bool wait)` - 排他锁
- `bool unlock(uint64_t offset, uint64_t length)` - 解锁

**异步操作**:
- `std::future<size_t> async_read(...)` - 异步读取
- `std::future<size_t> async_write(...)` - 异步写入

**零拷贝**:
- `size_t send_to(FileHandle& dest, ...)` - 使用 sendfile 零拷贝传输

**使用示例**:

```cpp
// 打开文件
FileHandle file("data.db", FileHandle::OpenMode::ReadWrite);

// 写入数据
char buffer[4096];
file.pwrite(buffer, 4096, 0);

// 内存映射
void* mapped = file.map(0, 4096);
// 直接操作映射内存
file.unmap(mapped, 4096);

// 异步读取
auto future = file.async_read(buffer, 4096, 0);
size_t bytes = future.get();
```

---

## 4. BPlusTree - B+树索引

**文件**: `BPlusTree.hpp` / `BPlusTree.cpp`

### 作用

实现 B+ 树索引结构，提供高效的有序数据存储和查询功能。支持点查询、范围查询和范围扫描。

### 主要组件

#### `BPlusTreeConfig`

B+ 树配置：

```cpp
struct BPlusTreeConfig {
    int order = 4;                  // 阶数，决定节点容量
    int leaf_order = 3;            // 叶子节点容量
    bool use_verbose_log = false;  // 调试日志开关
    std::string storage_path;      // 持久化存储路径
};
```

#### `KeyValuePair<KeyType, ValueType>`

键值对模板结构。

#### `Node<KeyType, ValueType>`

节点基类，定义通用接口：

- `InternalNode<KeyType, ValueType>` - 内部节点
  - 存储键和子节点指针
  - 用于路由查找
  
- `LeafNode<KeyType, ValueType>` - 叶子节点
  - 存储实际的键值对
  - 通过链表连接，支持范围扫描

#### `BPlusTree<KeyType, ValueType>`

B+ 树主类。

**核心方法**:

- `bool Insert(const KeyType& key, const ValueType& value)`
  - 插入键值对
  
- `bool Delete(const KeyType& key)`
  - 删除指定键
  
- `ValueType* Search(const KeyType& key)`
  - 查找指定键的值
  
- `std::vector<KeyValuePair> RangeQuery(const KeyType& start, const KeyType& end)`
  - 范围查询

**序列化**:
- `void Serialize(std::ostream& os) const` - 序列化到流
- `void Deserialize(std::istream& is)` - 从流反序列化

**B+ 树结构**:

```
                    [内部节点]
                  /    |    \
            [内部节点] [内部节点] [内部节点]
           /  |  \    /  |  \    /  |  \
    [叶子] [叶子] [叶子] [叶子] [叶子] [叶子]
      ↓      ↓      ↓      ↓      ↓      ↓
    [链表连接，支持范围扫描]
```

**使用示例**:

```cpp
BPlusTree<std::string, std::string> tree;

// 插入
tree.Insert("user:1", "Alice");
tree.Insert("user:2", "Bob");

// 查找
auto* value = tree.Search("user:1");

// 范围查询
auto results = tree.RangeQuery("user:1", "user:100");
```

**注意**: 当前实现在 `BPlusTree` 命名空间中，可能需要迁移到 `mementodb::core` 命名空间以保持一致性。

---

## 5. DiskEngine - 磁盘存储引擎

**文件**: `DiskEngine.hpp` / `DiskEngine.cpp`

### 作用

数据库的核心存储引擎，管理页缓存、磁盘 I/O、事务和恢复。提供高性能的页读写、缓冲池管理和数据持久化。

### 主要组件

#### `EngineConfig`

引擎配置结构：

```cpp
struct EngineConfig {
    // 存储配置
    size_t page_size = 4096;               // 页大小（4KB）
    size_t extent_size = 64MB;             // 区大小（64MB）
    size_t max_file_size = 4TB;            // 最大文件大小（4TB）
    
    // 缓冲池配置
    size_t buffer_pool_size = 1M页;        // 缓冲池大小（4GB）
    size_t hot_pool_size = 16K页;          // 热数据池（64MB）
    
    // 性能配置
    bool use_direct_io = true;             // 直接IO
    bool use_mmap = true;                  // 内存映射
    bool use_io_uring = true;               // io_uring异步IO
    bool use_zero_copy = true;             // 零拷贝传输
    
    // 刷盘策略
    enum class FlushStrategy {
        Lazy,           // 惰性刷盘
        Periodic,       // 定期刷盘
        WriteThrough,   // 直写
        WriteBack       // 回写
    } flush_strategy = FlushStrategy::WriteBack;
    
    // WAL配置
    bool enable_wal = true;                // 启用WAL
    size_t wal_buffer_size = 64MB;         // WAL缓冲区大小
    size_t wal_segment_size = 256MB;       // WAL段大小
    
    // 压缩配置
    bool enable_compression = false;
    enum class CompressionType {
        None, LZ4, ZSTD, Snappy
    } compression_type = CompressionType::LZ4;
};
```

#### `AlignedAllocator<T>`

对齐内存分配器，用于分配对齐的内存（如页对齐）。

#### `PageFrame`

页帧类，表示缓冲池中的一个页：

```cpp
class PageFrame {
    PageHeader* header();           // 获取页头
    char* payload();                // 获取数据区
    size_t payload_size() const;    // 数据区大小
    
    uint32_t compute_checksum() const;  // 计算校验和
    bool verify_checksum() const;      // 验证校验和
    void update_checksum();             // 更新校验和
    
    bool compress();    // 压缩页
    bool decompress();  // 解压页
};
```

#### `AsyncIOWrapper`

异步 I/O 包装器，封装 io_uring 异步 I/O 操作。

#### `DiskEngineV2`

主引擎类。

**核心方法**:

**页操作**:
- `PageFrame* ReadPage(uint64_t page_id)` - 读取页（从缓冲池或磁盘）
- `void WritePage(uint64_t page_id, const PageFrame& page)` - 写入页
- `uint64_t AllocatePage()` - 分配新页
- `void FreePage(uint64_t page_id)` - 释放页

**缓冲池管理**:
- `PageFrame* GetPageFromPool(uint64_t page_id)` - 从缓冲池获取页
- `void EvictPage(uint64_t page_id)` - 驱逐页（LRU）
- `void MarkDirty(uint64_t page_id)` - 标记脏页

**刷盘操作**:
- `void Flush()` - 刷新所有脏页
- `void Sync()` - 同步到磁盘
- `void FlushPage(uint64_t page_id)` - 刷新指定页

**统计信息**:
- `IOStats GetStats() const` - 获取 I/O 统计
- `BufferPoolStats GetBufferPoolStats() const` - 获取缓冲池统计

**使用示例**:

```cpp
// 创建引擎
EngineConfig config;
config.buffer_pool_size = 1024 * 1024;  // 1M页
DiskEngineV2 engine("data.db", config);

// 读取页
PageFrame* page = engine.ReadPage(1);

// 修改页数据
memcpy(page->payload(), data, size);
page->update_checksum();
engine.MarkDirty(1);

// 刷盘
engine.Flush();
```

---

## 模块关系图

```
┌─────────────────────────────────────────┐
│         DiskEngine (顶层引擎)            │
│  - 管理缓冲池                            │
│  - 协调所有存储操作                       │
└──────────────┬──────────────────────────┘
               │
       ┌───────┴────────┐
       │                │
┌──────▼──────┐  ┌──────▼────────┐
│    Page     │  │  BPlusTree    │
│  - 页管理    │  │  - 索引结构    │
│  - 序列化    │  │  - 使用Page    │
└──────┬──────┘  └────────────────┘
       │
┌──────▼──────┐
│   Record    │
│  - 编码/解码 │
│  - 零拷贝    │
└──────┬──────┘
       │
┌──────▼──────────┐
│  FileManager    │
│  - 文件I/O      │
│  - 内存映射      │
│  - 异步IO       │
└─────────────────┘
```

---

## 数据流向

### 写入流程

```
1. 用户调用 DiskEngine::WritePage()
   ↓
2. DiskEngine 从缓冲池获取 PageFrame
   ↓
3. 使用 Record::Encode() 编码数据到 Page
   ↓
4. 标记页为脏页
   ↓
5. 根据刷盘策略，调用 FileManager 写入磁盘
   ↓
6. FileManager 使用底层 I/O（直接IO/内存映射/异步IO）
```

### 读取流程

```
1. 用户调用 DiskEngine::ReadPage()
   ↓
2. DiskEngine 检查缓冲池（LRU缓存）
   ↓
3. 缓存未命中 → 从磁盘读取
   ↓
4. FileManager 读取文件数据
   ↓
5. 反序列化为 PageFrame
   ↓
6. 使用 Record::Decode() 解码数据（零拷贝）
   ↓
7. 返回 PageFrame 给用户
```

### B+树操作流程

```
1. BPlusTree::Insert(key, value)
   ↓
2. 查找合适的叶子节点（使用 Page）
   ↓
3. 在 Page 中使用 Record 存储键值对
   ↓
4. 节点分裂时，通过 DiskEngine 分配新页
   ↓
5. DiskEngine 管理页的持久化
```

---

## 性能特性

### 1. 零拷贝设计

- `Record::Decode()` 返回 `Slice`，直接指向原始缓冲区
- 避免不必要的内存拷贝

### 2. 缓冲池管理

- LRU 替换策略
- 热数据池分离
- 预取机制

### 3. 多种 I/O 模式

- **直接 I/O**: 绕过系统缓存，适合大文件
- **内存映射**: 减少系统调用，适合随机访问
- **异步 I/O**: io_uring，高并发场景

### 4. 数据完整性

- 页级校验和（FNV-1a / XXH32）
- WAL（Write-Ahead Logging）
- 原子写入

### 5. 压缩支持

- 页级压缩（LZ4/ZSTD/Snappy）
- 透明压缩/解压

---

## 测试

运行测试套件：

```bash
./run.sh
```

测试文件: `test_core.hpp`

测试覆盖：
- ✅ Record 编码/解码
- ✅ Page 序列化/反序列化
- ✅ Record 在 Page 中的存储
- ✅ 页类型和属性
- ✅ 叶子节点链表

---

## 注意事项

1. **页大小**: 固定 4KB，与文件系统块大小对齐
2. **页头大小**: 固定 64 字节，缓存行对齐
3. **命名空间**: 所有类在 `mementodb::core` 命名空间中
4. **线程安全**: FileManager 和 DiskEngine 支持多线程访问
5. **错误处理**: 使用异常机制，需要适当的错误处理

---

## 扩展建议

1. **WAL 实现**: 完整的 Write-Ahead Logging
2. **事务支持**: ACID 事务管理
3. **并发控制**: 多版本并发控制（MVCC）
4. **压缩优化**: 更智能的压缩策略
5. **监控指标**: 更详细的性能指标收集

---

## 相关文档

- [Arena 内存分配器说明](../memory/Arena说明.md)
- [Utils 工具模块说明](../utils/README.md)

---

**最后更新**: 2024年

