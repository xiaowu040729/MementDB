#include "SkipListIndex.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <limits>

namespace mementodb {
namespace index {

// SkipListNode 实现
SkipListNode::SkipListNode(const std::string& key, uint64_t value, int level)
    : key_(key), value_(value), level_(level), forward_(level + 1, nullptr) {
}

SkipListNode::~SkipListNode() = default;

// SkipListIndex 实现
SkipListIndex::SkipListIndex(const SkipListIndexConfig& config) 
    : config_(config), head_(nullptr), max_level_(0), size_(0) {
    
    if (!config_.validate()) {
        throw std::invalid_argument("Invalid SkipListIndexConfig");
    }
    
    // 创建头节点（最大层数）
    std::string empty_key;
    head_ = new SkipListNode(empty_key, 0, config_.max_level);
    max_level_ = 0;
    
    // 初始化层级统计（size_t 可安全按值拷贝/移动）
    const size_t needed_size = static_cast<size_t>(config_.max_level + 1);
    if (level_stats_.size() != needed_size) {
        level_stats_.assign(needed_size, 0);
    } else {
        std::fill(level_stats_.begin(), level_stats_.end(), 0);
    }
    
    // 初始化节点池
    if (config_.enable_node_pool) {
        init_node_pool();
    }
}

SkipListIndex::~SkipListIndex() {
    clear_helper();
    if (head_) {
        delete head_;
    }
    cleanup_node_pool();
}

int SkipListIndex::random_level() const {
    // 使用 thread_local 确保线程安全
    thread_local std::mt19937 rng(std::random_device{}());
    thread_local std::uniform_real_distribution<double> dist(0.0, 1.0);
    
    int level = 1;
    while (level < config_.max_level && dist(rng) < config_.probability) {
        ++level;
    }
    return level;
}

bool SkipListIndex::insert(const Slice& key, uint64_t value) {
    std::unique_lock lock(mutex_);
    
    try {
        std::string key_str(key.data(), key.size());
        
        // 查找插入位置
        std::vector<SkipListNode*> update(config_.max_level + 1, nullptr);
        find_predecessors(key, update);
        
        // 检查键是否已存在
        SkipListNode* existing = update[0]->get_next(0);
        if (existing && existing->get_key() == key_str) {
            if (config_.allow_duplicates) {
                existing->set_value(value);
                stats_.insert_count++;
                return true;
            } else {
                // 不允许重复，更新值
                existing->set_value(value);
                stats_.insert_count++;
                return true;
            }
        }
        
        // 生成新节点的层数
        int new_level = random_level();
        
        // 更新最大层数
        if (new_level > max_level_) {
            for (int i = max_level_ + 1; i <= new_level; ++i) {
                update[i] = head_;
            }
            max_level_ = new_level;
        }
        
        // 创建新节点
        SkipListNode* new_node = allocate_node(key_str, value, new_level);
        
        // 插入到各层
        for (int i = 0; i <= new_level; ++i) {
            new_node->set_next(i, update[i]->get_next(i));
            update[i]->set_next(i, new_node);
        }
        
        size_++;
        level_stats_[new_level]++;
        stats_.insert_count++;
        stats_.node_allocations_++;
        
        update_memory_stats();
        
        // 检查是否需要优化
        if (config_.enable_optimization && should_optimize()) {
            lock.unlock();
            optimize();
        }
        
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<uint64_t> SkipListIndex::get(const Slice& key) const {
    std::shared_lock lock(mutex_);
    
    stats_.lookup_count++;
    
    SkipListNode* node = find_node(key);
    if (node) {
        stats_.hit_count++;
        return node->get_value();
    } else {
        stats_.miss_count++;
        return std::nullopt;
    }
}

bool SkipListIndex::remove(const Slice& key) {
    std::unique_lock lock(mutex_);
    
    try {
        std::string key_str(key.data(), key.size());
        std::vector<SkipListNode*> update(config_.max_level + 1, nullptr);
        find_predecessors(key, update);
        
        SkipListNode* node = update[0]->get_next(0);
        if (!node || node->get_key() != key_str) {
            stats_.delete_count++;
            return false;
        }
        
        // 从各层删除
        delete_node(node, update);
        
        size_--;
        level_stats_[node->get_level()]--;
        stats_.delete_count++;
        stats_.node_deallocations_++;
        
        deallocate_node(node);
        update_memory_stats();
        
        return true;
    } catch (...) {
        return false;
    }
}

bool SkipListIndex::contains(const Slice& key) const {
    return get(key).has_value();
}

void SkipListIndex::clear() {
    std::unique_lock lock(mutex_);
    clear_helper();
}

void SkipListIndex::clear_helper() {
    SkipListNode* current = head_->get_next(0);
    while (current) {
        SkipListNode* next = current->get_next(0);
        deallocate_node(current);
        current = next;
    }
    
    // 重置头节点
    for (int i = 0; i <= config_.max_level; ++i) {
        head_->set_next(i, nullptr);
    }
    
    size_ = 0;
    max_level_ = 0;
    
    std::fill(level_stats_.begin(), level_stats_.end(), 0);
}

size_t SkipListIndex::size() const {
    return size_.load();
}

bool SkipListIndex::empty() const {
    return size() == 0;
}

IndexStatistics SkipListIndex::get_statistics() const {
    std::shared_lock lock(mutex_);
    
    IndexStatistics stats;
    stats.total_keys = size_.load();
    stats.memory_usage_bytes = memory_usage();
    stats.insert_count = stats_.insert_count.load();
    stats.lookup_count = stats_.lookup_count.load();
    stats.delete_count = stats_.delete_count.load();
    stats.hit_count = stats_.hit_count.load();
    stats.miss_count = stats_.miss_count.load();
    
    return stats;
}

void SkipListIndex::reset_statistics() {
    std::unique_lock lock(mutex_);
    // 原子变量不能直接赋值，需要逐个重置
    stats_.insert_count.store(0);
    stats_.lookup_count.store(0);
    stats_.delete_count.store(0);
    stats_.hit_count.store(0);
    stats_.miss_count.store(0);
    stats_.memory_usage_.store(0);
    stats_.node_allocations_.store(0);
    stats_.node_deallocations_.store(0);
    stats_.optimization_count.store(0);
}

bool SkipListIndex::persist(const std::string& path) const {
    std::string filepath = path.empty() ? config_.name + ".idx" : path;
    std::ofstream ofs(filepath, std::ios::binary);
    if (!ofs) {
        return false;
    }
    
    return save_to_stream(ofs);
}

bool SkipListIndex::load(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return false;
    }
    
    std::unique_lock lock(mutex_);
    return load_from_stream(ifs);
}

std::optional<uint64_t> SkipListIndex::insert_or_update(const Slice& key, uint64_t value) {
    std::unique_lock lock(mutex_);
    
    std::string key_str(key.data(), key.size());
    SkipListNode* existing = find_node(key);
    
    if (existing) {
        uint64_t old_value = existing->get_value();
        existing->set_value(value);
        stats_.insert_count++;
        return old_value;
    } else {
        insert(key, value);
        return std::nullopt;
    }
}

RangeResult SkipListIndex::range_query(const Slice& start_key, 
                                       const Slice& end_key, 
                                       size_t limit) const {
    std::shared_lock lock(mutex_);
    
    RangeResult result;
    
    try {
        std::string start_str(start_key.data(), start_key.size());
        std::string end_str(end_key.data(), end_key.size());
        
        // 找到起始位置
        SkipListNode* current = lower_bound(start_key).get_current();
        if (!current) {
            current = head_->get_next(0);
        }
        
        size_t count = 0;
        while (current && (limit == 0 || count < limit)) {
            std::string current_key = current->get_key();
            
            // 检查是否超出范围
            if (current_key > end_str) {
                break;
            }
            
            if (current_key >= start_str) {
                result.entries.emplace_back(
                    current->get_key_slice(),
                    current->get_value()
                );
                count++;
            }
            
            current = current->get_next(0);
        }
        
        result.has_more = (current != nullptr && current->get_key() <= end_str);
        stats_.lookup_count++;
    } catch (const std::exception& e) {
        result.error_msg = e.what();
    }
    
    return result;
}

RangeResult SkipListIndex::prefix_query(const Slice& prefix, size_t limit) const {
    std::shared_lock lock(mutex_);
    
    RangeResult result;
    
    try {
        std::string prefix_str(prefix.data(), prefix.size());
        
        // 找到前缀匹配的起始位置
        SkipListNode* current = lower_bound(prefix).get_current();
        if (!current) {
            current = head_->get_next(0);
        }
        
        size_t count = 0;
        while (current && (limit == 0 || count < limit)) {
            std::string current_key = current->get_key();
            
            // 检查前缀匹配
            if (current_key.size() < prefix_str.size() || 
                current_key.substr(0, prefix_str.size()) != prefix_str) {
                break;
            }
            
            result.entries.emplace_back(
                current->get_key_slice(),
                current->get_value()
            );
            count++;
            
            current = current->get_next(0);
        }
        
        result.has_more = (current != nullptr && 
                          current->get_key().size() >= prefix_str.size() &&
                          current->get_key().substr(0, prefix_str.size()) == prefix_str);
        stats_.lookup_count++;
    } catch (const std::exception& e) {
        result.error_msg = e.what();
    }
    
    return result;
}

std::unique_ptr<IndexConfig> SkipListIndex::get_config() const {
    return std::make_unique<SkipListIndexConfig>(config_);
}

std::optional<std::pair<Slice, uint64_t>> SkipListIndex::min() const {
    std::shared_lock lock(mutex_);
    
    SkipListNode* first = head_->get_next(0);
    if (!first) {
        return std::nullopt;
    }
    
    return std::make_pair(first->get_key_slice(), first->get_value());
}

std::optional<std::pair<Slice, uint64_t>> SkipListIndex::max() const {
    std::shared_lock lock(mutex_);
    
    SkipListNode* current = head_;
    
    // 从最高层开始，找到最右边的节点
    for (int level = max_level_; level >= 0; --level) {
        while (current->get_next(level)) {
            current = current->get_next(level);
        }
    }
    
    if (current == head_) {
        return std::nullopt;
    }
    
    return std::make_pair(current->get_key_slice(), current->get_value());
}

std::pair<size_t, std::vector<std::pair<Slice, uint64_t>>> 
SkipListIndex::batch_insert(const std::vector<std::pair<Slice, uint64_t>>& entries) {
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
SkipListIndex::batch_remove(const std::vector<Slice>& keys) {
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

size_t SkipListIndex::memory_usage() const {
    std::shared_lock lock(mutex_);
    return stats_.memory_usage_.load();
}

void SkipListIndex::optimize() {
    std::unique_lock lock(mutex_);
    
    if (!should_optimize()) {
        return;
    }
    
    compress_levels();
    stats_.optimization_count++;
    
    if (monitor_callback_) {
        monitor_callback_("optimize", size_.load());
    }
}

bool SkipListIndex::create_snapshot(const std::string& path) const {
    return persist(path);
}

bool SkipListIndex::restore_from_snapshot(const std::string& path) {
    return load(path);
}

void SkipListIndex::set_monitor_callback(MonitorCallback callback) {
    std::unique_lock lock(mutex_);
    monitor_callback_ = callback;
}

SkipListIterator SkipListIndex::begin() const {
    std::shared_lock lock(mutex_);
    return SkipListIterator(head_->get_next(0));
}

SkipListIterator SkipListIndex::lower_bound(const Slice& key) const {
    std::shared_lock lock(mutex_);
    
    std::string key_str(key.data(), key.size());
    SkipListNode* current = head_;
    
    // 从最高层开始查找
    for (int level = max_level_; level >= 0; --level) {
        while (current->get_next(level) && 
               current->get_next(level)->get_key() < key_str) {
            current = current->get_next(level);
        }
    }
    
    current = current->get_next(0);
    return SkipListIterator(current);
}

SkipListIterator SkipListIndex::upper_bound(const Slice& key) const {
    std::shared_lock lock(mutex_);
    
    std::string key_str(key.data(), key.size());
    SkipListNode* current = head_;
    
    // 从最高层开始查找
    for (int level = max_level_; level >= 0; --level) {
        while (current->get_next(level) && 
               current->get_next(level)->get_key() <= key_str) {
            current = current->get_next(level);
        }
    }
    
    current = current->get_next(0);
    return SkipListIterator(current);
}

#ifdef TEST_BUILD
bool SkipListIndex::verify_integrity() const {
    std::shared_lock lock(mutex_);
    
    // 检查每层的完整性
    for (int level = 0; level <= max_level_; ++level) {
        SkipListNode* current = head_;
        while (current->get_next(level)) {
            SkipListNode* next = current->get_next(level);
            if (level > 0 && current != head_) {
                // 检查排序
                if (current->get_key() >= next->get_key()) {
                    return false;
                }
            }
            current = next;
        }
    }
    
    return true;
}

size_t SkipListIndex::get_level_count(int level) const {
    std::shared_lock lock(mutex_);
    if (level < 0 || level >= static_cast<int>(level_stats_.size())) {
        return 0;
    }
    return level_stats_[level];
}
#endif

// 私有辅助方法

SkipListNode* SkipListIndex::find_node(const Slice& key) const {
    std::string key_str(key.data(), key.size());
    SkipListNode* current = head_;
    
    // 从最高层开始查找
    for (int level = max_level_; level >= 0; --level) {
        while (current->get_next(level) && 
               current->get_next(level)->get_key() < key_str) {
            current = current->get_next(level);
        }
    }
    
    current = current->get_next(0);
    if (current && current->get_key() == key_str) {
        return current;
    }
    
    return nullptr;
}

void SkipListIndex::find_predecessors(const Slice& key, 
                                     std::vector<SkipListNode*>& update) const {
    std::string key_str(key.data(), key.size());
    SkipListNode* current = head_;
    
    for (int level = max_level_; level >= 0; --level) {
        while (current->get_next(level) && 
               current->get_next(level)->get_key() < key_str) {
            current = current->get_next(level);
        }
        update[level] = current;
    }
}

void SkipListIndex::delete_node(SkipListNode* node, 
                                const std::vector<SkipListNode*>& update) {
    int node_level = node->get_level();
    
    for (int i = 0; i <= node_level; ++i) {
        update[i]->set_next(i, node->get_next(i));
    }
    
    // 更新最大层数
    while (max_level_ > 0 && head_->get_next(max_level_) == nullptr) {
        max_level_--;
    }
}

SkipListNode* SkipListIndex::allocate_node(const Slice& key, uint64_t value, int level) {
    if (config_.enable_node_pool && !free_nodes_.empty()) {
        std::lock_guard pool_lock(pool_mutex_);
        if (!free_nodes_.empty()) {
            SkipListNode* node = free_nodes_.back();
            free_nodes_.pop_back();
            // 重用节点（需要重新初始化）
            // 注意：这里简化处理，实际应该使用placement new
            return new SkipListNode(std::string(key.data(), key.size()), value, level);
        }
    }
    
    return new SkipListNode(std::string(key.data(), key.size()), value, level);
}

void SkipListIndex::deallocate_node(SkipListNode* node) {
    if (config_.enable_node_pool && free_nodes_.size() < config_.node_pool_size) {
        std::lock_guard pool_lock(pool_mutex_);
        if (free_nodes_.size() < config_.node_pool_size) {
            free_nodes_.push_back(node);
            return;
        }
    }
    
    delete node;
}

void SkipListIndex::init_node_pool() {
    std::lock_guard lock(pool_mutex_);
    free_nodes_.reserve(config_.node_pool_size);
}

void SkipListIndex::cleanup_node_pool() {
    std::lock_guard lock(pool_mutex_);
    for (auto* node : free_nodes_) {
        delete node;
    }
    free_nodes_.clear();
}

void SkipListIndex::update_memory_stats() {
    size_t usage = sizeof(SkipListIndex);
    usage += sizeof(SkipListNode) * size_.load();
    
    // 估算键的内存使用
    SkipListNode* current = head_->get_next(0);
    while (current) {
        usage += current->get_key().size();
        current = current->get_next(0);
    }
    
    stats_.memory_usage_ = usage;
}

bool SkipListIndex::save_to_stream(std::ostream& os) const {
    // 写入魔数和版本
    const char magic[] = "SKIP1.0";
    os.write(magic, 8);
    
    uint64_t version = 1;
    os.write(reinterpret_cast<const char*>(&version), sizeof(version));
    
    // 写入配置
    os.write(reinterpret_cast<const char*>(&config_.max_level), sizeof(config_.max_level));
    os.write(reinterpret_cast<const char*>(&config_.probability), sizeof(config_.probability));
    
    // 写入大小
    uint64_t size = size_.load();
    os.write(reinterpret_cast<const char*>(&size), sizeof(size));
    
    // 写入所有节点
    SkipListNode* current = head_->get_next(0);
    while (current) {
        const std::string& key = current->get_key();
        size_t key_size = key.size();
        os.write(reinterpret_cast<const char*>(&key_size), sizeof(key_size));
        os.write(key.data(), key_size);
        
        uint64_t value = current->get_value();
        os.write(reinterpret_cast<const char*>(&value), sizeof(value));
        
        current = current->get_next(0);
    }
    
    return os.good();
}

bool SkipListIndex::load_from_stream(std::istream& is) {
    // 读取魔数
    char magic[8];
    is.read(magic, 8);
    if (std::memcmp(magic, "SKIP1.0", 8) != 0) {
        return false;
    }
    
    uint64_t version;
    is.read(reinterpret_cast<char*>(&version), sizeof(version));
    
    // 读取配置
    int max_level;
    double probability;
    is.read(reinterpret_cast<char*>(&max_level), sizeof(max_level));
    is.read(reinterpret_cast<char*>(&probability), sizeof(probability));
    
    // 清空现有数据
    clear_helper();
    
    // 读取大小
    uint64_t size;
    is.read(reinterpret_cast<char*>(&size), sizeof(size));
    
    // 读取所有节点
    for (uint64_t i = 0; i < size; ++i) {
        size_t key_size;
        is.read(reinterpret_cast<char*>(&key_size), sizeof(key_size));
        
        std::vector<char> key_data(key_size);
        is.read(key_data.data(), key_size);
        
        uint64_t value;
        is.read(reinterpret_cast<char*>(&value), sizeof(value));
        
        Slice key(key_data.data(), key_size);
        insert(key, value);
    }
    
    return is.good();
}

void SkipListIndex::compress_levels() {
    // 简化实现：移除稀疏的层
    // 实际实现可以更复杂，比如合并相邻层
    for (int level = max_level_; level > 0; --level) {
        if (level_stats_[level] < 10) {
            // 移除这一层
            SkipListNode* current = head_;
            while (current) {
                SkipListNode* next = current->get_next(level);
                if (next) {
                    current->set_next(level, next->get_next(level));
                }
                current = next;
            }
            level_stats_[level] = 0;
        }
    }
}

bool SkipListIndex::should_optimize() const {
    if (!config_.enable_optimization) {
        return false;
    }
    
    size_t current_size = size_.load();
    if (current_size < config_.optimization_threshold) {
        return false;
    }
    
    // 检查是否有稀疏层
    for (size_t i = 1; i < level_stats_.size(); ++i) {
        if (level_stats_[i] < 10 && level_stats_[i] > 0) {
            return true;
        }
    }
    
    return false;
}

} // namespace index
} // namespace mementodb

