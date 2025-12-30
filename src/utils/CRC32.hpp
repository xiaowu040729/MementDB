// File: src/utils/CRC32.hpp
// CRC32校验（使用 crc32c 第三方库）
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace mementodb {
namespace utils {

/**
 * CRC32 - CRC32校验工具类
 * 
 * 使用 crc32c 库（支持硬件加速 SSE4.2 CRC32 指令）
 * 适用于关键路径的数据完整性校验（如 WAL 日志）
 */
class CRC32 {
public:
    /**
     * 计算数据的 CRC32 校验值
     * @param data 输入数据指针
     * @param size 数据长度（字节）
     * @param initial_crc 初始 CRC 值（用于增量计算，默认为0）
     * @return 32位 CRC 校验值
     */
    static uint32_t Compute(const void* data, size_t size, uint32_t initial_crc = 0);
    
    /**
     * 计算字符串的 CRC32 校验值
     * @param str 输入字符串
     * @param initial_crc 初始 CRC 值
     * @return 32位 CRC 校验值
     */
    static uint32_t ComputeString(const std::string& str, uint32_t initial_crc = 0);
    
    /**
     * 增量计算 CRC32（适用于流式数据）
     * @param data 输入数据指针
     * @param size 数据长度
     * @param current_crc 当前 CRC 值
     * @return 更新后的 CRC 值
     */
    static uint32_t Extend(const void* data, size_t size, uint32_t current_crc);
    
    /**
     * 合并两个 CRC32 值（用于合并多个数据块的校验值）
     * @param crc1 第一个 CRC 值
     * @param crc2 第二个 CRC 值
     * @param len2 第二个数据块的长度
     * @return 合并后的 CRC 值
     */
    static uint32_t Concat(uint32_t crc1, uint32_t crc2, size_t len2);

private:
    CRC32() = delete;  // 工具类，禁止实例化
};

} // namespace utils
} // namespace mementodb

