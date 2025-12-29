# Arena 内存池分配器

## 📚 概述

Arena 是一个高性能的内存池分配器，用于减少内存碎片、提升分配效率。

## ✨ 特性

- **快速分配**：O(1) 时间复杂度，只需移动指针
- **减少碎片**：连续分配，避免内存碎片化
- **批量释放**：析构时一次性释放所有内存
- **对齐支持**：支持内存对齐分配

## 🚀 使用方法

### 基本使用

```cpp
#include "memory/Arena.h"
using namespace utils;

// 创建 Arena（默认块大小 4KB）
Arena arena;

// 分配内存
char* buf1 = arena.Allocate(100);  // 分配 100 字节
char* buf2 = arena.Allocate(200);  // 分配 200 字节

// 使用内存...
strcpy(buf1, "Hello");
strcpy(buf2, "World");

// Arena 析构时自动释放所有内存
```

### 自定义块大小

```cpp
// 创建 8KB 块的 Arena
Arena arena(8192);

char* buf = arena.Allocate(1000);
```

### 对齐分配

```cpp
Arena arena;

// 对齐到 16 字节边界
char* aligned_buf = arena.AllocateAligned(100, 16);

// 验证对齐
uintptr_t addr = reinterpret_cast<uintptr_t>(aligned_buf);
assert((addr % 16) == 0);
```

### 重置 Arena

```cpp
Arena arena;

// 分配一些内存
arena.Allocate(100);
arena.Allocate(200);

// 重置（释放除第一个块外的所有块）
arena.Reset();

// 现在可以重新使用
arena.Allocate(50);
```

### 查询内存使用

```cpp
Arena arena;
arena.Allocate(100);
arena.Allocate(200);

size_t total = arena.MemoryUsage();  // 获取总内存使用量
size_t current = arena.CurrentBlockUsed();  // 当前块已使用量
```

## 📊 工作原理

```
传统方式（malloc）：
┌─────┐  ┌─────┐  ┌─────┐  ┌─────┐
│ 100 │  │ 50  │  │ 200 │  │ 100 │  ← 内存碎片化
└─────┘  └─────┘  └─────┘  └─────┘

Arena 方式：
┌─────────────────────────────────┐
│        大块连续内存 (4KB)        │
│  ┌───┐┌───┐┌────┐┌───┐┌───┐     │
│  │100││50 ││200 ││100││...│     │  ← 连续分配，无碎片
│  └───┘└───┘└────┘└───┘└───┘     │
└─────────────────────────────────┘
      ↑
   指针移动，O(1) 分配
```

## 🎯 适用场景

### ✅ 适合使用 Arena

- **临时对象分配**：解析、测试数据生成
- **大量小对象**：B+树节点操作、网络协议解析
- **短期使用**：函数内部临时分配，函数结束时释放

### ❌ 不适合使用 Arena

- **长期持有**：需要长期保存的对象
- **单独释放**：需要单独释放某些对象
- **大小差异很大**：对象大小差异很大（浪费空间）

## 📝 在 Random 类中的使用

```cpp
// Random::NextSlice() 使用 Arena 分配内存
Slice Random::NextSlice(size_t min_len, size_t max_len) {
    size_t length = Uniform(min_len, max_len);
    
    // 从 Arena 分配（快速，无碎片）
    char* buf = arena_->Allocate(length);
    NextBytes(buf, length);
    
    return Slice(buf, length);
}
```

## ⚠️ 注意事项

1. **内存生命周期**：Arena 分配的内存只在 Arena 存在期间有效
2. **不能单独释放**：只能通过 Arena 析构或 Reset() 批量释放
3. **线程安全**：当前实现不是线程安全的，多线程使用需要加锁
4. **内存对齐**：默认分配的内存可能不对齐，需要对齐时使用 AllocateAligned()

## 🔧 API 参考

### 构造函数

```cpp
explicit Arena(size_t block_size = 4096);
```

### 分配方法

```cpp
char* Allocate(size_t bytes);
char* AllocateAligned(size_t bytes, size_t alignment);
```

### 管理方法

```cpp
void Reset();  // 重置 Arena
size_t MemoryUsage() const;  // 总内存使用量
size_t CurrentBlockUsed() const;  // 当前块已使用量
```

## 📈 性能对比

| 操作 | malloc/free | Arena |
|------|------------|-------|
| 分配速度 | 较慢（需要查找空闲内存） | 极快（O(1)，只需移动指针） |
| 内存碎片 | 严重 | 无碎片 |
| 释放速度 | 慢（需要合并空闲块） | 极快（批量释放） |

## 🎉 总结

Arena 是数据库系统中重要的内存管理工具，特别适合：
- 解析操作（网络请求、日志解析）
- 临时数据结构（B+树操作、排序）
- 测试数据生成（Random::NextSlice）

通过使用 Arena，可以显著提升性能并减少内存碎片！

