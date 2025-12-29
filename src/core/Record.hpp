#pragma once

// File: src/core/Record.hpp
// 记录 / 键值对存储格式（页内布局）

#include <cstdint>
#include <cstddef>
#include <cstring>
#include "mementodb/Types.h"

namespace mementodb {
namespace core {

/**
 * Record - 页内键值对存储格式（简单定长头 + 不定长数据）
 *
 * 内存布局：
 * | key_size (uint32) | value_size (uint32) | key bytes ... | value bytes ... |
 *
 * 特点：
 * - 无额外对齐要求（按字节紧凑存储）
 * - header 固定 8 字节
 * - 支持零拷贝解码（返回 Slice 指向原缓冲区）
 */
class Record {
public:
    struct Header {
        uint32_t key_size;
        uint32_t value_size;
    };

    static constexpr size_t kHeaderSize = sizeof(Header); // 8 字节

    /** 计算存储指定 key/value 所需总字节数 */
    static constexpr size_t ByteSize(uint32_t key_size, uint32_t value_size) {
        return kHeaderSize + key_size + value_size;
    }

    /** 将 key/value 编码到目标缓冲区（缓冲区需至少 ByteSize 大小） */
    static void Encode(char* dst, const Slice& key, const Slice& value) {
        // 将 key 和 value 编码到目标缓冲区
        auto* hdr = reinterpret_cast<Header*>(dst);
        hdr->key_size = static_cast<uint32_t>(key.size());
        hdr->value_size = static_cast<uint32_t>(value.size());

        // 将 key 和 value 复制到目标缓冲区
        char* p = dst + kHeaderSize;
        if (key.size() > 0) {
            std::memcpy(p, key.data(), key.size());
            p += key.size();
        }
        if (value.size() > 0) {
            std::memcpy(p, value.data(), value.size());
        }
    }

    /**
     * 从缓冲区解码 Record，返回 key/value 的 Slice（指向原缓冲区，零拷贝）。
     * @param buf     输入缓冲区
     * @param buf_len 缓冲区长度
     * @param key_out 返回的 key 视图
     * @param val_out 返回的 value 视图
     * @return 成功返回 true；长度不足返回 false
     */
    static bool Decode(const char* buf, size_t buf_len,
                       Slice* key_out, Slice* val_out) {
        if (buf_len < kHeaderSize) return false;

        // 解析 header
        const auto* hdr = reinterpret_cast<const Header*>(buf);
        // 计算需要的字节数
        const size_t needed = kHeaderSize +
                              static_cast<size_t>(hdr->key_size) +
                              static_cast<size_t>(hdr->value_size);
        if (buf_len < needed) return false;

        // 解析 key 和 value
        const char* p = buf + kHeaderSize;
        // 返回 key 视图
        *key_out = Slice(p, hdr->key_size);
        p += hdr->key_size;
        // 返回 value 视图
        *val_out = Slice(p, hdr->value_size);
        return true;
    }
};

} // namespace core
} // namespace mementodb

