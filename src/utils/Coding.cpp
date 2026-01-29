// File: src/utils/Coding.cpp
#include "Coding.hpp"
#include <cstring>
#include <algorithm>
#include <arpa/inet.h> // 用于htonl/ntohl，保证跨平台一致性

namespace utils {

// ==================== 固定长度编码实现 ====================

void Coding::EncodeFixed16(char* buf, uint16_t value) {
    // 小端序存储：低字节在前
    buf[0] = static_cast<char>(value & 0xff);
    buf[1] = static_cast<char>((value >> 8) & 0xff);
}

uint16_t Coding::DecodeFixed16(const char* buf) {
    // 注意：static_cast<uint16_t>(char) 会符号扩展，必须先转成unsigned char
    return (static_cast<uint16_t>(static_cast<unsigned char>(buf[0]))) |
           (static_cast<uint16_t>(static_cast<unsigned char>(buf[1])) << 8);
}

void Coding::EncodeFixed32(char* buf, uint32_t value) {
    // 小端序存储：低字节在前，兼容x86架构，也是网络序的反序
    buf[0] = static_cast<char>(value & 0xff);
    buf[1] = static_cast<char>((value >> 8) & 0xff);
    buf[2] = static_cast<char>((value >> 16) & 0xff);
    buf[3] = static_cast<char>((value >> 24) & 0xff);
}

uint32_t Coding::DecodeFixed32(const char* buf) {
    // 注意：static_cast<uint32_t>(char) 会符号扩展，必须先转成unsigned char
    return (static_cast<uint32_t>(static_cast<unsigned char>(buf[0]))) |
           (static_cast<uint32_t>(static_cast<unsigned char>(buf[1])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(buf[2])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(buf[3])) << 24);
}

void Coding::EncodeFixed64(char* buf, uint64_t value) {
    buf[0] = static_cast<char>(value & 0xff);
    buf[1] = static_cast<char>((value >> 8) & 0xff);
    buf[2] = static_cast<char>((value >> 16) & 0xff);
    buf[3] = static_cast<char>((value >> 24) & 0xff);
    buf[4] = static_cast<char>((value >> 32) & 0xff);
    buf[5] = static_cast<char>((value >> 40) & 0xff);
    buf[6] = static_cast<char>((value >> 48) & 0xff);
    buf[7] = static_cast<char>((value >> 56) & 0xff);
}

uint64_t Coding::DecodeFixed64(const char* buf) {
    uint64_t lo = DecodeFixed32(buf);
    uint64_t hi = DecodeFixed32(buf + 4);
    return (hi << 32) | lo;
}

// ==================== 变长编码实现 ====================

void Coding::EncodeVarint32(std::string* dst, uint32_t value) {
    unsigned char buf[5];
    int pos = 0;
    
    // 每次处理7位，最高位表示是否继续
    while (value >= 0x80) {
        buf[pos++] = static_cast<unsigned char>(value) | 0x80;
        value >>= 7;
    }
    buf[pos++] = static_cast<unsigned char>(value);
    
    dst->append(reinterpret_cast<char*>(buf), pos);
}

bool Coding::GetVarint32(Slice* input, uint32_t* value) {
    // 获取输入数据的指针和长度
    const char* p = input->data();
    // 获取输入数据的结束位置
    const char* limit = p + input->size();
    // 使用备用函数解码，解码后的结果存储在value中
    const char* q = GetVarint32PtrFallback(p, limit, value);
    
    if (q == nullptr) {
        return false; // 数据不完整或损坏
    }
    
    *input = Slice(q, limit - q); // 移动输入指针
    return true;
}

const char* Coding::GetVarint32PtrFallback(const char* p, const char* limit, uint32_t* value) {
    uint32_t result = 0;
    
    // 循环解码，每次解码7位，最高位表示是否继续
    for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
        uint32_t byte = static_cast<unsigned char>(*p);
        p++;
        
        if (byte & 0x80) {
            // 最高位为1，表示还有后续字节
            result |= ((byte & 0x7f) << shift);
        } else {
            // 最高位为0，这是最后一个字节
            result |= (byte << shift);
            *value = result;
            return p; // 返回解码结束的位置
        }
    }
    
    return nullptr; // 数据不完整或超过32位
}

// ==================== Slice编码实现 ====================

void Coding::PutLengthPrefixedSlice(std::string* dst, const Slice& value) {
    // 格式：长度(Varint32) + 数据
    EncodeVarint32(dst, static_cast<uint32_t>(value.size()));
    dst->append(value.data(), value.size());
}

bool Coding::GetLengthPrefixedSlice(Slice* input, Slice* result) {
    uint32_t len = 0;
    
    // 1. 先解码出长度
    if (!GetVarint32(input, &len)) {
        return false;
    }
    
    // 2. 检查剩余数据是否足够
    if (input->size() < len) {
        return false;
    }
    
    // 3. 返回数据切片，并移动输入指针
    *result = Slice(input->data(), len);
    *input = Slice(input->data() + len, input->size() - len);
    return true;
}

// ==================== 浮点数编码实现 ====================

void Coding::EncodeDouble(char* buf, double value) {
    // 将double的位模式转换为整数，保证比较顺序
    uint64_t bits;
    static_assert(sizeof(value) == sizeof(bits), "double must be 64 bits");
    std::memcpy(&bits, &value, sizeof(bits));
    
    // 对负数进行转换，使得按整数比较时顺序与浮点数一致
    if (bits & (1ULL << 63)) {
        // 负数：按位取反
        bits = ~bits;
    } else {
        // 正数：设置最高位
        bits |= (1ULL << 63);
    }
    
    EncodeFixed64(buf, bits);
}

double Coding::DecodeDouble(const char* buf) {
    uint64_t bits = DecodeFixed64(buf);
    
    // 反转编码时的转换
    if (bits & (1ULL << 63)) {
        // 原为正数：清除最高位
        bits &= ~(1ULL << 63);
    } else {
        // 原为负数：按位取反
        bits = ~bits;
    }
    
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

} // namespace utils