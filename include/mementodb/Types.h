#pragma once

#include <cstddef>
#include <cstring>
#include <string>

namespace mementodb {

/**
 * Slice - 字符串/字节数组的轻量级视图
 * 用于避免不必要的字符串拷贝
 */
class Slice {
public:
    Slice() : data_(nullptr), size_(0) {}
    
    Slice(const char* data, size_t size) : data_(data), size_(size) {}
    
    Slice(const std::string& str) : data_(str.data()), size_(str.size()) {}
    
    Slice(const char* str) : data_(str), size_(str ? strlen(str) : 0) {}
    
    // 访问器
    const char* data() const { return data_; }
    size_t size() const { return size_; }
    
    // 判断是否为空
    bool empty() const { return size_ == 0; }
    
    // 转换为字符串
    std::string ToString() const {
        return std::string(data_, size_);
    }
    
    // 比较操作
    int compare(const Slice& other) const {
        const size_t min_len = (size_ < other.size_) ? size_ : other.size_;
        int r = memcmp(data_, other.data_, min_len);
        if (r == 0) {
            if (size_ < other.size_) r = -1;
            else if (size_ > other.size_) r = +1;
        }
        return r;
    }
    
    bool operator==(const Slice& other) const {
        return size_ == other.size_ && memcmp(data_, other.data_, size_) == 0;
    }
    
    bool operator!=(const Slice& other) const {
        return !(*this == other);
    }
    
private:
    const char* data_;
    size_t size_;
};

} // namespace mementodb

