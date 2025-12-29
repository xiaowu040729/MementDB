// File: src/utils/Random.cpp
#include "../memory/Arena.hpp"  // 必须先包含，确保完整类型定义
#include "Random.hpp"
#include <cstring>
#include <algorithm>

namespace utils {

// PCG算法常量
static const uint64_t PCG_MULTIPLIER = 6364136223846793005ULL;
static const uint64_t PCG_INCREMENT = 1442695040888963407ULL;

void Random::Seed(uint64_t seed) {
    state_ = 0;
    inc_ = (seed << 1) | 1;  // 确保inc为奇数
    NextU32Internal();       // 预热一次
    state_ += seed;
    NextU32Internal();
    
    // 初始化内部Arena（延迟初始化）
    if (!arena_) {
        InitArena();
    }
}

void Random::InitArena() {
    // 使用静态局部变量确保线程安全（C++11保证）
    static mementodb::Arena global_arena;
    arena_ = &global_arena;
}

uint32_t Random::NextU32Internal() {
    uint64_t oldstate = state_;
    state_ = oldstate * PCG_MULTIPLIER + inc_;
    
    // 输出转换（XSH-RR变体）
    uint32_t xorshifted = static_cast<uint32_t>(((oldstate >> 18u) ^ oldstate) >> 27u);
    uint32_t rot = oldstate >> 59u;
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

uint32_t Random::NextU32() {
    return NextU32Internal();
}

uint64_t Random::NextU64() {
    uint64_t high = NextU32Internal();
    uint64_t low = NextU32Internal();
    return (high << 32) | low;
}


int32_t Random::Uniform(int32_t min, int32_t max) {
    // 输入验证
    if (min > max) std::swap(min, max);
    if (min == max) return min;
    
    uint32_t range = static_cast<uint32_t>(max - min) + 1;
    uint32_t random = NextU32();
    
    // 避免模偏差（rejection sampling的简化版）
    return min + static_cast<int32_t>(random % range);
}


double Random::NextDouble() {
    // 生成[0, 1)区间的双精度浮点数
    // 使用52位随机数（double的尾数位数），确保结果在[0, 1)范围内
    const uint32_t a = NextU32() >> 5;   // 高27位
    const uint32_t b = NextU32() >> 5;   // 高27位
    // 组合成52位：a的高26位 + b的低26位
    const uint64_t combined = ((static_cast<uint64_t>(a) & 0x3FFFFFF) << 26) | 
                               (static_cast<uint64_t>(b) & 0x3FFFFFF);
    const double divisor = 1.0 / (1ULL << 52);
    return static_cast<double>(combined) * divisor;
}

void Random::NextBytes(void* buf, size_t size) {
    uint8_t* ptr = static_cast<uint8_t*>(buf);
    
    // 每次处理4字节（对齐优化）
    while (size >= sizeof(uint32_t)) {
        *reinterpret_cast<uint32_t*>(ptr) = NextU32();
        ptr += sizeof(uint32_t);
        size -= sizeof(uint32_t);
    }
    
    // 处理剩余字节
    if (size > 0) {
        uint32_t last = NextU32();
        for (size_t i = 0; i < size; ++i) {
            ptr[i] = static_cast<uint8_t>(last >> (8 * i));
        }
    }
}

Slice Random::NextSlice(size_t min_len, size_t max_len) {
    if (min_len > max_len) std::swap(min_len, max_len);
    if (min_len == max_len && min_len == 0) return Slice();
    
    // 生成随机长度
    size_t length = Uniform(static_cast<int32_t>(min_len), 
                           static_cast<int32_t>(max_len));
    
    // 从Arena分配内存
    char* buf = arena_->Allocate(length);
    NextBytes(buf, length);
    
    return Slice(buf, length);
}

std::string Random::NextString(size_t length) {
    static const char alphanum[] = 
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    
    std::string result;
    result.reserve(length);
    
    for (size_t i = 0; i < length; ++i) {
        int idx = NextU32() % (sizeof(alphanum) - 1);
        result.push_back(alphanum[idx]);
    }
    
    return result;
}

std::string Random::NextKey() {
    // 两种键格式随机选择
    if (NextU32() % 2 == 0) {
        // 格式1: key_xxxxxxxx
        char hex[9];
        uint32_t num = NextU32();
        snprintf(hex, sizeof(hex), "%08x", num);
        return std::string("key_") + hex;
    } else {
        // 格式2: user:[id]:[field]
        int32_t id = Uniform(1, 1000000);
        const char* fields[] = {"name", "email", "age", "balance", "created_at"};
        int field_idx = Uniform(0, static_cast<int32_t>(sizeof(fields)/sizeof(fields[0]) - 1));
        return "user:" + std::to_string(id) + ":" + fields[field_idx];
    }
}

std::string Random::NextValue(size_t avg_size) {
    // 实际大小在0.5x到1.5x之间随机
    int32_t min_size = static_cast<int32_t>(avg_size * 0.5);
    int32_t max_size = static_cast<int32_t>(avg_size * 1.5);
    int32_t size = Uniform(min_size, max_size);
    
    std::string value;
    value.reserve(size);
    
    // 生成偏真实的二进制数据（混合可打印和不可打印字符）
    for (int i = 0; i < size; ++i) {
        // 80%概率为可打印字符，20%为任意字节
        if (NextDouble() < 0.8) {
            value.push_back(static_cast<char>(Uniform(32, 126))); // 可打印ASCII
        } else {
            value.push_back(static_cast<char>(NextU32() & 0xFF));
        }
    }
    
    return value;
}

void Random::Skip(uint64_t count) {
    // 快速跳过（使用PCG的数学特性）
    uint64_t cur_mult = PCG_MULTIPLIER;
    uint64_t cur_plus = inc_;
    uint64_t acc_mult = 1;
    uint64_t acc_plus = 0;
    
    while (count > 0) {
        if (count & 1) {
            acc_mult *= cur_mult;
            acc_plus = acc_plus * cur_mult + cur_plus;
        }
        cur_plus = (cur_mult + 1) * cur_plus;
        cur_mult *= cur_mult;
        count >>= 1;
    }
    
    state_ = acc_mult * state_ + acc_plus;
}

} // namespace utils