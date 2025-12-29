// File: src/core/Page.cpp
#include "Page.hpp"
#include "utils/Coding.hpp"
#include <cstring>
#include <algorithm>
#include <cstddef>  // for offsetof
#include <stdexcept>  // for std::runtime_error

namespace mementodb {
namespace core {
// ==================== 构造函数与析构函数 ====================

Page::Page() {
    // 初始化数据区（使用 Arena 分配）
    InitializeDataArea();
    
    // 清零初始化页头
    std::memset(&header_, 0, sizeof(PageHeader));
    
    // 设置默认值
    header_.type = PageType::INVALID;
    header_.page_id = static_cast<uint64_t>(-1); // 无效页ID
    header_.free_offset = 0;
    header_.key_count = 0;
    
    // 初始化指针为无效值
    header_.next_leaf = static_cast<uint32_t>(-1);
    header_.prev_leaf = static_cast<uint32_t>(-1);
    header_.first_child = static_cast<uint32_t>(-1);
    header_.parent_id = static_cast<uint32_t>(-1);
    
    // 计算空页的校验和
    header_.checksum = CalculateChecksum();
}

Page::Page(uint64_t page_id) : Page() {
    header_.page_id = page_id;
}

Page::~Page() {
    // Arena 会自动释放所有分配的内存
    // data_arena_ 析构时会清理所有内存块
}

// 移动构造函数
Page::Page(Page&& other) noexcept 
    : header_(other.header_),
      data_arena_(std::move(other.data_arena_)),
      data_(other.data_) {
    // 移动 Arena 后，数据区指针已经转移
    // 清空原对象（避免双重释放）
    std::memset(&other.header_, 0, sizeof(PageHeader));
    other.data_ = nullptr;  // 原对象不再拥有数据区
    other.header_.page_id = static_cast<uint64_t>(-1);
    other.header_.type = PageType::INVALID;
}

// 移动赋值运算符
Page& Page::operator=(Page&& other) noexcept {
    if (this != &other) {
        // 先释放当前资源（Arena 析构会自动清理）
        // 然后移动资源
        header_ = other.header_;
        data_arena_ = std::move(other.data_arena_);
        data_ = other.data_;
        
        // 清空原对象
        std::memset(&other.header_, 0, sizeof(PageHeader));
        other.data_ = nullptr;
        other.header_.page_id = static_cast<uint64_t>(-1);
        other.header_.type = PageType::INVALID;
    }
    return *this;
}

// 初始化数据区（私有辅助方法）
void Page::InitializeDataArea() {
    // 使用 Arena 分配数据区（4032字节）
    // Arena 默认块大小是4KB，足够分配数据区
    data_ = data_arena_.Allocate(kDataAreaSize);
    
    // 清零初始化数据区
    std::memset(data_, 0, kDataAreaSize);
}

// ==================== 核心方法实现 ====================

size_t Page::GetFreeSpace() const {
    // 可用空间 = 总数据区大小 - 当前已用偏移量
    if (header_.free_offset > kDataAreaSize) {
        // 数据损坏保护：返回0，避免后续操作越界
        return 0;
    }
    return kDataAreaSize - header_.free_offset;
}

void Page::SerializeTo(char* dest) const {
    // 临时缓冲区，用于计算校验和
    PageHeader header_copy = header_;
    
    // 计算当前数据的校验和
    header_copy.checksum = CalculateChecksum();
    
    // 1. 序列化页头（按字段逐个编码，保证跨平台一致性）
    char* ptr = dest;
    
    // type (1字节)
    *ptr = static_cast<uint8_t>(header_copy.type);
    ptr += 1;
    
    // padding1[7] (7字节) - 对齐到8字节
    std::memset(ptr, 0, 7);
    ptr += 7;
    
    // page_id (8字节)
    utils::Coding::EncodeFixed64(ptr, header_copy.page_id);
    ptr += 8;
    
    // parent_id (4字节)
    utils::Coding::EncodeFixed32(ptr, header_copy.parent_id);
    ptr += 4;
    
    // key_count (2字节)
    utils::Coding::EncodeFixed16(ptr, header_copy.key_count);
    ptr += 2;
    
    // free_offset (2字节)
    utils::Coding::EncodeFixed16(ptr, header_copy.free_offset);
    ptr += 2;
    
    // next_leaf (4字节)
    utils::Coding::EncodeFixed32(ptr, header_copy.next_leaf);
    ptr += 4;
    
    // prev_leaf (4字节)
    utils::Coding::EncodeFixed32(ptr, header_copy.prev_leaf);
    ptr += 4;
    
    // first_child (4字节)
    utils::Coding::EncodeFixed32(ptr, header_copy.first_child);
    ptr += 4;
    
    // checksum (4字节)
    utils::Coding::EncodeFixed32(ptr, header_copy.checksum);
    ptr += 4;
    
    // reserved (32字节) - 填充0，但需要对齐到64字节
    const size_t reserved_size = sizeof(PageHeader) - (ptr - dest);
    std::memset(ptr, 0, reserved_size);
    ptr += reserved_size;
    
    // 验证：确保页头序列化大小正确
    const size_t header_size = ptr - dest;
    if (header_size != sizeof(PageHeader)) {
        // 这不应该发生，但如果发生，是严重的程序错误
        throw std::runtime_error("Page header serialization size mismatch");
    }
    
    // 2. 复制数据区（从 Arena 分配的内存复制到序列化缓冲区）
    std::memcpy(ptr, data_, kDataAreaSize);
}

void Page::DeserializeFrom(const char* src) {
    const char* ptr = src;
    
    // 1. 解码页头
    // type
    header_.type = static_cast<PageType>(static_cast<uint8_t>(*ptr));
    ptr += 1;
    
    // padding1[7] (7字节) - 对齐到8字节
    ptr += 7;
    
    // page_id
    header_.page_id = utils::Coding::DecodeFixed64(ptr);
    ptr += 8;
    
    // parent_id
    header_.parent_id = utils::Coding::DecodeFixed32(ptr);
    ptr += 4;
    
    // key_count
    header_.key_count = utils::Coding::DecodeFixed16(ptr);
    ptr += 2;
    
    // free_offset
    header_.free_offset = utils::Coding::DecodeFixed16(ptr);
    ptr += 2;
    
    // next_leaf
    header_.next_leaf = utils::Coding::DecodeFixed32(ptr);
    ptr += 4;
    
    // prev_leaf
    header_.prev_leaf = utils::Coding::DecodeFixed32(ptr);
    ptr += 4;
    
    // first_child
    header_.first_child = utils::Coding::DecodeFixed32(ptr);
    ptr += 4;
    
    // checksum
    header_.checksum = utils::Coding::DecodeFixed32(ptr);
    ptr += 4;
    
    // reserved (跳过剩余字节，对齐到64字节)
    const size_t header_so_far = ptr - src;
    const size_t reserved_size = sizeof(PageHeader) - header_so_far;
    ptr += reserved_size;
    
    // 2. 确保数据区已分配（如果未分配则初始化）
    if (!data_) {
        InitializeDataArea();
    }
    
    // 3. 复制数据区（从序列化缓冲区复制到 Arena 分配的内存）
    std::memcpy(data_, ptr, kDataAreaSize);
    
    // 4. 验证校验和（数据完整性检查）
    uint32_t calculated_checksum = CalculateChecksum();
    if (header_.checksum != calculated_checksum && header_.checksum != 0) {
        // 校验和失败，可能数据已损坏
        // 注意：checksum==0 可能是新页，跳过检查
        throw std::runtime_error("Page checksum verification failed - possible data corruption");
    }
    
    // 5. 验证 free_offset 不会越界
    if (header_.free_offset > kDataAreaSize) {
        header_.free_offset = static_cast<uint16_t>(kDataAreaSize);
        // 记录错误或抛出异常，取决于错误处理策略
    }
}

uint32_t Page::CalculateChecksum() const {
    // 简单的 FNV-1a 32位哈希，用于快速校验
    // 注意：这不是加密安全的，但足够检测意外数据损坏
    uint32_t hash = 0x811c9dc5; // FNV偏移基础值
    
    // 哈希页头（除checksum字段本身）
    const uint8_t* header_bytes = reinterpret_cast<const uint8_t*>(&header_);
    for (size_t i = 0; i < offsetof(PageHeader, checksum); ++i) {
        hash ^= static_cast<uint32_t>(header_bytes[i]);
        hash *= 0x01000193; // FNV质数
    }
    
    // 哈希数据区（使用 kDataAreaSize 而不是 sizeof(data_)）
    if (data_) {  // 确保数据区已分配
    const uint8_t* data_bytes = reinterpret_cast<const uint8_t*>(data_);
        for (size_t i = 0; i < kDataAreaSize; ++i) {
        hash ^= static_cast<uint32_t>(data_bytes[i]);
        hash *= 0x01000193;
        }
    }
    
    return hash;
}

} // namespace core

} // namespace mementodb