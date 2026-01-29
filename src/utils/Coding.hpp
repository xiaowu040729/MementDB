#pragma once

// File: src/utils/Coding.h
#pragma once

#include "mementodb/Types.h"
#include <cstdint>
#include <string>

namespace utils {
using Slice = mementodb::Slice;

/**
 * 编码/解码工具集 (序列化与反序列化)
 * 核心职责：将数据转换为紧凑、可移植的字节序列，用于持久化和网络传输。
 * 所有函数均为内联或静态，无状态，线程安全。
 */
class Coding {
public:
    // ==================== 固定长度编码（用于定长结构，如页头、元数据）====================
    
    /**
     * 将16位整数编码为小端字节序（2字节）
     */
    static void EncodeFixed16(char* buf, uint16_t value);
    
    /**
     * 从小端字节序解码16位整数
     */
    static uint16_t DecodeFixed16(const char* buf);
    
    /**
     * 将32位整数编码为小端字节序（4字节）
     */
    static void EncodeFixed32(char* buf, uint32_t value);
    
    /**
     * 从小端字节序解码32位整数
     */
    static uint32_t DecodeFixed32(const char* buf);
    
    /**
     * 将64位整数编码为小端字节序（8字节）
     */
    static void EncodeFixed64(char* buf, uint64_t value);
    
    /**
     * 将64位整数编码为大端字节序（8字节）
     */
    static void EncodeBigEndian64(char* buf, uint64_t value);
    
    /**
     * 从小端字节序解码64位整数
     */
    static uint64_t DecodeBigEndian64(const char* buf);
    /**
     * 从小端字节序解码64位整数
     */
    static uint64_t DecodeFixed64(const char* buf);
    
    // 数字更小用更少字节，数字更大用更多字节。
    // ==================== 变长编码（节省空间，用于存储长度等小整数）====================
    
    /**
    * @brief 将32位整数编码为Varint32
    * @param [in] dst 输出字符串
    * @param [in] value 输入整数
    * @return 无
    * 例子：
    * ```cpp
    * std::string buffer;
    * uint32_t value = 12345;
    * Coding::EncodeVarint32(&buffer, value);
    * ```
    * buffer 现在只有1字节（而不是4字节）！
     */
    static void EncodeVarint32(std::string* dst, uint32_t value);
    
    /**
     * 从字节流解码Varint32，并移动输入指针。
     * @param [in/out] input 输入数据，解码后指针会移动到Varint之后。
     * @param [out] value 解码出的整数值。
     * @return 成功返回true，数据不完整或损坏返回false。
     */
    static bool GetVarint32(Slice* input, uint32_t* value);
    
    // 核心功能：把任意二进制数据（键、值等）序列化/反序列化，用在页存储、WAL、网络协议。
    // ==================== Slice 带长度前缀的编码（数据库核心）====================
    
    /**
     * 将Slice编码为：长度(Varint32) + 数据内容
     * 这是数据库序列化键值对的标准格式。
     * 使用场景：
     *   1. 将键值对写入数据页（DiskEngine）
     *   2. 将命令写入WAL日志
     *   3. 网络协议传输
     */
    static void PutLengthPrefixedSlice(std::string* dst, const Slice& value);
    
    /**
     * 从输入Slice中解码出长度前缀的Slice，并移动输入指针。
     * @param [in/out] input 输入数据，解码后指针会移动到Slice之后。
     * @param [out] result 解码出的Slice。
     * @return 成功返回true，数据不完整或损坏返回false。
     */
    static bool GetLengthPrefixedSlice(Slice* input, Slice* result);
    
    // 字符串编码：把字符串转换成字节序列，用在键值对、日志、网络协议。
    // ==================== 浮点数编码（按需可选）====================
    
    /**
     * 将double编码为64位整数表示（保留比较顺序）
     */
    static void EncodeDouble(char* buf, double value);
    
    /**
     * 从64位整数解码为double
     */
    static double DecodeDouble(const char* buf);
    
private:
    // 私有构造函数，禁止实例化
    Coding() = delete;
    
    // 内部辅助函数
    static const char* GetVarint32PtrFallback(const char* p, const char* limit, uint32_t* value);
};

} // namespace utils