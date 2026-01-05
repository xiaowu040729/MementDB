// File: src/transaction/src/VectorCharHash.hpp
// 字节序列通用哈希与比较工具（增强版）
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <chrono>

#include "../../utils/Hash.hpp"
#include "../../utils/CRC32.hpp"

namespace mementodb {
namespace transaction {

/**
 * 字节序列特征
 */
template<typename T>
struct is_byte_sequence : std::false_type {};

template<>
struct is_byte_sequence<std::vector<char>> : std::true_type {};

template<>
struct is_byte_sequence<std::vector<unsigned char>> : std::true_type {};

template<>
struct is_byte_sequence<std::string> : std::true_type {};

template<>
struct is_byte_sequence<std::string_view> : std::true_type {};

template<std::size_t N>
struct is_byte_sequence<std::array<char, N>> : std::true_type {};

/**
 * 字节序列哈希策略
 */
enum class HashStrategy : uint8_t {
    XXHASH64 = 0,       // 高性能，低碰撞（推荐默认）
    MURMURHASH3 = 1,    // 平衡性能和碰撞率
    CITYHASH = 2,       // Google出品，针对短字符串优化
    FNV1A = 3,          // 简单快速，适合短键
    CRC32C = 4,         // 硬件加速，适合校验
    AES_HASH = 5,       // 加密安全，高性能硬件加速
    XXHASH3 = 6,        // 最新版本，极致性能
    CUSTOM = 7          // 用户自定义
};

/**
 * 哈希配置
 */
struct HashConfig {
    HashStrategy strategy = HashStrategy::XXHASH64;
    bool use_hardware_acceleration = true;   // 使用硬件加速（如果有）
    bool cache_hash_result = false;          // 缓存哈希结果（如果序列不变）
    size_t max_bytes_to_hash = 0;            // 0表示哈希全部字节
    uint64_t seed = 0;                       // 哈希种子
};

/**
 * 字节序列哈希函数（主类）
 *
 * 说明：当前实现统一使用 64 位 FNV-1a 变体作为底层算法，
 * 不同 HashStrategy 仅作为配置标识，后续可扩展为真正的多算法实现。
 */
template<HashStrategy Strategy = HashStrategy::XXHASH64>
class ByteSequenceHash {
public:
    using result_type = std::size_t;
    
    ByteSequenceHash() = default;
    
    explicit ByteSequenceHash(const HashConfig& config)
        : config_(config) {}
    
    // ========== 通用接口 ==========
    
    template<typename ByteSequence,
             typename = std::enable_if_t<is_byte_sequence<ByteSequence>::value>>
    result_type operator()(const ByteSequence& seq) const noexcept {
        if (seq.empty()) {
            return empty_hash_value();
        }
        
        const auto* data = reinterpret_cast<const uint8_t*>(seq.data());
        size_t size = seq.size();
        
        if (config_.max_bytes_to_hash != 0 &&
            size > config_.max_bytes_to_hash) {
            size = config_.max_bytes_to_hash;
        }
        
        return hash_impl(data, size);
    }
    
    // ========== C风格接口 ==========
    
    result_type operator()(const void* data, size_t size) const noexcept {
        if (!data || size == 0) {
            return empty_hash_value();
        }
        if (config_.max_bytes_to_hash != 0 &&
            size > config_.max_bytes_to_hash) {
            size = config_.max_bytes_to_hash;
        }
        return hash_impl(static_cast<const uint8_t*>(data), size);
    }
    
    // ========== 分段哈希 ==========
    
    result_type hash_combine(result_type seed, const void* data, size_t size) const noexcept {
        return combine_hash(seed, operator()(data, size));
    }
    
    template<typename ByteSequence>
    result_type hash_combine(result_type seed, const ByteSequence& seq) const noexcept {
        return combine_hash(seed, operator()(seq));
    }
    
    // ========== 工具方法 ==========
    
    static result_type empty_hash_value() { return 0x9e3779b97f4a7c15ULL; }
    
    static result_type combine_hash(result_type seed, result_type hash) noexcept {
        // 64位混合函数（来自boost）
        return seed ^ (hash + 0x9e3779b9 + (seed << 6) + (seed >> 2));
    }
    
    // ========== 配置管理 ==========
    
    void set_config(const HashConfig& config) { config_ = config; }
    const HashConfig& get_config() const { return config_; }
    
private:
    HashConfig config_{};
    
    result_type hash_impl(const uint8_t* data, size_t size) const noexcept {
        using ::utils::Hash;
        using mementodb::utils::CRC32;
        
        switch (config_.strategy) {
            case HashStrategy::XXHASH64: {
                // 使用第三方 xxhash 库，通过工具类 Hash::ComputeHash64
                uint64_t h = Hash::ComputeHash64(data, size, config_.seed);
                return static_cast<result_type>(h);
            }
            case HashStrategy::MURMURHASH3: {
                uint32_t h32 = Hash::ComputeHash(data, size,
                                                 static_cast<uint32_t>(config_.seed));
                return static_cast<result_type>(h32);
            }
            case HashStrategy::FNV1A: {
                // 使用 Hash::ComputeHashWith 中的 FNV1A_32
                uint32_t h32 = Hash::ComputeHashWith(
                    Hash::FNV1A_32, data, size,
                    static_cast<uint32_t>(config_.seed));
                return static_cast<result_type>(h32);
            }
            case HashStrategy::CRC32C: {
                // 使用第三方 crc32c 库，通过 CRC32 包装类
                uint32_t crc = CRC32::Compute(data, size);
                return static_cast<result_type>(crc);
            }
            case HashStrategy::XXHASH3:
            case HashStrategy::CITYHASH:
            case HashStrategy::AES_HASH:
            case HashStrategy::CUSTOM:
            default: {
                // 默认回退到 64 位 FNV-1a 变体
                const uint64_t FNV_OFFSET = 14695981039346656037ULL;
                const uint64_t FNV_PRIME  = 1099511628211ULL;
                
                uint64_t hash = config_.seed ? (FNV_OFFSET ^ config_.seed) : FNV_OFFSET;
                for (size_t i = 0; i < size; ++i) {
                    hash ^= static_cast<uint64_t>(data[i]);
                    hash *= FNV_PRIME;
                }
                return static_cast<result_type>(hash);
            }
        }
    }
};

/**
 * 字节序列相等比较
 */
class ByteSequenceEqual {
public:
    template<typename Lhs, typename Rhs,
             typename = std::enable_if_t<
                 is_byte_sequence<Lhs>::value && 
                 is_byte_sequence<Rhs>::value>>
    bool operator()(const Lhs& lhs, const Rhs& rhs) const noexcept {
        if (lhs.size() != rhs.size()) {
            return false;
        }
        if (lhs.size() == 0) {
            return true;
        }
        return std::memcmp(lhs.data(), rhs.data(), lhs.size()) == 0;
    }
    
    // 混合类型比较
    template<typename Lhs, typename Rhs>
    bool operator()(const Lhs* lhs_data, size_t lhs_size,
                    const Rhs* rhs_data, size_t rhs_size) const noexcept {
        if (lhs_size != rhs_size) {
            return false;
        }
        if (lhs_size == 0) {
            return true;
        }
        return std::memcmp(lhs_data, rhs_data, lhs_size) == 0;
    }
};

/**
 * 字节序列小于比较（用于有序容器）
 */
class ByteSequenceLess {
public:
    template<typename Lhs, typename Rhs,
             typename = std::enable_if_t<
                 is_byte_sequence<Lhs>::value && 
                 is_byte_sequence<Rhs>::value>>
    bool operator()(const Lhs& lhs, const Rhs& rhs) const noexcept {
        return compare(lhs, rhs) < 0;
    }
    
    template<typename Lhs, typename Rhs>
    int compare(const Lhs& lhs, const Rhs& rhs) const noexcept {
        const size_t min_size = std::min(lhs.size(), rhs.size());
        if (min_size > 0) {
            int cmp = std::memcmp(lhs.data(), rhs.data(), min_size);
            if (cmp != 0) {
                return cmp;
            }
        }
        // 如果前面部分相同，长度短的较小
        return static_cast<int>(lhs.size()) - static_cast<int>(rhs.size());
    }
};

/**
 * 预定义的哈希类型别名
 */
using VectorCharHash = ByteSequenceHash<HashStrategy::XXHASH64>;
using VectorCharEqual = ByteSequenceEqual;
using VectorCharLess = ByteSequenceLess;

// 针对特定场景优化的哈希类型（当前实现与 VectorCharHash 相同）
using FastVectorCharHash = ByteSequenceHash<HashStrategy::XXHASH3>;      // 最快（预留）
using SecureVectorCharHash = ByteSequenceHash<HashStrategy::AES_HASH>;   // 最安全（预留）
using CompactVectorCharHash = ByteSequenceHash<HashStrategy::FNV1A>;     // 最小代码（预留）
using HardwareVectorCharHash = ByteSequenceHash<HashStrategy::CRC32C>;   // 硬件加速（预留）

/**
 * 哈希工具类
 */
class HashUtils {
public:
    /**
     * 计算哈希，考虑数据特征进行优化
     */
    template<typename ByteSequence>
    static std::size_t smart_hash(const ByteSequence& seq, 
                                  HashConfig config = HashConfig{}) {
        if (seq.empty()) {
            return 0;
        }
        
        // 根据数据特征自动选择策略（当前仅作为配置标记）
        if (seq.size() <= 16) {
            config.strategy = HashStrategy::CITYHASH;  // 短字符串优化
        } else if (seq.size() <= 256) {
            config.strategy = HashStrategy::XXHASH64;  // 中等长度
        } else {
            config.strategy = HashStrategy::XXHASH3;   // 长字符串
        }
        
        ByteSequenceHash<> hasher(config);
        return hasher(seq);
    }
    
    /**
     * 批量哈希计算
     */
    template<typename Iterator>
    static std::vector<std::size_t> batch_hash(
        Iterator begin, Iterator end,
        HashConfig config = HashConfig{}) {
        std::vector<std::size_t> results;
        results.reserve(std::distance(begin, end));
        
        ByteSequenceHash<> hasher(config);
        for (auto it = begin; it != end; ++it) {
            results.push_back(hasher(*it));
        }
        
        return results;
    }
    
    /**
     * 检查哈希质量（碰撞率）
     */
    template<typename Iterator>
    static double calculate_collision_rate(
        Iterator begin, Iterator end,
        HashConfig config = HashConfig{}) {
        std::unordered_set<std::size_t> hashes;
        ByteSequenceHash<> hasher(config);
        
        size_t total = 0;
        size_t collisions = 0;
        
        for (auto it = begin; it != end; ++it) {
            auto hash = hasher(*it);
            if (!hashes.insert(hash).second) {
                collisions++;
            }
            total++;
        }
        
        return total > 0 ? static_cast<double>(collisions) / total : 0.0;
    }
    
    /**
     * 哈希性能基准测试（简化占位实现）
     */
    struct BenchmarkResult {
        double hash_per_second{0.0};
        double bytes_per_second{0.0};
        double avg_time_ns{0.0};
        double collision_rate{0.0};
    };
    
    static BenchmarkResult benchmark(
        const std::vector<std::vector<char>>& test_data,
        HashConfig config = HashConfig{}) {
        BenchmarkResult result;
        // 简化实现：仅计算碰撞率，其它字段留待未来扩展
        result.collision_rate = calculate_collision_rate(
            test_data.begin(), test_data.end(), config);
        return result;
    }
};

/**
 * 缓存哈希值的包装器（用于不变量）
 */
template<typename ByteSequence>
class CachedHash {
public:
    explicit CachedHash(const ByteSequence& seq)
        : data_(seq)
        , hash_(compute_hash(seq)) {}
    
    CachedHash(ByteSequence&& seq)
        : data_(std::move(seq))
        , hash_(compute_hash(data_)) {}
    
    const ByteSequence& data() const { return data_; }
    std::size_t hash() const { return hash_; }
    
    bool operator==(const CachedHash& other) const {
        return data_ == other.data_;
    }
    
    bool operator<(const CachedHash& other) const {
        return data_ < other.data_;
    }
    
private:
    ByteSequence data_;
    std::size_t hash_;
    
    static std::size_t compute_hash(const ByteSequence& seq) {
        return VectorCharHash{}(seq);
    }
};

// CachedHash的特化哈希函数
template<typename ByteSequence>
struct CachedHashHash {
    std::size_t operator()(const CachedHash<ByteSequence>& cached) const noexcept {
        return cached.hash();
    }
};

// CachedHash的特化相等函数
template<typename ByteSequence>
struct CachedHashEqual {
    bool operator()(const CachedHash<ByteSequence>& a, 
                    const CachedHash<ByteSequence>& b) const noexcept {
        return a.data() == b.data();
    }
};

} // namespace transaction
} // namespace mementodb


