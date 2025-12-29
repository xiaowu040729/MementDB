// File: src/memory/Arena.cpp
#include "Arena.hpp"
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <new>
#include <stdexcept>

namespace mementodb {

// ==================== Block 实现 ====================

Arena::Block::Block(size_t block_size) 
    : size(block_size), used(0) {
    // 使用系统分配器（避免与Arena递归）
    memory = static_cast<char*>(std::malloc(size));
    if (!memory) {
        throw std::bad_alloc();
    }
}

Arena::Block::~Block() {
    std::free(memory);
}

// ==================== Arena 公有方法 ====================

Arena::Arena(size_t block_size)
    : current_block_index_(0)
    , block_size_(std::max(block_size, sizeof(void*)))
    , total_allocated_(0)
    , current_block_(nullptr) {
    // 立即分配第一个块（延迟分配也可，这里选择立即）
    blocks_.reserve(16);  // 预分配块指针空间
    AllocateNewBlock(0);  // 分配标准块
}

Arena::~Arena() {
    for (Block* block : blocks_) {
        delete block;
    }
}

Arena::Arena(Arena&& other) noexcept
    : blocks_(std::move(other.blocks_))
    , current_block_index_(other.current_block_index_)
    , block_size_(other.block_size_)
    , total_allocated_(other.total_allocated_)
    , current_block_(other.current_block_) {
    // 清空原对象状态
    other.blocks_.clear();
    other.current_block_index_ = 0;
    other.total_allocated_ = 0;
    other.current_block_ = nullptr;
}

Arena& Arena::operator=(Arena&& other) noexcept {
    if (this != &other) {
        // 释放现有资源
        for (Block* block : blocks_) {
            delete block;
        }
        
        // 转移资源
        blocks_ = std::move(other.blocks_);
        current_block_index_ = other.current_block_index_;
        block_size_ = other.block_size_;
        total_allocated_ = other.total_allocated_;
        current_block_ = other.current_block_;
        
        // 清空原对象
        other.blocks_.clear();
        other.current_block_index_ = 0;
        other.total_allocated_ = 0;
        other.current_block_ = nullptr;
    }
    return *this;
}

char* Arena::Allocate(size_t bytes) {
    if (bytes == 0) {
        return nullptr;
    }
    
    // 检查当前块是否有足够空间
    if (current_block_->used + bytes <= current_block_->size) {
        char* result = current_block_->memory + current_block_->used;
        current_block_->used += bytes;
        total_allocated_ += bytes;
        return result;
    }
    
    // 需要新块：如果请求很大，单独分配；否则分配标准块
    size_t new_block_size = std::max(block_size_, bytes);
    AllocateNewBlock(new_block_size);
    
    // 从新块分配
    char* result = current_block_->memory;
    current_block_->used = bytes;
    total_allocated_ += bytes;
    return result;
}

char* Arena::AllocateAligned(size_t bytes, size_t alignment) {
    // 对齐必须是2的幂
    if ((alignment & (alignment - 1)) != 0) {
        alignment = sizeof(void*);  // 回退到指针大小
    }
    
    // 计算当前指针需要多少填充才能对齐
    const char* current_ptr = current_block_->memory + current_block_->used;
    size_t padding = AlignmentPadding(current_ptr, alignment);
    
    // 检查当前块是否有足够空间（包括填充）
    if (current_block_->used + padding + bytes <= current_block_->size) {
        current_block_->used += padding;  // 跳过填充
        char* result = current_block_->memory + current_block_->used;
        current_block_->used += bytes;
        total_allocated_ += bytes + padding;
        return result;
    }
    
    // 需要新块：对齐分配要求新块本身对齐
    size_t new_block_size = std::max(block_size_, bytes + alignment);
    AllocateNewBlock(new_block_size);
    
    // 新块起始地址保证对齐（通过AllocateNewBlock中的对齐保证）
    current_block_->used = 0;  // 新块未使用
    
    // 重新计算（新块起始已对齐）
    const char* new_current_ptr = current_block_->memory;
    size_t new_padding = AlignmentPadding(new_current_ptr, alignment);
    current_block_->used = new_padding;
    
    char* result = current_block_->memory + current_block_->used;
    current_block_->used += bytes;
    total_allocated_ += bytes + new_padding;
    return result;
}

size_t Arena::MemoryUsage() const {
    size_t total = 0;
    for (const Block* block : blocks_) {
        total += block->size + sizeof(Block);  // 块内存 + 管理开销
    }
    total += blocks_.capacity() * sizeof(Block*);  // vector容量开销
    return total;
}

size_t Arena::RemainingBytes() const {
    if (!current_block_) return 0;
    return current_block_->size - current_block_->used;
}

size_t Arena::BlockCount() const {
    return blocks_.size();
}

void Arena::Reset() {
    // 保留第一个块（如果存在），释放其余块
    while (blocks_.size() > 1) {
        delete blocks_.back();
        blocks_.pop_back();
    }
    
    if (!blocks_.empty()) {
        current_block_ = blocks_[0];
        current_block_->used = 0;
        current_block_index_ = 0;
    } else {
        current_block_ = nullptr;
        current_block_index_ = 0;
    }
    
    total_allocated_ = 0;
}

void Arena::Reserve(size_t bytes) {
    if (bytes <= RemainingBytes()) {
        return;  // 当前块已足够
    }
    
    size_t needed = bytes - RemainingBytes();
    size_t new_block_size = std::max(block_size_, needed);
    AllocateNewBlock(new_block_size);
}

// ==================== Arena 私有方法 ====================

void Arena::AllocateNewBlock(size_t min_bytes) {
    // 实际分配大小（向上取整到块大小的倍数）
    size_t actual_size = std::max(block_size_, min_bytes);
    actual_size = (actual_size + block_size_ - 1) & ~(block_size_ - 1);
    
    // 创建新块
    Block* new_block = new Block(actual_size);
    
    // 添加到块列表
    blocks_.push_back(new_block);
    current_block_index_ = blocks_.size() - 1;
    current_block_ = new_block;
}

size_t Arena::AlignmentPadding(const char* ptr, size_t alignment) {
    // 计算ptr需要前进多少字节才能对齐到alignment
    size_t mod = reinterpret_cast<uintptr_t>(ptr) & (alignment - 1);
    return (mod == 0) ? 0 : (alignment - mod);
}

} // namespace mementodb
