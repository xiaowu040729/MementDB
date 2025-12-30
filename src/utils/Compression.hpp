// File: src/utils/Compression.hpp
// 压缩工具类（使用 zstd 第三方库）
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <optional>

namespace mementodb {
namespace utils {

/**
 * Compression - 压缩工具类
 * 
 * 使用 zstd 库进行数据压缩/解压
 * 适用于 WAL 日志压缩、数据存储压缩等场景
 */
class Compression {
public:
    /**
     * 压缩级别枚举
     */
    enum class Level {
        FAST = 1,           // 快速压缩（低压缩比）
        DEFAULT = 3,         // 默认压缩（平衡）
        HIGH = 6,            // 高压缩比
        MAX = 22            // 最大压缩比（最慢）
    };
    
    /**
     * 压缩数据
     * @param input 输入数据
     * @param level 压缩级别
     * @return 压缩后的数据（如果失败返回空）
     */
    static std::optional<std::vector<uint8_t>> Compress(
        const void* input,
        size_t input_size,
        Level level = Level::DEFAULT
    );
    
    /**
     * 压缩数据（从 vector）
     */
    static std::optional<std::vector<uint8_t>> Compress(
        const std::vector<uint8_t>& input,
        Level level = Level::DEFAULT
    );
    
    /**
     * 压缩字符串
     */
    static std::optional<std::vector<uint8_t>> CompressString(
        const std::string& input,
        Level level = Level::DEFAULT
    );
    
    /**
     * 解压数据
     * @param compressed 压缩后的数据
     * @param max_output_size 最大输出大小（防止解压后数据过大）
     * @return 解压后的数据（如果失败返回空）
     */
    static std::optional<std::vector<uint8_t>> Decompress(
        const void* compressed,
        size_t compressed_size,
        size_t max_output_size = 1024 * 1024 * 1024  // 默认最大 1GB
    );
    
    /**
     * 解压数据（从 vector）
     */
    static std::optional<std::vector<uint8_t>> Decompress(
        const std::vector<uint8_t>& compressed,
        size_t max_output_size = 1024 * 1024 * 1024
    );
    
    /**
     * 解压为字符串
     */
    static std::optional<std::string> DecompressToString(
        const void* compressed,
        size_t compressed_size,
        size_t max_output_size = 1024 * 1024 * 1024
    );
    
    /**
     * 获取压缩后的预估大小（用于预分配缓冲区）
     * @param input_size 输入数据大小
     * @return 预估的压缩后大小
     */
    static size_t GetCompressedBound(size_t input_size);
    
    /**
     * 检查数据是否为有效的压缩数据
     * @param data 数据指针
     * @param size 数据大小
     * @return 是否为有效的压缩数据
     */
    static bool IsValidCompressedData(const void* data, size_t size);

private:
    Compression() = delete;  // 工具类，禁止实例化
    
    /**
     * 将压缩级别转换为 zstd 级别
     */
    static int LevelToZstd(Level level);
};

} // namespace utils
} // namespace mementodb

