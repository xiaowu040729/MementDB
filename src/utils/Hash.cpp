// File: src/utils/Hash.cpp
#include "Hash.hpp"
#include <algorithm> // for std::rotate

namespace utils {

// 默认使用 MurmurHash3
uint32_t Hash::ComputeHash(const void* data, size_t n, uint32_t seed) {
    return MurmurHash3(data, n, seed);
}

uint32_t Hash::ComputeHashWith(Algorithm algo, const void* data, size_t n, uint32_t seed) {
    switch (algo) {
        case MURMUR3_32:
            return MurmurHash3(data, n, seed);
        case FNV1A_32:
            return FNV1aHash(data, n);
        default:
            // 默认回退
            return MurmurHash3(data, n, seed);
    }
}

// ---------- MurmurHash3 实现 ----------
uint32_t Hash::MurmurHash3(const void* key, size_t len, uint32_t seed) {
    const uint8_t* data = (const uint8_t*)key;
    const int nblocks = len / 4;
    
    uint32_t h1 = seed;
    
    const uint32_t c1 = 0xcc9e2d51;
    const uint32_t c2 = 0x1b873593;
    
    // 按4字节块处理
    const uint32_t* blocks = (const uint32_t*)(data + nblocks * 4);
    for (int i = -nblocks; i; i++) {
        uint32_t k1 = blocks[i];
        
        k1 *= c1;
        k1 = (k1 << 15) | (k1 >> 17);
        k1 *= c2;
        h1 ^= k1;
        h1 = (h1 << 13) | (h1 >> 19);
        h1 = h1 * 5 + 0xe6546b64;
    }
    
    // 处理尾部不足4字节的部分
    const uint8_t* tail = (const uint8_t*)(data + nblocks * 4);
    uint32_t k1 = 0;
    
    switch (len & 3) {
        case 3: k1 ^= tail[2] << 16;
        case 2: k1 ^= tail[1] << 8;
        case 1: k1 ^= tail[0];
            k1 *= c1;
            k1 = (k1 << 15) | (k1 >> 17);
            k1 *= c2;
            h1 ^= k1;
    }
    
    // 最终混合
    h1 ^= len;
    h1 ^= h1 >> 16;
    h1 *= 0x85ebca6b;
    h1 ^= h1 >> 13;
    h1 *= 0xc2b2ae35;
    h1 ^= h1 >> 16;
    
    return h1;
}

// ---------- FNV-1a 实现 ----------
uint32_t Hash::FNV1aHash(const void* key, size_t len) {
    const uint8_t* data = (const uint8_t*)key;
    uint32_t h = 0x811c9dc5; // FNV偏移基础值
    
    for (size_t i = 0; i < len; i++) {
        h ^= (uint32_t)data[i];
        h *= 0x01000193; // FNV质数
    }
    
    return h;
}

} // namespace utils

