#pragma once

// File: src/core/Page.hpp
// 页结构（磁盘/内存基本单元）

#include <cstdint>
#include <cstddef>
#include "../memory/Arena.hpp"

namespace mementodb {
namespace core {
    // 数据库页大小（固定4KB，匹配文件系统块）
constexpr size_t kPageSize = 4096;

// 页类型枚举
enum class PageType : uint8_t {
    INVALID = 0,      // 无效页
    FREE = 1,          // 空闲页
    INTERNAL = 2,      // B+树内部节点
    LEAF = 3,          // B+树叶节点
    OVERFLOW = 4,      // 溢出页（处理大记录）
    DATA = 5           // 数据页（存储键值对记录）
};

// 页头结构（必须固定大小，以便序列化）
// 使用 packed 属性确保紧凑布局，避免对齐导致的额外空间
struct __attribute__((packed)) PageHeader {
    PageType type;          // 页类型 (1字节)
    uint8_t padding1[7];   // 填充到8字节对齐
    uint64_t page_id;       // 页号 (8字节)
    uint32_t parent_id;     // 父页页号 (4字节)
    uint16_t key_count;     // 键的数量 (2字节)
    uint16_t free_offset;   // 空闲空间起始偏移 (2字节)
    
    // 叶子节点特有
    uint32_t next_leaf;     // 下一个叶节点 (4字节)
    uint32_t prev_leaf;     // 上一个叶节点 (4字节)
    
    // 内部节点特有
    uint32_t first_child;   // 第一个子节点 (4字节)
    
    // 校验和（可选，用于数据完整性）
    uint32_t checksum;      // 页数据校验和 (4字节)
    
    // 填充字节，使结构体大小为64字节（缓存行对齐）
    // 计算：1 + 7 + 8 + 4 + 2 + 2 + 4 + 4 + 4 + 4 = 40字节，剩余24字节用于reserved
    uint8_t reserved[24];
};
static_assert(sizeof(PageHeader) == 64, "PageHeader must be 64 bytes");

// 页类（内存表示）
// 使用 Arena 管理数据区，而不是在类中定义固定大小的数组
// 这样可以减少栈上大对象的内存占用，提供更灵活的内存管理
class Page {
public:
    // 构造函数
    Page();
    explicit Page(uint64_t page_id);
    
    // 禁止拷贝（每页都是唯一的）
    Page(const Page&) = delete;
    Page& operator=(const Page&) = delete;
    
    // 允许移动
    Page(Page&& other) noexcept;
    Page& operator=(Page&& other) noexcept;
    
    // 析构函数：释放 Arena 分配的数据区
    ~Page();
    
    // ========== 基本访问 ==========
    uint64_t GetPageId() const { return header_.page_id; }
    PageType GetType() const { return header_.type; }
    void SetType(PageType type) { header_.type = type; }
    
    // ========== 数据操作 ==========
    // 获取页内可用空间
    size_t GetFreeSpace() const;
    
    // 获取/设置空闲偏移量
    uint16_t GetFreeOffset() const { return header_.free_offset; }
    void SetFreeOffset(uint16_t offset) { header_.free_offset = offset; }
    
    // 获取数据指针（用于B+树操作）
    char* GetData() { return data_; }
    const char* GetData() const { return data_; }
    
    // 序列化/反序列化（用于磁盘读写）
    void SerializeTo(char* dest) const;   // 页→字节流
    void DeserializeFrom(const char* src); // 字节流→页
    
    // ========== B+树相关 ==========
    bool IsLeaf() const { return header_.type == PageType::LEAF; }
    bool IsInternal() const { return header_.type == PageType::INTERNAL; }
    
    uint16_t GetKeyCount() const { return header_.key_count; }
    void SetKeyCount(uint16_t count) { header_.key_count = count; }
    
    // 获取/设置兄弟叶子指针（用于范围扫描）
    uint32_t GetNextLeaf() const { return header_.next_leaf; }
    void SetNextLeaf(uint32_t page_id) { header_.next_leaf = page_id; }
    
    uint32_t GetPrevLeaf() const { return header_.prev_leaf; }
    void SetPrevLeaf(uint32_t page_id) { header_.prev_leaf = page_id; }
    
private:
    PageHeader header_;                          // 页头信息（64字节，栈上分配）
    
    // 数据区通过 Arena 分配（4032字节）
    // 使用 Arena 的好处：
    // 1. 减少栈上大对象的内存占用
    // 2. 更灵活的内存管理
    // 3. 可以延迟分配（如果需要）
    // 4. 更好的内存对齐控制
    Arena data_arena_;                          // 用于分配数据区的 Arena
    char* data_;                                // 指向数据区的指针（由 Arena 管理）
    static constexpr size_t kDataAreaSize = kPageSize - sizeof(PageHeader);  // 4032字节
    
    // 计算校验和（简单的FNV-1a）
    uint32_t CalculateChecksum() const;
    
    // 初始化数据区（在构造函数中调用）
    void InitializeDataArea();
};


} // namespace core


} // namespace mementodb

