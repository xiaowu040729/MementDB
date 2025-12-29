// File: src/utils/Arena.h
#pragma once

#include <cstddef>
#include <vector>

namespace mementodb {

/**
 * 高性能内存分配器（区域分配器）
 * 
 * 设计原理：
 * 1. 一次性申请大块内存（如4KB/8KB），从该块中切分小对象
 * 2. 所有内存只在析构时一次性释放，中间无单个对象释放操作
 * 3. 支持对齐分配，避免缓存行伪共享
 * 
 * 适用场景：
 * - B+树节点的频繁创建/销毁
 * - 临时键值对的短生命周期存储
 * - 批量操作中的中间数据
 * 
 */
class Arena {
public:
    // 默认块大小（4KB，匹配大多数系统内存页和磁盘扇区）
    static constexpr size_t kDefaultBlockSize = 4096;
    
    /**
     * 构造函数
     * @param block_size 每个内存块的大小，默认4KB
     */
    explicit Arena(size_t block_size = kDefaultBlockSize);
    
    ~Arena();
    
    // 禁止拷贝（分配器通常独占内存）
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    
    // 允许移动（资源可转移）
    Arena(Arena&& other) noexcept;
    Arena& operator=(Arena&& other) noexcept;
    
    // ==================== 核心分配接口 ====================
    
    /**
     * 分配指定大小的原始内存（不对齐）
     * @param bytes 请求的字节数
     * @return 分配的内存指针，保证至少有效到Arena销毁
     */
    char* Allocate(size_t bytes);
    
    /**
     * 分配对齐的内存（对齐到指定边界）
     * @param bytes 请求的字节数
     * @param alignment 对齐要求（必须是2的幂）
     * @return 对齐的内存指针
     * 
     * 使用场景：
     * - SSE/AVX指令需要16/32字节对齐
     * - 原子操作需要避免缓存行伪共享（64字节对齐）
     */
    char* AllocateAligned(size_t bytes, size_t alignment = sizeof(void*));
    
    /**
     * 分配并构造一个对象（类似placement new）
     * @tparam T 对象类型
     * @tparam Args 构造函数参数类型
     * @param args 构造函数参数
     * @return 指向新对象的指针
     */
    template<typename T, typename... Args>
    T* New(Args&&... args);
    
    /**
     * 分配对象数组（不支持非平凡析构类型）
     * @tparam T 数组元素类型
     * @param count 元素数量
     * @return 指向数组首元素的指针
     */
    template<typename T>
    T* AllocateArray(size_t count);
    
    // ==================== 内存使用信息 ====================
    
    /**
     * 返回已分配的总内存（包括管理开销）
     * @return 字节数
     */
    size_t MemoryUsage() const;
    
    /**
     * 返回当前内存块剩余字节数
     */
    size_t RemainingBytes() const;
    
    /**
     * 返回分配的内存块数量
     */
    size_t BlockCount() const;
    
    /**
     * 重置分配器（释放所有内存，可重用）
     * 注意：会使之前分配的所有指针失效
     */
    void Reset();
    
    /**
     * 预分配足够内存（减少后续分配时的块分裂）
     * @param bytes 预计需要的总字节数
     */
    void Reserve(size_t bytes);

private:
    // 内存块结构
    struct Block {
        char* memory;      // 内存起始地址
        size_t size;       // 块总大小
        size_t used;       // 已使用字节数
        
        Block(size_t block_size);
        ~Block();
        
        // 禁止拷贝和移动
        Block(const Block&) = delete;
        Block& operator=(const Block&) = delete;
    };
    
    // ==================== 私有方法 ====================
    
    /**
     * 申请新内存块（内部使用）
     * @param min_bytes 新块至少需要的字节数
     */
    void AllocateNewBlock(size_t min_bytes);
    
    /**
     * 对齐调整计算
     * @param ptr 需要对齐的指针
     * @param alignment 对齐要求
     * @return 调整到对齐所需的字节偏移量
     */
    static size_t AlignmentPadding(const char* ptr, size_t alignment);
    
    // ==================== 成员变量 ====================
    
    std::vector<Block*> blocks_;       // 所有内存块（所有权）
    size_t current_block_index_;       // 当前使用的块索引
    size_t block_size_;                // 标准块大小
    size_t total_allocated_;           // 总分配字节数（不含开销）
    
    // 当前块指针（缓存，避免频繁访问vector）
    Block* current_block_;
};

// ==================== 模板方法实现（必须在头文件中） ====================

template<typename T, typename... Args>
T* Arena::New(Args&&... args) {
    // 分配内存
    char* mem = Allocate(sizeof(T));
    
    // 构造对象（placement new）
    return new (mem) T(std::forward<Args>(args)...);
}

template<typename T>
T* Arena::AllocateArray(size_t count) {
    static_assert(std::is_trivially_destructible_v<T>,
                  "Arena only supports arrays of trivially destructible types");
    
    // 分配连续内存
    char* mem = Allocate(sizeof(T) * count);
    
    // 默认初始化（POD类型）
    T* array = reinterpret_cast<T*>(mem);
    for (size_t i = 0; i < count; ++i) {
        new (&array[i]) T();  // 值初始化
    }
    
    return array;
}

} // namespace mementodb