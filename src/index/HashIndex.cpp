#include "HashIndex.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace mementodb {
namespace index {

HashIndex::HashIndex(const HashIndexConfig& config) 
    : IIndex(), config_(config), element_count_(0) {
    if (!config_.validate()) {
        throw std::invalid_argument("Invalid HashIndexConfig");
    }
    
    buckets_.resize(config_.initial_bucket_count);
    stats_.max_bucket_depth = 0;
}

bool HashIndex::insert(const Slice& key, uint64_t value) {
    std::unique_lock lock(mutex_);
    
    try {
        size_t bucket_idx = hash_to_index(key);
        Bucket& bucket = buckets_[bucket_idx];
        
        // 检查键是否已存在
        auto it = find_in_bucket(bucket_idx, key);
        if (it != bucket.end()) {
            // 更新值
            it->value = value;
            record_insert(true);
            return true;
        }
        
        // 插入新键值对
        bucket.emplace_back(key, value);
        element_count_++;
        
        // 记录统计
        record_insert(true);
        record_collision(bucket.size());
        
        // 检查是否需要自动重哈希
        if (config_.enable_rehashing && needs_rehashing()) {
            lock.unlock();
            perform_auto_rehash();
        }
        
        return true;
    } catch (...) {
        record_insert(false);
        return false;
    }
}

std::optional<uint64_t> HashIndex::get(const Slice& key) const {
    std::shared_lock lock(mutex_);
    
    stats_.lookup_count++;
    
    auto result = find_internal(key);
    if (result.has_value()) {
        record_lookup(true);
        return result;
    } else {
        record_lookup(false);
        return std::nullopt;
    }
}

bool HashIndex::remove(const Slice& key) {
    std::unique_lock lock(mutex_);
    
    try {
        size_t bucket_idx = hash_to_index(key);
        Bucket& bucket = buckets_[bucket_idx];
        
        auto it = find_in_bucket(bucket_idx, key);
        if (it != bucket.end()) {
            bucket.erase(it);
            element_count_--;
            record_delete(true);
            return true;
        }
        
        record_delete(false);
        return false;
    } catch (...) {
        record_delete(false);
        return false;
    }
}

bool HashIndex::contains(const Slice& key) const {
    return get(key).has_value();
}

void HashIndex::clear() {
    std::unique_lock lock(mutex_);
    
    for (auto& bucket : buckets_) {
        bucket.clear();
    }
    element_count_ = 0;
    lock.unlock();
    reset_statistics();
}

size_t HashIndex::size() const {
    std::shared_lock lock(mutex_);
    return element_count_;
}

bool HashIndex::empty() const {
    return size() == 0;
}

IndexStatistics HashIndex::get_statistics() const {
    std::shared_lock lock(mutex_);
    
    // 从原子变量读取值并设置到普通变量
    IndexStatistics stats;
    stats.total_keys = element_count_;
    stats.memory_usage_bytes = calculate_memory_usage();
    stats.insert_count = stats_.insert_count.load();
    stats.lookup_count = stats_.lookup_count.load();
    stats.delete_count = stats_.delete_count.load();
    stats.hit_count = stats_.hit_count.load();
    stats.miss_count = stats_.miss_count.load();
    
    return stats;
}

void HashIndex::reset_statistics() {
    std::unique_lock lock(mutex_);
    // 原子变量不能直接赋值，需要逐个重置
    stats_.insert_count.store(0);
    stats_.lookup_count.store(0);
    stats_.delete_count.store(0);
    stats_.hit_count.store(0);
    stats_.miss_count.store(0);
    stats_.rehash_count.store(0);
    stats_.collision_count.store(0);
    stats_.max_bucket_depth.store(0);
}

bool HashIndex::persist(const std::string& path) const {
    std::string filepath = path.empty() ? config_.name + ".idx" : path;
    std::ofstream ofs(filepath, std::ios::binary);
    if (!ofs) {
        return false;
    }
    
    return save_to_stream(ofs);
}

bool HashIndex::load(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return false;
    }
    
    std::unique_lock lock(mutex_);
    return load_from_stream(ifs);
}

std::pair<size_t, std::vector<std::pair<Slice, uint64_t>>> 
HashIndex::batch_insert(const std::vector<std::pair<Slice, uint64_t>>& entries) {
    std::vector<std::pair<Slice, uint64_t>> failed;
    size_t success_count = 0;
    
    for (const auto& entry : entries) {
        try {
            if (insert(entry.first, entry.second)) {
                success_count++;
            } else {
                failed.push_back(entry);
            }
        } catch (...) {
            failed.push_back(entry);
        }
    }
    
    return {success_count, failed};
}

std::pair<size_t, std::vector<Slice>> 
HashIndex::batch_remove(const std::vector<Slice>& keys) {
    std::vector<Slice> failed;
    size_t success_count = 0;
    
    for (const auto& key : keys) {
        try {
            if (remove(key)) {
                success_count++;
            } else {
                failed.push_back(key);
            }
        } catch (...) {
            failed.push_back(key);
        }
    }
    
    return {success_count, failed};
}

std::optional<uint64_t> HashIndex::insert_or_update(const Slice& key, uint64_t value) {
    std::unique_lock lock(mutex_);
    
    size_t bucket_idx = hash_to_index(key);
    Bucket& bucket = buckets_[bucket_idx];
    
    auto it = find_in_bucket(bucket_idx, key);
    if (it != bucket.end()) {
        uint64_t old_value = it->value;
        it->value = value;
        record_insert(true);
        return old_value;
    }
    
    bucket.emplace_back(key, value);
    element_count_++;
    record_insert(true);
    record_collision(bucket.size());
    
    if (config_.enable_rehashing && needs_rehashing()) {
        lock.unlock();
        perform_auto_rehash();
    }
    
    return std::nullopt;
}

std::unique_ptr<IndexConfig> HashIndex::get_config() const {
    return std::make_unique<HashIndexConfig>(config_);
}

double HashIndex::load_factor() const {
    std::shared_lock lock(mutex_);
    if (buckets_.empty()) return 0.0;
    return static_cast<double>(element_count_) / buckets_.size();
}

bool HashIndex::rehash(size_t new_bucket_count) {
    if (new_bucket_count > config_.max_bucket_count) {
        return false;
    }
    
    std::unique_lock lock(mutex_);
    
    std::vector<Bucket> new_buckets(new_bucket_count);
    
    // 重新分配所有元素
    for (auto& bucket : buckets_) {
        for (auto& kv : bucket) {
            size_t new_idx = hash_to_index(kv.key);
            new_buckets[new_idx].push_back(std::move(kv));
        }
    }
    
    buckets_ = std::move(new_buckets);
    stats_.rehash_count++;
    
    return true;
}

void HashIndex::reserve(size_t bucket_count) {
    if (bucket_count > buckets_.size()) {
        rehash(bucket_count);
    }
}

size_t HashIndex::bucket_count() const {
    std::shared_lock lock(mutex_);
    return buckets_.size();
}

size_t HashIndex::max_bucket_depth() const {
    std::shared_lock lock(mutex_);
    
    size_t max_depth = 0;
    for (const auto& bucket : buckets_) {
        max_depth = std::max(max_depth, bucket.size());
    }
    
    return max_depth;
}

void HashIndex::shrink_to_fit() {
    std::unique_lock lock(mutex_);
    // 对于链表实现，shrink_to_fit 主要是清理未使用的桶
    // 这里可以触发一次重哈希以优化内存
    if (element_count_ > 0) {
        size_t optimal_size = static_cast<size_t>(
            element_count_ / config_.max_load_factor);
        if (optimal_size < buckets_.size()) {
            rehash(optimal_size);
        }
    }
}

std::shared_ptr<IIndex> HashIndex::snapshot() const {
    std::shared_lock lock(mutex_);
    
    auto snapshot_index = std::static_pointer_cast<HashIndex>(
        std::make_shared<HashIndex>(config_));
    snapshot_index->buckets_.resize(buckets_.size());
    snapshot_index->element_count_ = element_count_;
    
    // 深拷贝所有元素
    for (size_t i = 0; i < buckets_.size(); ++i) {
        for (const auto& kv : buckets_[i]) {
            snapshot_index->buckets_[i].emplace_back(kv.key.slice(), kv.value);
        }
    }
    
    // 复制统计信息
    // 原子变量不能直接赋值，需要逐个复制
    snapshot_index->stats_.insert_count.store(stats_.insert_count.load());
    snapshot_index->stats_.lookup_count.store(stats_.lookup_count.load());
    snapshot_index->stats_.delete_count.store(stats_.delete_count.load());
    snapshot_index->stats_.hit_count.store(stats_.hit_count.load());
    snapshot_index->stats_.miss_count.store(stats_.miss_count.load());
    snapshot_index->stats_.rehash_count.store(stats_.rehash_count.load());
    snapshot_index->stats_.collision_count.store(stats_.collision_count.load());
    snapshot_index->stats_.max_bucket_depth.store(stats_.max_bucket_depth.load());
    
    return snapshot_index;
}

// 私有辅助方法实现

void HashIndex::record_insert(bool success) const {
    if (config_.enable_statistics) {
        if (success) {
            stats_.insert_count++;
        }
    }
}

void HashIndex::record_lookup(bool hit) const {
    if (config_.enable_statistics) {
        if (hit) {
            stats_.hit_count++;
        } else {
            stats_.miss_count++;
        }
    }
}

void HashIndex::record_delete(bool success) const {
    if (config_.enable_statistics) {
        if (success) {
            stats_.delete_count++;
        }
    }
}

void HashIndex::record_collision(size_t depth) const {
    if (config_.enable_statistics && depth > 1) {
        stats_.collision_count++;
        size_t current_max = stats_.max_bucket_depth.load();
        while (depth > current_max && 
               !stats_.max_bucket_depth.compare_exchange_weak(current_max, depth)) {
            current_max = stats_.max_bucket_depth.load();
        }
    }
}

std::optional<uint64_t> HashIndex::find_internal(const Slice& key) const {
    size_t bucket_idx = hash_to_index(key);
    const Bucket& bucket = buckets_[bucket_idx];
    
    auto it = find_in_bucket(bucket_idx, key);
    if (it != bucket.end()) {
        return it->value;
    }
    return std::nullopt;
}

HashIndex::Bucket::iterator HashIndex::find_in_bucket(size_t bucket_idx, const Slice& key) {
    Bucket& bucket = buckets_[bucket_idx];
    return std::find_if(bucket.begin(), bucket.end(),
        [&key](const KeyValuePair& kv) {
            return kv.key.slice() == key;
        });
}

HashIndex::Bucket::const_iterator HashIndex::find_in_bucket(size_t bucket_idx, const Slice& key) const {
    const Bucket& bucket = buckets_[bucket_idx];
    return std::find_if(bucket.begin(), bucket.end(),
        [&key](const KeyValuePair& kv) {
            return kv.key.slice() == key;
        });
}

bool HashIndex::needs_rehashing() const {
    if (buckets_.empty()) return false;
    double current_load = static_cast<double>(element_count_) / buckets_.size();
    return current_load > config_.max_load_factor && 
           buckets_.size() * 2 <= config_.max_bucket_count;
}

bool HashIndex::perform_auto_rehash() {
    size_t new_size = buckets_.size() * 2;
    if (new_size > config_.max_bucket_count) {
        new_size = config_.max_bucket_count;
    }
    return rehash(new_size);
}

bool HashIndex::save_to_stream(std::ostream& os) const {
    std::shared_lock lock(mutex_);
    
    SerializedHeader header;
    header.element_count = element_count_;
    header.bucket_count = buckets_.size();
    header.load_factor = load_factor();
    
    // 写入头部
    os.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (!os) return false;
    
    // 写入所有键值对
    for (const auto& bucket : buckets_) {
        for (const auto& kv : bucket) {
            // 写入键长度和键数据
            size_t key_size = kv.key.size();
            os.write(reinterpret_cast<const char*>(&key_size), sizeof(key_size));
            os.write(kv.key.data(), key_size);
            
            // 写入值
            os.write(reinterpret_cast<const char*>(&kv.value), sizeof(kv.value));
            
            if (!os) return false;
        }
    }
    
    return true;
}

bool HashIndex::load_from_stream(std::istream& is) {
    SerializedHeader header;
    is.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!is) return false;
    
    // 验证魔数
    if (std::memcmp(header.magic, "HIDX1.0", 8) != 0) {
        return false;
    }
    
    // 清空现有数据
    clear();
    buckets_.resize(header.bucket_count);
    
    // 读取所有键值对
    for (uint64_t i = 0; i < header.element_count; ++i) {
        size_t key_size;
        is.read(reinterpret_cast<char*>(&key_size), sizeof(key_size));
        if (!is) return false;
        
        std::vector<char> key_data(key_size);
        is.read(key_data.data(), key_size);
        if (!is) return false;
        
        uint64_t value;
        is.read(reinterpret_cast<char*>(&value), sizeof(value));
        if (!is) return false;
        
        Slice key(key_data.data(), key_size);
        insert(key, value);
    }
    
    return true;
}

size_t HashIndex::calculate_memory_usage() const {
    size_t usage = sizeof(HashIndex);
    usage += buckets_.size() * sizeof(Bucket);
    
    for (const auto& bucket : buckets_) {
        usage += bucket.size() * sizeof(KeyValuePair);
        for (const auto& kv : bucket) {
            usage += kv.key.size();
        }
    }
    
    return usage;
}

} // namespace index
} // namespace mementodb

