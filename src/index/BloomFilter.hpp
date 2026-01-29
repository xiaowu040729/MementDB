#ifndef BLOOM_FILTER_HPP
#define BLOOM_FILTER_HPP

#include "IIndex.hpp"
#include "../utils/Hash.hpp"
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <cmath>
#include <memory>
#include <fstream>
#include <bit>

namespace mementodb {
namespace index {

/**
 * 布隆过滤器配置
 */
struct BloomFilterConfig : public IndexConfig {
    size_t expected_element_count = 1000000;  // 预期元素数量
    double false_positive_rate = 0.01;        // 假阳性率（1%）
    size_t num_hash_functions = 0;            // 哈希函数数量（0表示自动计算）
    size_t bit_array_size = 0;                // 位数组大小（0表示自动计算）
    bool thread_safe = true;                  // 是否线程安全
    bool counting_bloom = false;              // 是否使用计数布隆过滤器（支持删除）
    size_t counting_bits = 4;                 // 计数位数（如果counting_bloom为true）
    bool enable_fast_hash = true;             // 是否使用快速哈希（双哈希模拟多哈希）
    
    BloomFilterConfig() {
        type = IndexType::BLOOM_FILTER;
        name = "BloomFilter_" + std::to_string(reinterpret_cast<uintptr_t>(this));
    }
    
    /**
     * 根据预期元素数量和假阳性率计算最优参数
     */
    void calculate_optimal_parameters() {
        if (bit_array_size == 0) {
            // m = -n * ln(p) / (ln(2)^2)
            bit_array_size = static_cast<size_t>(
                std::ceil(-expected_element_count * std::log(false_positive_rate) / 
                         (std::log(2.0) * std::log(2.0))));
            // 确保是8的倍数以便字节对齐
            bit_array_size = ((bit_array_size + 7) / 8) * 8;
        }
        
        if (num_hash_functions == 0) {
            // k = (m/n) * ln(2)
            num_hash_functions = static_cast<size_t>(
                std::ceil(static_cast<double>(bit_array_size) / 
                         expected_element_count * std::log(2.0)));
            
            // 限制哈希函数数量在合理范围内
            num_hash_functions = std::max<size_t>(1, std::min<size_t>(30, num_hash_functions));
        }
        
        if (counting_bloom && counting_bits < 2) {
            counting_bits = 4; // 默认4位计数器（最大计数15）
        }
    }
    
    std::unique_ptr<IndexConfig> clone() const override {
        auto clone = std::make_unique<BloomFilterConfig>(*this);
        return clone;
    }
    
    bool validate() const override {
        return IndexConfig::validate() &&
               expected_element_count > 0 &&
               false_positive_rate > 0.0 && false_positive_rate < 1.0 &&
               (bit_array_size == 0 || bit_array_size >= 8) &&
               (!counting_bloom || (counting_bits >= 2 && counting_bits <= 8));
    }
};

/**
 * 布隆过滤器实现
 * 
 * 特点：
 * - 空间效率高
 * - O(k) 时间复杂度（k为哈希函数数量）
 * - 可能存在假阳性，但不会有假阴性
 * - 标准版本不支持删除，计数版本支持
 * - 不支持获取值，仅支持存在性检查
 */
class BloomFilter : public IIndex {
public:
    /**
     * 构造函数
     * @param config 配置
     */
    explicit BloomFilter(const BloomFilterConfig& config);
    
    /**
     * 析构函数
     */
    ~BloomFilter() override = default;
    
    // 禁用拷贝，允许移动
    BloomFilter(const BloomFilter&) = delete;
    BloomFilter& operator=(const BloomFilter&) = delete;
    BloomFilter(BloomFilter&&) = default;
    BloomFilter& operator=(BloomFilter&&) = default;
    
    // IIndex 接口实现
    IndexType get_type() const override { return IndexType::BLOOM_FILTER; }
    std::string get_name() const override { return config_.name; }
    
    /**
     * 插入键（值被忽略，因为布隆过滤器只存储存在性）
     * @return 总是返回true，因为布隆过滤器总可以"插入"
     */
    bool insert(const Slice& key, uint64_t value) override;
    
    /**
     * 不支持获取值
     */
    std::optional<uint64_t> get(const Slice& key) const override {
        return std::nullopt;
    }
    
    /**
     * 删除键（仅计数布隆过滤器支持）
     */
    bool remove(const Slice& key) override;
    
    /**
     * 检查键是否存在（可能返回假阳性）
     */
    bool contains(const Slice& key) const override;
    
    void clear() override;
    size_t size() const override;
    bool empty() const override;
    
    IndexStatistics get_statistics() const override;
    void reset_statistics() override;
    
    bool persist(const std::string& path = "") const override;
    bool load(const std::string& path) override;
    
    /**
     * 批量插入优化版本
     */
    std::pair<size_t, std::vector<std::pair<Slice, uint64_t>>> 
    batch_insert(const std::vector<std::pair<Slice, uint64_t>>& entries) override;
    
    /**
     * 获取实际假阳性率（基于当前元素数量）
     */
    double get_actual_false_positive_rate() const;
    
    /**
     * 获取位数组使用率
     */
    double get_bit_usage_ratio() const;
    
    /**
     * 获取配置
     */
    std::unique_ptr<IndexConfig> get_config() const override {
        return std::make_unique<BloomFilterConfig>(config_);
    }
    
    /**
     * 获取哈希函数数量
     */
    size_t get_num_hash_functions() const { return num_hash_functions_; }
    
    /**
     * 获取位数组大小（位数）
     */
    size_t get_bit_array_size() const { return bit_array_size_; }
    
    /**
     * 获取估计元素数量（基于位使用率）
     */
    size_t get_estimated_element_count() const;
    
    /**
     * 合并两个布隆过滤器（必须相同配置）
     */
    bool merge(const BloomFilter& other);
    
    /**
     * 检查是否支持删除
     */
    bool supports_deletion() const { return config_.counting_bloom; }
    
    // 不支持范围查询和前缀查询
    bool supports_range_query() const override { return false; }
    bool supports_prefix_query() const override { return false; }
    
private:
    BloomFilterConfig config_;
    
    // 标准布隆过滤器使用字节数组，避免vector<bool>的问题
    std::vector<uint8_t> bit_array_;
    size_t bit_array_size_;         // 位数组大小（位数）
    size_t byte_array_size_;        // 字节数组大小
    
    // 计数布隆过滤器使用单独的计数器数组
    std::vector<uint8_t> count_array_; // 每个计数器占用config_.counting_bits位
    
    size_t num_hash_functions_;     // 哈希函数数量
    std::atomic<size_t> element_count_{0};  // 估计的元素数量（非精确）
    
    // 哈希种子（用于双哈希模拟多哈希）
    std::vector<uint64_t> hash_seeds_;
    
    // 线程安全
    mutable std::shared_mutex mutex_;
    
    // 统计信息（使用原子操作保证线程安全）
    struct Statistics {
        std::atomic<uint64_t> insert_count{0};
        std::atomic<uint64_t> lookup_count{0};
        std::atomic<uint64_t> false_positive_count{0};
        std::atomic<uint64_t> delete_count{0};
        std::atomic<uint64_t> actual_inserts{0};  // 实际设置的位数增加
    };
    
    mutable Statistics stats_;
    
    // 辅助方法
    void init_hash_seeds();
    
    // 位操作
    void set_bit(size_t bit_index);
    void clear_bit(size_t bit_index);
    bool test_bit(size_t bit_index) const;
    
    // 计数操作（仅用于计数布隆过滤器）
    void increment_counter(size_t counter_index);
    void decrement_counter(size_t counter_index);
    uint8_t get_counter(size_t counter_index) const;
    
    // 哈希函数（双哈希法模拟k个哈希）
    std::vector<size_t> compute_hash_positions(const Slice& key) const;
    
    // 统计更新（线程安全）
    void record_insert() const;
    void record_lookup(bool is_false_positive) const;
    void record_delete() const;
    
    // 内部持久化/加载辅助函数
    bool save_to_file(const std::string& path) const;
    bool load_from_file(const std::string& path);
    
    // 计算双哈希
    std::pair<uint64_t, uint64_t> double_hash(const Slice& key) const;
};

} // namespace index
} // namespace mementodb

#endif // BLOOM_FILTER_HPP
