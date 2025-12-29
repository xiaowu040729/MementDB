
// File: src/utils/Hash.h
#pragma once

#include "mementodb/Types.h" // 使用项目统一的 Slice 类型
#include <cstdint>
#include <string>

namespace utils {
using Slice = mementodb::Slice;

/**
 * 哈希工具集
 * 提供多种非加密哈希函数，适用于哈希表等数据结构。
 *
 */
class Hash {
public:
    /**
     * 计算数据的32位哈希值（默认使用高效的MurmurHash3）
     * @param data 输入数据的指针
     * @param n 数据长度（字节）
     * @param seed 随机种子（可用于防御哈希碰撞攻击）
     * @return 32位哈希值
     */
    static uint32_t ComputeHash(const void* data, size_t n, uint32_t seed = 0x9747b28c);

    /**
     * 为Slice类型特化的便捷函数
     */
    static uint32_t HashSlice(const Slice& s, uint32_t seed = 0x9747b28c) {
        return ComputeHash(s.data(), s.size(), seed);
    }

    /**
     * 为整型特化的哈希函数（直接使用混合函数）
     *把输入的数字彻底打乱，让连续的数字（1,2,3…）产生看起来毫无规律的哈希值。
     */
    static uint32_t HashInt32(uint32_t key) {
        // 简单高效的整数混合
        key = ((key >> 16) ^ key) * 0x45d9f3b;
        key = ((key >> 16) ^ key) * 0x45d9f3b;
        key = (key >> 16) ^ key;
        return key;
    }

    /**
     * 计算64位整数的哈希值
     */
    static uint64_t HashInt64(uint64_t key) {
        key = (~key) + (key << 21); // key = (key << 21) - key - 1;
        key = key ^ (key >> 24);
        key = (key + (key << 3)) + (key << 8); // key * 265
        key = key ^ (key >> 14);
        key = (key + (key << 2)) + (key << 4); // key * 21
        key = key ^ (key >> 28);
        key = key + (key << 31);
        return key;
    }

    // 算法枚举（可按需扩展）
    enum Algorithm {
        MURMUR3_32,    // 平衡性能与分布
        FNV1A_32,      // 极简，适合短键
        XXHASH_32      // 极快，现代算法（可后期实现）
    };

    /**
     * 使用指定算法计算哈希
     */
    static uint32_t ComputeHashWith(Algorithm algo, const void* data, size_t n, uint32_t seed = 0);

    /**
     * 计算字符串的哈希值
     */
    static uint32_t HashString(const std::string& str, uint32_t seed = 0x9747b28c) {
        return ComputeHash(str.data(), str.size(), seed);
    }
    
    /**
     * 计算字符串视图的哈希值
     */
    static uint32_t HashStringView(std::string_view sv, uint32_t seed = 0x9747b28c) {
        return ComputeHash(sv.data(), sv.size(), seed);
    }

    /**
     * 将新值合并到已有哈希值中
     */
    static uint32_t CombineHash(uint32_t hash, uint32_t value) {
    return hash ^ (value + 0x9e3779b9 + (hash << 6) + (hash >> 2));
}


private:
    // 私有构造函数，防止实例化
    Hash() = delete;
    
    // ---------- 各算法具体实现 ----------
    
    /**
     * MurmurHash3 32位版本
     * 出色的性能与随机分布，是许多生产系统的默认选择
     */
    static uint32_t MurmurHash3(const void* key, size_t len, uint32_t seed);
    
    /**
     * FNV-1a 32位版本
     * 极其简单，适合短字符串和CPU缓存敏感场景
     */
    static uint32_t FNV1aHash(const void* key, size_t len);
    
    // （XXHash等算法可后续添加）
};

} // namespace utils
