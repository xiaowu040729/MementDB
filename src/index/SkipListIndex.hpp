#ifndef SKIP_LIST_INDEX_HPP
#define SKIP_LIST_INDEX_HPP

#include "IIndex.hpp"
#include "mementodb/Types.h"
#include <memory>
#include <random>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <vector>
#include <string>
#include <functional>
#include <fstream>
#include <stdexcept>

namespace mementodb {
namespace index {

/**
 * 跳表索引配置
 */
struct SkipListIndexConfig : public IndexConfig {
    int max_level = 16;              // 最大层数
    double probability = 0.5;       // 节点提升概率
    bool thread_safe = true;         // 是否线程安全
    bool allow_duplicates = false;   // 是否允许重复键（如果允许，后插入的覆盖）
    bool enable_node_pool = true;    // 是否启用节点池
    size_t node_pool_size = 1000;   // 节点池大小
    bool enable_optimization = false; // 是否启用自动优化
    size_t optimization_threshold = 10000; // 优化阈值
    
    SkipListIndexConfig() {
        type = IndexType::SKIP_LIST;
    }
    
    // 重写 clone 方法
    std::unique_ptr<IndexConfig> clone() const override {
        auto cloned = std::make_unique<SkipListIndexConfig>(*this);
        return cloned;
    }
    
    // 配置验证
    bool validate() const override {
        return IndexConfig::validate() &&
               max_level > 0 && max_level <= 64 &&
               probability > 0.0 && probability < 1.0 &&
               node_pool_size > 0;
    }
};

/**
 * 跳表节点（改进版：直接存储字符串键）
 */
class SkipListNode {
public:
    SkipListNode(const std::string& key, uint64_t value, int level);
    ~SkipListNode();
    
    // 禁止拷贝
    SkipListNode(const SkipListNode&) = delete;
    SkipListNode& operator=(const SkipListNode&) = delete;
    
    // 允许移动
    SkipListNode(SkipListNode&&) = default;
    SkipListNode& operator=(SkipListNode&&) = default;
    
    const std::string& get_key() const { return key_; }
    Slice get_key_slice() const { return Slice(key_); }
    uint64_t get_value() const { return value_; }
    void set_value(uint64_t value) { value_ = value; }
    
    int get_level() const { return level_; }
    SkipListNode* get_next(int level) const { return forward_[level]; }
    void set_next(int level, SkipListNode* node) { forward_[level] = node; }
    
private:
    std::string key_;                      // 键（直接存储字符串）
    uint64_t value_;                       // 值
    int level_;                            // 节点层数
    std::vector<SkipListNode*> forward_;   // 每层的下一个节点指针
};

/**
 * 跳表迭代器
 */
class SkipListIterator {
public:
    explicit SkipListIterator(SkipListNode* node) : current_(node) {}
    
    bool valid() const { return current_ != nullptr; }
    Slice key() const { 
        return current_ ? current_->get_key_slice() : Slice();
    }
    uint64_t value() const { 
        return current_ ? current_->get_value() : 0;
    }
    void next() {
        if (current_) {
            current_ = current_->get_next(0);
        }
    }
    
    SkipListIterator& operator++() {
        next();
        return *this;
    }
    
    // 获取当前节点（用于内部访问）
    SkipListNode* get_current() const { return current_; }
    
    bool operator==(const SkipListIterator& other) const {
        return current_ == other.current_;
    }
    
    bool operator!=(const SkipListIterator& other) const {
        return !(*this == other);
    }
    
private:
    SkipListNode* current_;
};

/**
 * 跳表索引实现（优化版）
 * 
 * 特点：
 * - O(log n) 平均时间复杂度查找
 * - 支持范围查询
 * - 支持前缀查询
 * - 内存索引，查询速度快
 * - 线程安全（可选）
 * - 节点池优化
 * - 自动优化功能
 * 
 * 适用场景：
 * - 内存索引
 * - 需要范围查询的场景
 * - 需要排序的场景
 */
class SkipListIndex : public IIndex {
public:
    /**
     * 构造函数（带配置验证）
     * @param config 配置
     * @throws std::invalid_argument 配置无效
     */
    explicit SkipListIndex(const SkipListIndexConfig& config);
    
    /**
     * 析构函数
     */
    ~SkipListIndex() override;
    
    // 禁用拷贝，允许移动
    SkipListIndex(const SkipListIndex&) = delete;
    SkipListIndex& operator=(const SkipListIndex&) = delete;
    SkipListIndex(SkipListIndex&&) = default;
    SkipListIndex& operator=(SkipListIndex&&) = default;
    
    // IIndex 接口实现
    IndexType get_type() const override { return IndexType::SKIP_LIST; }
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
    
    // 支持 insert_or_update
    std::optional<uint64_t> insert_or_update(const Slice& key, uint64_t value) override;
    
    // 范围查询支持
    RangeResult range_query(const Slice& start_key, 
                           const Slice& end_key, 
                           size_t limit = 0) const override;
    
    // 前缀查询支持
    RangeResult prefix_query(const Slice& prefix, 
                            size_t limit = 0) const override;
    
    // 支持范围查询和前缀查询
    bool supports_range_query() const override { return true; }
    bool supports_prefix_query() const override { return true; }
    
    // 获取配置
    std::unique_ptr<IndexConfig> get_config() const override;
    
    /**
     * 获取最小键
     */
    std::optional<std::pair<Slice, uint64_t>> min() const;
    
    /**
     * 获取最大键
     */
    std::optional<std::pair<Slice, uint64_t>> max() const;
    
    /**
     * 批量插入（优化版本）
     */
    std::pair<size_t, std::vector<std::pair<Slice, uint64_t>>> 
    batch_insert(const std::vector<std::pair<Slice, uint64_t>>& entries) override;
    
    /**
     * 批量删除（优化版本）
     */
    std::pair<size_t, std::vector<Slice>> 
    batch_remove(const std::vector<Slice>& keys) override;
    
    /**
     * 内存使用报告
     */
    size_t memory_usage() const;
    
    /**
     * 优化跳表（压缩稀疏层）
     */
    void optimize();
    
    /**
     * 创建快照
     */
    bool create_snapshot(const std::string& path) const;
    
    /**
     * 从快照恢复
     */
    bool restore_from_snapshot(const std::string& path);
    
    /**
     * 设置性能监控回调
     */
    using MonitorCallback = std::function<void(const std::string&, size_t)>;
    void set_monitor_callback(MonitorCallback callback);
    
    // 迭代器支持
    SkipListIterator begin() const;
    SkipListIterator lower_bound(const Slice& key) const;
    SkipListIterator upper_bound(const Slice& key) const;
    SkipListIterator end() const { return SkipListIterator(nullptr); }
    
    // 用于测试的内部状态检查
    #ifdef TEST_BUILD
    bool verify_integrity() const;
    int get_max_level() const { return max_level_; }
    SkipListNode* get_head() const { return head_; }
    size_t get_level_count(int level) const;
    #endif
    
private:
    SkipListIndexConfig config_;
    
    // 跳表结构
    SkipListNode* head_;              // 头节点
    int max_level_;                   // 当前最大层数
    std::atomic<size_t> size_;        // 节点数量
    
    // 使用自定义分配器和节点池
    using NodeAllocator = std::allocator<SkipListNode>;
    NodeAllocator node_allocator_;
    
    // 预分配节点池
    std::vector<SkipListNode*> free_nodes_;
    std::mutex pool_mutex_;  // 节点池互斥锁
    
    // 层级统计（使用普通 size_t，并通过 mutex_ 保护）
    mutable std::vector<size_t> level_stats_;
    
    // 性能监控回调
    MonitorCallback monitor_callback_;
    
    // 线程安全的随机数生成器（使用thread_local）
    // 注意：rng_ 和 dist_ 已移除，改用 thread_local
    
    // 线程安全
    mutable std::shared_mutex mutex_;  // 读写锁
    
    // 统计信息（增强版）
    struct Statistics {
        std::atomic<uint64_t> insert_count{0};
        std::atomic<uint64_t> lookup_count{0};
        std::atomic<uint64_t> delete_count{0};
        std::atomic<uint64_t> hit_count{0};
        std::atomic<uint64_t> miss_count{0};
        std::atomic<uint64_t> memory_usage_{0};
        std::atomic<uint64_t> node_allocations_{0};
        std::atomic<uint64_t> node_deallocations_{0};
        std::atomic<uint64_t> optimization_count{0};
    };
    
    mutable Statistics stats_;
    
    // 辅助方法
    int random_level() const;  // 线程安全的随机层数生成
    SkipListNode* find_node(const Slice& key) const;
    void find_predecessors(const Slice& key, 
                          std::vector<SkipListNode*>& update) const;
    void delete_node(SkipListNode* node, 
                    const std::vector<SkipListNode*>& update);
    void clear_helper();
    
    // 节点池管理
    SkipListNode* allocate_node(const Slice& key, uint64_t value, int level);
    void deallocate_node(SkipListNode* node);
    void init_node_pool();
    void cleanup_node_pool();
    
    // 内存管理
    void update_memory_stats();
    
    // 持久化辅助
    bool save_to_stream(std::ostream& os) const;
    bool load_from_stream(std::istream& is);
    
    // 内部优化方法
    void compress_levels();
    bool should_optimize() const;
};

} // namespace index
} // namespace mementodb

#endif // SKIP_LIST_INDEX_HPP
