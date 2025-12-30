// File: src/transaction/src/VectorCharHash.hpp
// 为 std::vector<char> 提供哈希函数
#pragma once

#include <vector>
#include <cstddef>
#include <cstdint>

namespace mementodb {
namespace transaction {

// 为 std::vector<char> 提供哈希函数
struct VectorCharHash {
    std::size_t operator()(const std::vector<char>& vec) const noexcept {
        std::size_t hash = 0;
        for (char c : vec) {
            hash ^= static_cast<std::size_t>(static_cast<unsigned char>(c)) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        }
        return hash;
    }
};

} // namespace transaction
} // namespace mementodb

