#ifndef HASH_INDEX_HPP
#define HASH_INDEX_HPP

#include "IIndex.hpp"
#include "../utils/Hash.hpp"
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <memory>
#include <string>
#include <fstream>
#include <stdexcept>
#include <optional>
#include <list>
#include <cstring>

namespace mementodb {
namespace index {

/**
 * 哈希索引配置
 */
struct HashIndexConfig : public IndexConfig {
    size_t initial_bucket_count = 1024;  // 初始桶数量
    double max_load_factor = 0.75;       // 最大负载因子
    bool thread_safe = true;              // 是否线程安全
    bool enable_rehashing = true;         // 是否启用自动重哈希
    size_t max_bucket_count = 1 << 24;   // 最大桶数量限制
    bool store_keys = true;               // 是否存储键的副本（如果false，依赖外部数据生命周期）
    bool enable_statistics = true;        // 是否启用详细统计
    
    HashIndexConfig() {
        type = IndexType::HASH;
        if (name.empty()) {
            name = "HashIndex_" + std::to_string(reinterpret_cast<uintptr_t>(this));
        }
    }
    
    std::unique_ptr<IndexConfig> clone() const override {
        auto cloned = std::make_unique<HashIndexConfig>(*this);
        return cloned;
    }
    
    bool validate() const override {
        return IndexConfig::validate() &&
               initial_bucket_count > 0 &&
               max_load_factor > 0.1 && max_load_factor < 5.0 &&
               max_bucket_count >= initial_bucket_count;
    }
};

/**
 * 键存储策略：复制键数据以保证独立性
 */
class KeyStorage {
public:
    explicit KeyStorage(const Slice& key) 
        : data_(new char[key.size()]), size_(key.size()) {
        std::memcpy(data_.get(), key.data(), key.size());
    }
    
    KeyStorage(const KeyStorage& other)
        : data_(new char[other.size_]), size_(other.size_) {
        std::memcpy(data_.get(), other.data_.get(), size_);
    }
    
    KeyStorage(KeyStorage&& other) noexcept
        : data_(std::move(other.data_)), size_(other.size_) {
        other.size_ = 0;
    }
    
    KeyStorage& operator=(KeyStorage other) noexcept {
        swap(*this, other);
        return *this;
    }
    
    Slice slice() const { return Slice(data_.get(), size_); }
    size_t size() const { return size_; }
    const char* data() const { return data_.get(); }
    
    friend void swap(KeyStorage& a, KeyStorage& b) noexcept {
        using std::swap;
        swap(a.data_, b.data_);
        swap(a.size_, b.size_);
    }
    
private:
    std::unique_ptr<char[]> data_;
    size_t size_;
};

// 哈希索引实现
class HashIndex : public IIndex {
public:
    explicit HashIndex(const HashIndexConfig& config);
    ~HashIndex() override = default;
    
    // 禁用拷贝，允许移动
    HashIndex(const HashIndex&) = delete;
    HashIndex& operator=(const HashIndex&) = delete;
    HashIndex(HashIndex&&) = default;
    HashIndex& operator=(HashIndex&&) = default;
    
    // IIndex 接口实现
    IndexType get_type() const override { return IndexType::HASH; }
    std::string get_name() const override { return config_.name; }
    
    bool insert(const Slice& key, uint64_t value) override;
    std::optional<uint64_t> get(const Slice& key) const override;
    bool remove(const Slice& key) override;
    bool contains(const Slice& key) const override;
    void clear() override;
    size_t size() const override;
    bool empty() const override;
    
    IndexStatistics get_statistics() const override;
    void reset_statistics() override;
    
    bool persist(const std::string& path = "") const override;
    bool load(const std::string& path) override;
    
    // 批量操作优化版本（重写以返回详细信息）
    std::pair<size_t, std::vector<std::pair<Slice, uint64_t>>> 
    batch_insert(const std::vector<std::pair<Slice, uint64_t>>& entries) override;
    
    std::pair<size_t, std::vector<Slice>> 
    batch_remove(const std::vector<Slice>& keys) override;
    
    // 支持 insert_or_update
    std::optional<uint64_t> insert_or_update(const Slice& key, uint64_t value) override;
    
    // 获取配置
    std::unique_ptr<IndexConfig> get_config() const override;
    
    // 不支持范围查询和前缀查询
    bool supports_range_query() const override { return false; }
    bool supports_prefix_query() const override { return false; }
    
    /**
     * 获取负载因子
     */
    double load_factor() const;
    
    /**
     * 重新哈希（扩容）
     * @param new_bucket_count 新的桶数量
     * @return 成功返回true
     */
    bool rehash(size_t new_bucket_count);
    
    /**
     * 预留空间
     * @param bucket_count 桶数量
     */
    void reserve(size_t bucket_count);
    
    /**
     * 获取桶数量
     */
    size_t bucket_count() const;
    
    /**
     * 获取最大桶深度（用于性能分析）
     */
    size_t max_bucket_depth() const;
    
    /**
     * 压缩索引（清理已删除项，优化内存）
     */
    void shrink_to_fit();
    
    /**
     * 支持快照（创建独立的副本）
     */
    std::shared_ptr<IIndex> snapshot() const override;
    
private:
    // 键值对结构
    struct KeyValuePair {
        KeyStorage key;
        uint64_t value;
        
        KeyValuePair(const Slice& k, uint64_t v) : key(k), value(v) {}
    };
    
    // 使用链表解决哈希冲突（或使用vector，根据配置选择）
    using Bucket = std::list<KeyValuePair>;
    
    // 哈希函数
    struct KeyStorageHash {
        size_t operator()(const KeyStorage& key) const {
            return utils::Hash::HashSlice(key.slice());
        }
    };
    
    // 键比较函数
    struct KeyStorageEqual {
        bool operator()(const KeyStorage& a, const KeyStorage& b) const {
            return a.slice() == b.slice();
        }
    };
    
    HashIndexConfig config_;
    std::vector<Bucket> buckets_;
    size_t element_count_ = 0;
    
    // 线程安全：可选读写锁或互斥锁
    mutable std::shared_mutex mutex_;
    
    // 统计信息
    struct Statistics {
        std::atomic<uint64_t> insert_count{0};
        std::atomic<uint64_t> lookup_count{0};
        std::atomic<uint64_t> delete_count{0};
        std::atomic<uint64_t> hit_count{0};
        std::atomic<uint64_t> miss_count{0};
        std::atomic<uint64_t> rehash_count{0};
        std::atomic<uint64_t> collision_count{0};  // 冲突次数
        std::atomic<size_t> max_bucket_depth{0};   // 最大桶深度
        
        // 获取内存使用量
        size_t memory_usage_bytes() const {
            // 估算：桶数组 + 元素存储
            // 实际实现需要计算具体内存使用
            return 0; // 需要具体实现
        }
    };
    
    mutable Statistics stats_;
    
    // 辅助方法
    size_t hash_to_index(const KeyStorage& key) const {
        KeyStorageHash hasher;
        return hasher(key) % buckets_.size();
    }
    
    size_t hash_to_index(const Slice& key) const {
        return utils::Hash::HashSlice(key) % buckets_.size();
    }
    
    // 线程安全的统计更新
    void record_insert(bool success) const;
    void record_lookup(bool hit) const;
    void record_delete(bool success) const;
    void record_collision(size_t depth) const;
    
    // 内部查找（无锁版本，供内部使用）
    std::optional<uint64_t> find_internal(const Slice& key) const;
    Bucket::iterator find_in_bucket(size_t bucket_idx, const Slice& key);
    Bucket::const_iterator find_in_bucket(size_t bucket_idx, const Slice& key) const;
    
    // 自动重哈希检查
    bool needs_rehashing() const;
    bool perform_auto_rehash();
    
    // 持久化辅助
    struct SerializedHeader {
        char magic[8] = {'H', 'I', 'D', 'X', '1', '.', '0', '\0'};
        uint64_t version = 1;
        uint64_t element_count = 0;
        uint64_t bucket_count = 0;
        double load_factor = 0;
        uint64_t config_flags = 0;
    };
    
    bool save_to_stream(std::ostream& os) const;
    bool load_from_stream(std::istream& is);
    
    // 内存管理
    size_t calculate_memory_usage() const;
};

} // namespace index
} // namespace mementodb

#endif // HASH_INDEX_HPP
