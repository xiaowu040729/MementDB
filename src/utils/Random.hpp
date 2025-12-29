#pragma once

#include <cstdint>
#include <string>
#include "mementodb/Types.h"
#include "../memory/Arena.hpp"  // 需要完整类型定义（用于成员变量）

namespace utils {
using Slice = mementodb::Slice;

/**
 * 高性能随机数生成器
 * 
 * 设计目标：
 * 1. 生成高性能、统计质量良好的伪随机数
 * 2. 完全确定性（相同种子产生相同序列），保证测试可重复
 * 3. 提供数据库测试所需的专用方法（随机键、值、字符串）
 * 
 * 实现：PCG算法（Permuted Congruential Generator）
 * 优点：比传统线性同余生成器质量更好，速度接近，代码简洁
 */
class Random {
public:
    /**
     * 用指定种子初始化生成器
     * @param seed 随机种子，默认为1（0会导致生成器失效）
     */
    explicit Random(uint64_t seed = 1) : state_(0), inc_(0), arena_(nullptr) {
        Seed(seed);
    }

    // ==================== 核心随机数生成 ====================
    
    /**
     * 生成32位无符号随机整数 [0, 2^32-1]
     */
    uint32_t NextU32();
    /**
     * 生成64位无符号随机整数 [0, 2^64-1]
     */
    uint64_t NextU64();

    /**
     * 生成32位有符号随机整数 [min, max]（包含两端）
     */
    int32_t Uniform(int32_t min, int32_t max);
    
    /**
     * 生成均匀分布的浮点数 [0.0, 1.0)
     */
    double NextDouble();

    // 生成随机数，用于测试、模拟、加密等。
    // ==================== 数据库专用生成器 ====================
    /**
     * 生成随机字节序列（长度由调用者保证）
     * @param buf 输出缓冲区
     * @param size 字节数
     */
    void NextBytes(void* buf, size_t size);
    
    /**
     * 生成随机长度的Slice（使用内部Arena分配内存）
     * @param min_len 最小长度
     * @param max_len 最大长度
     * @return 包含随机内容的Slice（内存由内部管理）
     */
    Slice NextSlice(size_t min_len, size_t max_len);
    
    /**
     * 生成随机字符串（可打印字符）
     * @param length 字符串长度
     */
    std::string NextString(size_t length);
    
    /**
     * 生成数据库风格的随机键
     * 格式："key_[16位十六进制]" 或 "user:[id]:[field]"
     */
    std::string NextKey();
    
    /**
     * 生成随机值（可变长度的二进制数据）
     * @param avg_size 平均大小（实际大小在0.5x-1.5x之间波动）
     */
    std::string NextValue(size_t avg_size = 100);
    
    // ==================== 状态控制 ====================
    
    /**
     * 重置生成器状态（相同种子产生相同序列）
     */
    void Seed(uint64_t seed);
    
    /**
     * 跳过指定数量的随机数（用于并行测试时划分序列）
     */
    void Skip(uint64_t count);
    
private:
    // PCG状态变量
    uint64_t state_;
    uint64_t inc_;
    
    // 内部Arena，用于NextSlice的内存分配
    mementodb::Arena* arena_;
    
    // 内部辅助函数
    uint32_t NextU32Internal();
    void InitArena();
};

} // namespace utils
