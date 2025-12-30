// File: src/utils/Compression.cpp
// 压缩实现（使用 zstd 库）

#include "Compression.hpp"
#include <zstd.h>
#include <cstring>
#include <stdexcept>

namespace mementodb {
namespace utils {

int Compression::LevelToZstd(Level level) {
    switch (level) {
        case Level::FAST: return 1;
        case Level::DEFAULT: return 3;
        case Level::HIGH: return 6;
        case Level::MAX: return 22;
        default: return 3;
    }
}

std::optional<std::vector<uint8_t>> Compression::Compress(
    const void* input,
    size_t input_size,
    Level level
) {
    if (input == nullptr || input_size == 0) {
        return std::nullopt;
    }
    
    // 获取压缩后的预估大小
    size_t compressed_bound = ZSTD_compressBound(input_size);
    std::vector<uint8_t> output(compressed_bound);
    
    // 执行压缩
    size_t compressed_size = ZSTD_compress(
        output.data(),
        compressed_bound,
        input,
        input_size,
        LevelToZstd(level)
    );
    
    if (ZSTD_isError(compressed_size)) {
        return std::nullopt;
    }
    
    // 调整输出大小
    output.resize(compressed_size);
    return output;
}

std::optional<std::vector<uint8_t>> Compression::Compress(
    const std::vector<uint8_t>& input,
    Level level
) {
    return Compress(input.data(), input.size(), level);
}

std::optional<std::vector<uint8_t>> Compression::CompressString(
    const std::string& input,
    Level level
) {
    return Compress(input.data(), input.size(), level);
}

std::optional<std::vector<uint8_t>> Compression::Decompress(
    const void* compressed,
    size_t compressed_size,
    size_t max_output_size
) {
    if (compressed == nullptr || compressed_size == 0) {
        return std::nullopt;
    }
    
    // 获取解压后的预估大小
    unsigned long long decompressed_size = ZSTD_getFrameContentSize(compressed, compressed_size);
    
    if (decompressed_size == ZSTD_CONTENTSIZE_ERROR || 
        decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN) {
        return std::nullopt;
    }
    
    if (decompressed_size > max_output_size) {
        return std::nullopt;  // 防止解压后数据过大
    }
    
    std::vector<uint8_t> output(decompressed_size);
    
    // 执行解压
    size_t actual_size = ZSTD_decompress(
        output.data(),
        decompressed_size,
        compressed,
        compressed_size
    );
    
    if (ZSTD_isError(actual_size) || actual_size != decompressed_size) {
        return std::nullopt;
    }
    
    return output;
}

std::optional<std::vector<uint8_t>> Compression::Decompress(
    const std::vector<uint8_t>& compressed,
    size_t max_output_size
) {
    return Decompress(compressed.data(), compressed.size(), max_output_size);
}

std::optional<std::string> Compression::DecompressToString(
    const void* compressed,
    size_t compressed_size,
    size_t max_output_size
) {
    auto result = Decompress(compressed, compressed_size, max_output_size);
    if (!result.has_value()) {
        return std::nullopt;
    }
    
    return std::string(result->begin(), result->end());
}

size_t Compression::GetCompressedBound(size_t input_size) {
    return ZSTD_compressBound(input_size);
}

bool Compression::IsValidCompressedData(const void* data, size_t size) {
    if (data == nullptr || size == 0) {
        return false;
    }
    
    // 检查是否为有效的 zstd 压缩数据
    unsigned long long frame_size = ZSTD_getFrameContentSize(data, size);
    return frame_size != ZSTD_CONTENTSIZE_ERROR;
}

} // namespace utils
} // namespace mementodb

