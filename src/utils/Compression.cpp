// File: src/utils/Compression.cpp
// 压缩实现（使用 zstd 库，可选）

#include "Compression.hpp"
#include <cstring>
#include <stdexcept>

#ifdef HAVE_ZSTD
#include <zstd.h>
#endif

namespace mementodb {
namespace utils {

#ifdef HAVE_ZSTD
int Compression::LevelToZstd(Level level) {
    switch (level) {
        case Level::FAST: return 1;
        case Level::DEFAULT: return 3;
        case Level::HIGH: return 6;
        case Level::MAX: return 22;
        default: return 3;
    }
}
#endif

std::optional<std::vector<uint8_t>> Compression::Compress(
    const void* input,
    size_t input_size,
    Level level
) {
#ifdef HAVE_ZSTD
    if (input == nullptr || input_size == 0) {
        return std::nullopt;
    }

    size_t compressed_bound = ZSTD_compressBound(input_size);
    std::vector<uint8_t> output(compressed_bound);

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

    output.resize(compressed_size);
    return output;
#else
    (void)input;
    (void)input_size;
    (void)level;
    return std::nullopt;  // compression disabled when zstd not available
#endif
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
#ifdef HAVE_ZSTD
    if (compressed == nullptr || compressed_size == 0) {
        return std::nullopt;
    }

    unsigned long long decompressed_size = ZSTD_getFrameContentSize(compressed, compressed_size);

    if (decompressed_size == ZSTD_CONTENTSIZE_ERROR ||
        decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN) {
        return std::nullopt;
    }

    if (decompressed_size > max_output_size) {
        return std::nullopt;
    }

    std::vector<uint8_t> output(decompressed_size);

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
#else
    (void)compressed;
    (void)compressed_size;
    (void)max_output_size;
    return std::nullopt;
#endif
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
#ifdef HAVE_ZSTD
    return ZSTD_compressBound(input_size);
#else
    (void)input_size;
    return 0;
#endif
}

bool Compression::IsValidCompressedData(const void* data, size_t size) {
#ifdef HAVE_ZSTD
    if (data == nullptr || size == 0) {
        return false;
    }
    unsigned long long frame_size = ZSTD_getFrameContentSize(data, size);
    return frame_size != ZSTD_CONTENTSIZE_ERROR;
#else
    (void)data;
    (void)size;
    return false;
#endif
}

} // namespace utils
} // namespace mementodb
