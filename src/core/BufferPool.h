#ifndef BUFFER_POOL_H
#define BUFFER_POOL_H

#include "Page.hpp"
#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>
#include <unordered_map>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <queue>
#include <deque>
#include <list>
#include <set>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <fstream>
#include <sstream>
#include <system_error>
#include <tuple>
#include <variant>
#include <random>
#include <cstring>
#include <future>
#include <condition_variable>
#include <string>

// 简单时间戳工具，避免前置声明依赖
inline uint64_t bufferpool_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 性能监控宏
#ifdef BUFFER_POOL_ENABLE_PERF_COUNTERS
#define BUFFER_POOL_PERF_COUNTER(name) \
    do { \
        if (performance_counters_) { \
            performance_counters_->name++; \
        } \
    } while (0)
#else
#define BUFFER_POOL_PERF_COUNTER(name)
#endif

namespace mementodb {
namespace core {

// 前向声明
class FileManager;
class FileHandle;
class PageAllocator;

// ==================== 替换策略接口（增强版） ====================

// ==================== 缓冲池配置 ====================

/**
 * @brief 缓冲池配置
 */
// Page 的别名，兼容 DiskPage 语义
using DiskPage = Page;

// ==================== 缓冲池统计信息 ====================

/**
 * @brief 缓冲池统计信息
 * @details 基础统计信息结构体，用于增强版缓冲池继承
 */
struct BufferPoolStatistics {
    size_t total_frames = 0;  // 总帧数
    size_t used_frames = 0;  // 使用帧数
    size_t free_frames = 0;  // 空闲帧数
    size_t dirty_frames = 0;  // 脏帧数
    size_t pinned_frames = 0;  // 固定帧数
    uint64_t hit_count = 0;  // 命中次数
    uint64_t miss_count = 0;  // 未命中次数
    double hit_ratio = 0.0;  // 命中率
    uint64_t evict_count = 0; // 驱逐次数
    uint64_t flush_count = 0;  // 刷盘次数
    uint64_t prefetch_count = 0;  // 预取次数
    std::string replacer_name;  // 替换器名称
};

} // namespace core
} // namespace mementodb

// ==================== 增强版接口 ====================
// 以下为增强版BufferPool接口，提供高级功能和优化

namespace mementodb {
namespace core {

// ==================== 性能计数器 ====================

/**
 * @brief 缓冲池性能计数器
 * @details 记录各种操作的性能统计，用于调优和分析
 */
struct BufferPoolPerformanceCounters {
    uint64_t fetch_page_calls = 0; // 获取页调用次数
    uint64_t fetch_page_hits = 0; // 获取页命中次数
    uint64_t fetch_page_misses = 0; // 获取页未命中次数
    uint64_t new_page_calls = 0; // 创建页调用次数
    uint64_t unpin_page_calls = 0; // 释放页调用次数
    uint64_t flush_page_calls = 0; // 刷盘页调用次数
    uint64_t evict_page_calls = 0; // 驱逐页调用次数
    uint64_t prefetch_calls = 0; // 预取页调用次数
    uint64_t prefetch_hits = 0; // 预取页命中次数
    uint64_t lock_contention_count = 0; // 锁竞争次数
    uint64_t lock_wait_time_ns = 0; // 锁等待时间
    uint64_t io_read_bytes = 0; // 读取字节数
    uint64_t io_write_bytes = 0; // 写入字节数
    uint64_t io_read_time_ns = 0; // 读取时间   
    uint64_t io_write_time_ns = 0; // 写入时间
    
    // 操作延迟统计
    struct LatencyStats {
        uint64_t count = 0; // 延迟次数
        uint64_t total_ns = 0; // 延迟总时间
        uint64_t min_ns = UINT64_MAX; // 最小延迟时间
        uint64_t max_ns = 0; // 最大延迟时间
        
        void record(uint64_t latency_ns); // 记录延迟时间
        double average_ns() const; // 计算平均延迟时间
        void reset(); // 重置延迟统计
    };
    
    LatencyStats fetch_latency; // 获取页延迟统计
    LatencyStats flush_latency; // 刷盘延迟统计
    LatencyStats evict_latency; // 驱逐页延迟统计
    
    // 计算命中率
    double hit_ratio() const; // 计算命中率
    
    // 计算平均IO延迟
    double avg_read_latency_ns() const;
    // 计算平均写入延迟时间
    double avg_write_latency_ns() const;
    
    // 重置所有计数器
    void reset();
    
    // 转换为字符串
    std::string to_string() const;
    
    // 导出为JSON
    std::string to_json() const;
};


/**
 * @brief 替换策略配置
 */
struct ReplacerConfig {
    enum class Type {
        LRU,            // 最近最少使用
        CLOCK,          // 时钟算法
        ARC,            // 自适应替换缓存（Adaptive Replacement Cache）
        LIRS,           // Low Inter-reference Recency Set
        TWO_QUEUE,      // 2Q算法
        MQ,             // Multi-Queue
        ADAPTIVE,       // 自适应选择
        HYBRID          // 混合策略
    } type = Type::LRU;
    
    // LRU特定配置
    struct LRUConfig {
        bool enable_scan_resistance = true;  // 防扫描保护
        size_t hot_threshold = 10;           // 热页面阈值
    } lru_config;
    
    // Clock特定配置
    struct ClockConfig {
        size_t second_chance = 2;            // 二次机会次数
        bool enable_adaptive_hand = true;    // 自适应时钟指针
    } clock_config;
    
    // ARC特定配置
    struct ARCConfig {
        size_t adaptive_p = 0;               // 自适应参数
        bool enable_dynamic_p = true;        // 动态调整p
    } arc_config;
    
    // LIRS特定配置
    struct LIRSConfig {
        size_t lir_size_ratio = 20;          // LIR集合大小比例（百分比）
        size_t hir_size_ratio = 80;          // HIR集合大小比例
        size_t stack_size_multiplier = 2;    // 栈大小倍数
    } lirs_config;
    
    // 2Q特定配置
    struct TwoQueueConfig {
        size_t kin_ratio = 25;               // Kin队列大小比例
        size_t kout_ratio = 50;              // Kout队列大小比例
        bool enable_fifo_second_chance = true;
    } two_queue_config;
    
    // 通用配置
    bool enable_access_frequency = true;     // 启用访问频率统计
    bool enable_access_recency = true;       // 启用访问最近性统计
    bool enable_adaptive_behavior = false;   // 启用自适应行为
    size_t monitor_window_size = 1000;       // 监控窗口大小
    std::chrono::seconds adapt_interval{60}; // 自适应调整间隔
    
    static ReplacerConfig default_config();
    std::string type_to_string() const;
};

/**
 * @brief 增强的替换策略接口
 */
/**
 * @brief 增强的替换策略接口
 * @details 合并了基础接口和增强功能，提供完整的替换策略接口
 */
class IAdvancedReplacer {
public:
    virtual ~IAdvancedReplacer() = default;
    
    // ========== 基础功能（来自原 IReplacer） ==========
    
    /**
     * @brief 记录页面访问
     * @param frame_id 帧ID
     */
    virtual void record_access(size_t frame_id) = 0;
    
    /**
     * @brief 固定页面（防止被替换）
     * @param frame_id 帧ID
     */
    virtual void pin(size_t frame_id) = 0;
    
    /**
     * @brief 取消固定页面（允许被替换）
     * @param frame_id 帧ID
     * @return 如果页面可以被替换返回true
     */
    virtual bool unpin(size_t frame_id) = 0;
    
    /**
     * @brief 选择要替换的页面
     * @return 帧ID，如果没有可替换的页面返回std::nullopt
     */
    virtual std::optional<size_t> select_victim() = 0;
    
    /**
     * @brief 移除页面（页面被驱逐时调用）
     * @param frame_id 帧ID
     */
    virtual void remove(size_t frame_id) = 0;
    
    /**
     * @brief 获取可替换的页面数量
     */
    virtual size_t get_replacable_count() const = 0;
    
    /**
     * @brief 获取总页面数量
     */
    virtual size_t get_total_count() const = 0;
    
    /**
     * @brief 重置替换器
     */
    virtual void reset() = 0;
    
    /**
     * @brief 获取替换器名称
     */
    virtual std::string get_name() const = 0;
    
    // ========== 增强功能 ==========
    
    /**
     * @brief 批量记录访问
     */
    virtual void record_access_batch(const std::vector<size_t>& frame_ids) = 0;
    
    /**
     * @brief 批量固定页面
     */
    virtual void pin_batch(const std::vector<size_t>& frame_ids) = 0;
    
    /**
     * @brief 批量取消固定
     */
    virtual void unpin_batch(const std::vector<size_t>& frame_ids) = 0;
    
    /**
     * @brief 批量选择受害者
     */
    virtual std::vector<size_t> select_victims(size_t count) = 0;
    
    /**
     * @brief 获取页面热度评分
     */
    virtual double get_heat_score(size_t frame_id) const = 0;
    
    /**
     * @brief 获取页面冷度评分（越低越容易被替换）
     */
    virtual double get_cold_score(size_t frame_id) const = 0;
    
    /**
     * @brief 调整替换策略参数
     */
    virtual void tune(const std::string& param, double value) = 0;
    
    /**
     * @brief 获取策略统计信息
     */
    struct ReplacerStats {
        size_t total_accesses = 0;
        size_t hits = 0;
        size_t compulsory_misses = 0;    // 强制性缺失
        size_t capacity_misses = 0;      // 容量缺失
        size_t conflict_misses = 0;      // 冲突缺失
        double hit_ratio = 0.0;
        std::chrono::milliseconds total_decision_time{0};
        std::map<std::string, double> custom_metrics;
    };
    
    virtual ReplacerStats get_stats() const = 0;
    
    /**
     * @brief 导出策略状态（用于持久化）
     */
    virtual std::string export_state() const = 0;
    
    /**
     * @brief 导入策略状态（用于恢复）
     */
    virtual bool import_state(const std::string& state) = 0;
};

// ==================== LRU替换策略（增强版） ====================

/**
 * @brief LRU（最近最少使用）替换策略（增强版）
 * @details 使用双向链表实现O(1)的访问和替换操作，支持批量操作和统计
 */
class LRUReplacer : public IAdvancedReplacer {
public:
    explicit LRUReplacer(size_t num_pages);
    ~LRUReplacer() override = default;
    
    // 基础功能
    void record_access(size_t frame_id) override;
    void pin(size_t frame_id) override;
    bool unpin(size_t frame_id) override;
    std::optional<size_t> select_victim() override;
    void remove(size_t frame_id) override;
    size_t get_replacable_count() const override;
    size_t get_total_count() const override;
    void reset() override;
    std::string get_name() const override { return "LRU"; }
    
    // 增强功能
    void record_access_batch(const std::vector<size_t>& frame_ids) override;
    void pin_batch(const std::vector<size_t>& frame_ids) override;
    void unpin_batch(const std::vector<size_t>& frame_ids) override;
    std::vector<size_t> select_victims(size_t count) override;
    double get_heat_score(size_t frame_id) const override;
    double get_cold_score(size_t frame_id) const override;
    void tune(const std::string& param, double value) override;
    ReplacerStats get_stats() const override;
    std::string export_state() const override;
    bool import_state(const std::string& state) override;
    
private:
    // 双向链表节点
    struct Node {
        size_t frame_id;
        Node* prev;
        Node* next;
        
        Node(size_t id) : frame_id(id), prev(nullptr), next(nullptr) {}
    };
    
    size_t num_pages_;
    mutable std::mutex mutex_;
    
    // 双向链表（最近使用的在头部，最少使用的在尾部）
    Node* head_;
    Node* tail_;
    
    // 帧ID到节点的映射
    std::unordered_map<size_t, std::unique_ptr<Node>> node_map_;
    
    // 固定状态（true表示被固定，不能替换）
    std::vector<bool> pinned_;
    
    // 统计信息
    mutable std::mutex stats_mutex_;
    ReplacerStats stats_;
    
    // 辅助方法
    void add_to_head(Node* node);
    void remove_node(Node* node);
    void move_to_head(Node* node);
    void update_stats(bool hit);
};

// ==================== Clock替换策略（增强版） ====================

/**
 * @brief Clock（时钟）替换策略（增强版）
 * @details 使用时钟算法，每个页面有一个引用位，扫描时检查引用位，支持批量操作和统计
 */
class ClockReplacer : public IAdvancedReplacer {
public:
    explicit ClockReplacer(size_t num_pages);
    ~ClockReplacer() override = default;
    
    // 基础功能
    void record_access(size_t frame_id) override;
    void pin(size_t frame_id) override;
    bool unpin(size_t frame_id) override;
    std::optional<size_t> select_victim() override;
    void remove(size_t frame_id) override;
    size_t get_replacable_count() const override;
    size_t get_total_count() const override;
    void reset() override;
    std::string get_name() const override { return "Clock"; }
    
    // 增强功能
    void record_access_batch(const std::vector<size_t>& frame_ids) override;
    void pin_batch(const std::vector<size_t>& frame_ids) override;
    void unpin_batch(const std::vector<size_t>& frame_ids) override;
    std::vector<size_t> select_victims(size_t count) override;
    double get_heat_score(size_t frame_id) const override;
    double get_cold_score(size_t frame_id) const override;
    void tune(const std::string& param, double value) override;
    ReplacerStats get_stats() const override;
    std::string export_state() const override;
    bool import_state(const std::string& state) override;
    
private:
    size_t num_pages_;
    mutable std::mutex mutex_;
    
    // 时钟指针
    size_t clock_hand_;
    
    // 引用位（true表示最近被访问）
    std::vector<std::atomic<bool>> ref_bits_;
    
    // 固定状态
    std::vector<std::atomic<bool>> pinned_;
    
    // 是否在替换器中（unpin后加入，pin后移除）
    std::vector<std::atomic<bool>> in_replacer_;
    
    // 统计信息
    mutable std::mutex stats_mutex_;
    ReplacerStats stats_;
    
    // 辅助方法
    void update_stats(bool hit);
};

// ==================== ARC替换策略 ====================

/**
 * @brief ARC（Adaptive Replacement Cache）替换策略
 * @details 自适应替换缓存，结合了LRU和LFU的优点
 */
class ARCReplacer : public IAdvancedReplacer {
public:
    explicit ARCReplacer(size_t num_pages, const ReplacerConfig::ARCConfig& config = {});
    ~ARCReplacer() override = default;
    
    void record_access(size_t frame_id) override;
    void record_access_batch(const std::vector<size_t>& frame_ids) override;
    void pin(size_t frame_id) override;
    void pin_batch(const std::vector<size_t>& frame_ids) override;
    bool unpin(size_t frame_id) override;
    void unpin_batch(const std::vector<size_t>& frame_ids) override;
    std::optional<size_t> select_victim() override;
    std::vector<size_t> select_victims(size_t count) override;
    void remove(size_t frame_id) override;
    size_t get_replacable_count() const override;
    size_t get_total_count() const override;
    void reset() override;
    std::string get_name() const override { return "ARC"; }
    
    double get_heat_score(size_t frame_id) const override;
    double get_cold_score(size_t frame_id) const override;
    void tune(const std::string& param, double value) override;
    ReplacerStats get_stats() const override;
    std::string export_state() const override;
    bool import_state(const std::string& state) override;
    
private:
    mutable std::shared_mutex mutex_;
    size_t capacity_;
    ReplacerConfig::ARCConfig config_;
    
    // ARC算法的四个列表
    std::list<size_t> t1_;  // 最近访问的页面
    std::list<size_t> t2_;  // 频繁访问的页面
    std::list<size_t> b1_;  // 最近被替换的页面（幽灵列表）
    std::list<size_t> b2_;  // 频繁访问后被替换的页面（幽灵列表）
    
    // 映射表：frame_id -> 迭代器
    std::unordered_map<size_t, std::list<size_t>::iterator> t1_map_;
    std::unordered_map<size_t, std::list<size_t>::iterator> t2_map_;
    std::unordered_map<size_t, std::list<size_t>::iterator> b1_map_;
    std::unordered_map<size_t, std::list<size_t>::iterator> b2_map_;
    
    // 自适应参数p
    size_t p_ = 0;
    
    // 固定状态
    std::vector<std::atomic<bool>> pinned_;
    
    // 统计信息
    mutable std::mutex stats_mutex_;
    ReplacerStats stats_;
    
    // 辅助方法
    bool is_in_t1(size_t frame_id) const;
    bool is_in_t2(size_t frame_id) const;
    bool is_in_b1(size_t frame_id) const;
    bool is_in_b2(size_t frame_id) const;
    
    void move_to_t2(size_t frame_id);
    void replace(bool in_b2);
    size_t choose_victim_from_t1() const;
    size_t choose_victim_from_t2() const;
    
    void update_stats(bool hit);
};

// ==================== 自适应替换策略 ====================

/**
 * @brief 自适应替换策略
 * @details 根据访问模式动态选择最佳替换策略
 */
class AdaptiveReplacer : public IAdvancedReplacer {
public:
    AdaptiveReplacer(size_t num_pages, const ReplacerConfig& config);
    ~AdaptiveReplacer();
    
    // 实现IAdvancedReplacer接口...
    void record_access(size_t frame_id) override;
    void record_access_batch(const std::vector<size_t>& frame_ids) override;
    void pin(size_t frame_id) override;
    void pin_batch(const std::vector<size_t>& frame_ids) override;
    bool unpin(size_t frame_id) override;
    void unpin_batch(const std::vector<size_t>& frame_ids) override;
    std::optional<size_t> select_victim() override;
    std::vector<size_t> select_victims(size_t count) override;
    void remove(size_t frame_id) override;
    size_t get_replacable_count() const override;
    size_t get_total_count() const override;
    void reset() override;
    std::string get_name() const override { return "Adaptive"; }
    
    double get_heat_score(size_t frame_id) const override;
    double get_cold_score(size_t frame_id) const override;
    void tune(const std::string& param, double value) override;
    ReplacerStats get_stats() const override;
    std::string export_state() const override;
    bool import_state(const std::string& state) override;
    
private:
    std::unique_ptr<IAdvancedReplacer> current_replacer_;
    ReplacerConfig config_;
    
    // 访问模式分析器
    class AccessPatternAnalyzer {
    public:
        enum class Pattern {
            RANDOM,         // 随机访问
            SEQUENTIAL,     // 顺序访问
            LOOPING,        // 循环访问
            SCAN,           // 扫描访问
            HOT_COLD,       // 热冷分区
            UNKNOWN         // 未知模式
        };
        
        AccessPatternAnalyzer(size_t window_size = 1000);
        
        void record_access(size_t frame_id, uint64_t timestamp_ns);
        Pattern analyze() const;
        
        struct AnalysisResult {
            Pattern pattern;
            double confidence;
            std::map<std::string, double> metrics;
            std::string suggested_replacer;
        };
        
        AnalysisResult get_analysis() const;
        
    private:
        size_t window_size_;
        std::deque<std::pair<size_t, uint64_t>> access_history_;
        mutable std::mutex mutex_;
        
        // 分析指标
        double calculate_locality_score() const;
        double calculate_sequential_score() const;
        double calculate_loop_score() const;
        double calculate_hot_cold_ratio() const;
    };
    
    std::unique_ptr<AccessPatternAnalyzer> analyzer_;
    std::chrono::steady_clock::time_point last_adapt_time_;
    
    // 自适应调整
    void adapt_to_pattern();
    std::unique_ptr<IAdvancedReplacer> create_replacer_for_pattern(
        AccessPatternAnalyzer::Pattern pattern);
};

// ==================== 缓冲池配置增强 ====================

/**
 * @brief 增强的缓冲池配置
 */
/**
 * @brief 增强的缓冲池配置
 * @details 合并了基础配置和增强功能配置
 */
struct EnhancedBufferPoolConfig {
    // ========== 基础配置（来自原 BufferPoolConfig） ==========
    
    size_t pool_size = 1024;                    // 缓冲池大小（页数）
    size_t page_size = kPageSize;              // 页大小（默认4KB）
    
    enum class ReplacerType {
        LRU,                                    // LRU算法
        CLOCK,                                  // Clock算法
        ADAPTIVE                                // 自适应（根据访问模式选择）
    } replacer_type = ReplacerType::LRU;
    
    enum class FlushStrategy {
        LAZY,                                   // 惰性刷盘
        PERIODIC,                               // 定期刷盘
        WRITE_THROUGH,                          // 直写
        WRITE_BACK                              // 回写
    } flush_strategy = FlushStrategy::WRITE_BACK;
    
    std::chrono::milliseconds flush_interval{1000};  // 刷盘间隔
    size_t dirty_page_threshold = 100;         // 脏页阈值
    
    bool enable_prefetch = true;               // 启用预取
    size_t prefetch_size = 4;                  // 预取页数
    
    bool enable_statistics = true;             // 启用统计
    std::chrono::seconds stats_interval{60};   // 统计间隔
    
    // ========== 增强配置 ==========
    
    // 动态调整配置
    struct DynamicConfig {
        bool enabled = false;                       // 启用动态调整
        size_t min_pool_size = 64;                  // 最小缓冲池大小
        size_t max_pool_size = 65536;               // 最大缓冲池大小
        std::chrono::seconds adjust_interval{300};  // 调整间隔
        double target_hit_ratio = 0.95;             // 目标命中率
        size_t adjust_step = 64;                    // 调整步长
    } dynamic_config;
    
    // 预取配置
    struct PrefetchConfig {
        bool enabled = true;
        enum class Strategy {
            SEQUENTIAL,           // 顺序预取
            STRIDE,               // 步长预取
            MARKOV,               // 马尔可夫链预测
            MACHINE_LEARNING,     // 机器学习预测
            HYBRID                // 混合策略
        } strategy = Strategy::SEQUENTIAL;
        
        size_t lookahead = 4;                      // 预取向前看页数
        size_t max_prefetch_queue = 100;           // 最大预取队列
        bool adaptive_lookahead = true;            // 自适应向前看
        size_t max_concurrent_prefetch = 4;        // 最大并发预取数
    } prefetch_config;
    
    // 写优化配置
    struct WriteOptimizationConfig {
        bool enable_write_combining = true;        // 启用写合并
        size_t combine_window_ms = 100;            // 合并窗口（毫秒）
        size_t max_combine_size = 16;              // 最大合并页数
        bool enable_group_commit = true;           // 启用组提交
        size_t group_commit_threshold = 8;         // 组提交阈值
        std::chrono::milliseconds group_commit_timeout{50}; // 组提交超时
    } write_optimization_config;
    
    // 内存管理配置
    struct MemoryManagementConfig {
        bool enable_huge_pages = false;            // 启用大页
        bool enable_numa_aware = false;            // NUMA感知
        int numa_node = -1;                         // NUMA节点（-1表示所有）
        bool enable_page_coloring = false;         // 页着色优化
        size_t alignment = 64;                      // 内存对齐
    } memory_management_config;
    
    // 监控配置
    struct MonitoringConfig {
        bool enable_performance_counters = true;    // 启用性能计数器
        bool enable_access_tracing = false;         // 启用访问跟踪
        size_t trace_buffer_size = 10000;           // 跟踪缓冲区大小
        std::chrono::seconds stats_dump_interval{300}; // 统计转储间隔
        std::string stats_dump_path;                // 统计转储路径
    } monitoring_config;
    
    // 恢复配置
    struct RecoveryConfig {
        bool enable_checkpoint = true;              // 启用检查点
        std::chrono::seconds checkpoint_interval{60}; // 检查点间隔
        std::string checkpoint_dir;                 // 检查点目录
        bool enable_wal = false;                    // 启用写前日志
        std::string wal_dir;                        // WAL目录
        size_t wal_buffer_size = 16384;             // WAL缓冲区大小
    } recovery_config;
    
    static EnhancedBufferPoolConfig default_config();
    bool validate() const;
};

// ==================== 预取预测器接口 ====================

/**
 * @brief 预取预测器接口
 */
class IPrefetchPredictor {
public:
    virtual ~IPrefetchPredictor() = default;
    
    virtual void record_access(uint64_t page_id, uint64_t timestamp_ns) = 0;
    virtual std::vector<uint64_t> predict_next_pages(uint64_t current_page_id, size_t max_count) = 0;
    virtual void train(const std::vector<std::vector<uint64_t>>& access_sequences) = 0;
    virtual std::string get_name() const = 0;
    virtual std::string export_model() const = 0;
    virtual bool import_model(const std::string& model_data) = 0;
};

/**
 * @brief 顺序预取预测器
 */
class SequentialPrefetchPredictor : public IPrefetchPredictor {
public:
    SequentialPrefetchPredictor(size_t lookahead = 4, bool adaptive = true);
    
    void record_access(uint64_t page_id, uint64_t timestamp_ns) override;
    std::vector<uint64_t> predict_next_pages(uint64_t current_page_id, size_t max_count) override;
    void train(const std::vector<std::vector<uint64_t>>& access_sequences) override;
    std::string get_name() const override { return "Sequential"; }
    std::string export_model() const override;
    bool import_model(const std::string& model_data) override;
    
private:
    size_t lookahead_;
    bool adaptive_;
    uint64_t last_page_id_ = 0;
    int64_t last_stride_ = 0;
    std::deque<int64_t> recent_strides_;
    
    size_t calculate_adaptive_lookahead() const;
};

// ==================== 写合并器 ====================

/**
 * @brief 写合并器
 * @details 将多个小的写操作合并为大的写操作，减少IO次数
 */
class WriteCombiner {
public:
    struct WriteOperation {
        uint64_t page_id;
        std::vector<uint8_t> data;
        size_t offset;
        size_t size;
        uint64_t timestamp_ns;
        std::promise<bool> promise;
    };
    
    WriteCombiner(size_t max_combine_size = 16, 
                  std::chrono::milliseconds combine_window = std::chrono::milliseconds(100));
    ~WriteCombiner();
    
    std::future<bool> schedule_write(const WriteOperation& op);
    void flush();
    void stop();
    
    struct Stats {
        size_t total_operations = 0;
        size_t combined_operations = 0;
        size_t flushed_batches = 0;
        double combine_ratio = 0.0;
        uint64_t saved_io_ops = 0;
    };
    
    Stats get_stats() const;
    
private:
    size_t max_combine_size_;
    std::chrono::milliseconds combine_window_;
    
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<WriteOperation> pending_writes_;
    std::atomic<bool> running_{true};
    std::thread combine_thread_;
    
    Stats stats_;
    
    void combine_loop();
    std::vector<WriteOperation> extract_batch();
};

// ==================== 增强的缓冲池帧 ====================

/**
 * @brief 增强的缓冲池帧
 * @details 存储一个页面及其元数据，提供高级功能
 */
class EnhancedBufferFrame {
public:
    EnhancedBufferFrame(size_t page_size, size_t frame_id);
    ~EnhancedBufferFrame() = default;
    
    // 禁止拷贝和移动
    EnhancedBufferFrame(const EnhancedBufferFrame&) = delete;
    EnhancedBufferFrame& operator=(const EnhancedBufferFrame&) = delete;
    EnhancedBufferFrame(EnhancedBufferFrame&&) = delete;
    EnhancedBufferFrame& operator=(EnhancedBufferFrame&&) = delete;
    
    // ========== 基础功能（来自原 BufferFrame） ==========
    
    // 页面数据
    DiskPage& page() { return page_; }
    const DiskPage& page() const { return page_; }
    
    // Pin/Unpin操作
    void pin();
    void unpin();
    bool is_pinned() const { return pin_count_.load() > 0; }
    uint32_t pin_count() const { return pin_count_.load(); }
    
    // 脏页标记
    void mark_dirty() { dirty_.store(true); }
    void clear_dirty() { dirty_.store(false); }
    bool is_dirty() const { return dirty_.load(); }
    
    // 访问标记
    void mark_accessed();
    uint64_t last_access_time() const { return last_access_time_.load(); }
    
    // 页面ID
    void set_page_id(uint64_t page_id) { page_id_.store(page_id); }
    uint64_t page_id() const { return page_id_.load(); }
    bool is_valid() const { return page_id_.load() != 0; }
    
    // 锁定（用于并发控制）
    bool try_lock();
    void unlock();
    bool is_locked() const { return locked_.load(); }
    
    // ========== 增强功能 ==========
    
    // 访问频率统计
    uint32_t access_count() const { return access_count_.load(); }
    uint64_t first_access_time() const { return first_access_time_.load(); }
    
    // 热度评分
    double heat_score() const;
    
    // 页面状态
    enum class PageState {
        FREE,           // 空闲
        CLEAN,          // 干净
        DIRTY,          // 脏
        PENDING_READ,   // 待读
        PENDING_WRITE,  // 待写
        EVICTING,       // 正在驱逐
        CORRUPT         // 损坏
    };
    
    PageState state() const { return state_.load(); }
    void set_state(PageState state) { state_.store(state); }
    
    // IO统计
    void record_io_time(uint64_t read_time_ns, uint64_t write_time_ns);
    uint64_t total_read_time_ns() const { return total_read_time_ns_.load(); }
    uint64_t total_write_time_ns() const { return total_write_time_ns_.load(); }
    
    // 内存信息
    size_t frame_id() const { return frame_id_; }
    void* raw_data() { return page().GetData(); }
    const void* raw_data() const { return page().GetData(); }
    
    // 校验和
    uint32_t calculate_checksum() const;
    void update_checksum() { checksum_.store(calculate_checksum()); }
    uint32_t get_checksum() const { return checksum_.load(); }
    bool validate_checksum() const;
    
    // 压缩支持
    bool is_compressed() const { return compressed_.load(); }
    size_t compressed_size() const { return compressed_size_.load(); }
    bool compress();
    bool decompress();
    
private:
    DiskPage page_; // 页面数据
    std::atomic<uint64_t> page_id_{0};
    std::atomic<uint32_t> pin_count_{0}; // 引用计数
    std::atomic<bool> dirty_{false}; // 脏页标记
    std::atomic<bool> locked_{false}; // 锁定标记
    
    size_t frame_id_;
    std::atomic<uint64_t> first_access_time_{0}; // 第一次访问时间
    std::atomic<uint64_t> last_access_time_{0}; // 最后一次访问时间（合并了原 BufferFrame 的 access_time_）
    std::atomic<uint32_t> access_count_{0}; // 访问次数
    std::atomic<PageState> state_{PageState::FREE}; // 页面状态 
    std::atomic<uint64_t> total_read_time_ns_{0}; // 总读取时间
    std::atomic<uint64_t> total_write_time_ns_{0}; // 总写入时间
    std::atomic<uint32_t> checksum_{0}; // 校验和
    std::atomic<bool> compressed_{false}; // 是否压缩
    std::atomic<size_t> compressed_size_{0}; // 压缩后大小
    
    // 压缩缓冲区
    std::vector<uint8_t> compressed_buffer_; // 压缩缓冲区
    mutable std::mutex compress_mutex_; // 压缩互斥锁
};


/**
 * @brief 增强的缓冲池
 * @details 提供高级功能和优化的缓冲池实现
 */
class EnhancedBufferPool {
public:
    using BufferPoolPtr = std::unique_ptr<EnhancedBufferPool>;
    
    /**
     * @brief 创建增强缓冲池
     */
    static BufferPoolPtr create(
        FileManager* file_manager,
        const EnhancedBufferPoolConfig& config = EnhancedBufferPoolConfig::default_config());
    
    ~EnhancedBufferPool();
    
    // 禁止拷贝和移动
    EnhancedBufferPool(const EnhancedBufferPool&) = delete;
    EnhancedBufferPool& operator=(const EnhancedBufferPool&) = delete;
    EnhancedBufferPool(EnhancedBufferPool&&) = delete;
    EnhancedBufferPool& operator=(EnhancedBufferPool&&) = delete;
    
    // 基本操作
    EnhancedBufferFrame* fetch_page(uint64_t page_id, bool exclusive = false);
    EnhancedBufferFrame* new_page();
    void unpin_page(uint64_t page_id, bool mark_dirty = false);
    bool delete_page(uint64_t page_id);
    
    // 批量获取页面
    std::vector<EnhancedBufferFrame*> fetch_pages(const std::vector<uint64_t>& page_ids, bool exclusive = false);
    // 批量创建页面
    std::vector<EnhancedBufferFrame*> new_pages(size_t count);
    // 批量释放页面
    void unpin_pages(const std::vector<uint64_t>& page_ids, bool mark_dirty = false);
    
    // 预取页面
    bool prefetch_pages(const std::vector<uint64_t>& page_ids, bool low_priority = false);
    // 
    bool prefetch_predictive(uint64_t current_page_id);
    
    void flush_all();
    bool flush_page(uint64_t page_id);
    void flush_dirty_pages(bool async = true);
    
    // 内存管理
    bool resize(size_t new_size);
    bool shrink_to_fit();
    bool expand(size_t additional_pages);
    
    // 监控和调优
    struct EnhancedStatistics : public BufferPoolStatistics {
        // 性能计数器
        BufferPoolPerformanceCounters perf_counters;
        
        // 内存使用
        size_t total_memory_bytes = 0;
        size_t used_memory_bytes = 0;
        size_t wasted_memory_bytes = 0;
        double memory_utilization = 0.0;
        
        // IO统计
        uint64_t total_io_operations = 0;
        uint64_t read_operations = 0;
        uint64_t write_operations = 0;
        double avg_read_latency_ms = 0.0;
        double avg_write_latency_ms = 0.0;
        uint64_t io_throughput_bytes_per_sec = 0;
        
        // 预取效果
        double prefetch_accuracy = 0.0;
        double prefetch_coverage = 0.0;
        
        // 写优化效果
        size_t write_combine_ratio = 0;
        size_t group_commit_count = 0;
        
        // 访问模式分析
        std::string access_pattern;
        double locality_score = 0.0;
        double sequential_score = 0.0;
        
        std::string to_string(bool detailed = false) const;
        std::string to_json() const;
    };
    
    EnhancedStatistics get_statistics() const;
    BufferPoolPerformanceCounters get_performance_counters() const;
    
    void reset_statistics();
    
    // 配置管理
    const EnhancedBufferPoolConfig& get_config() const { return config_; }
    bool update_config(const EnhancedBufferPoolConfig& new_config);
    
    // 诊断和调试
    struct DiagnosticInfo {
        std::vector<uint64_t> hot_pages;
        std::vector<uint64_t> cold_pages;
        std::vector<uint64_t> dirty_pages;
        std::map<uint64_t, double> page_heat_scores;
        std::string replacer_status;
        std::string prefetcher_status;
        std::string memory_layout;
    };
    
    DiagnosticInfo get_diagnostic_info() const;
    
    // 持久化和恢复
    bool checkpoint(const std::string& path = "");
    bool restore_from_checkpoint(const std::string& path);
    
    std::string export_state() const;
    bool import_state(const std::string& state);
    
    // 健康检查
    struct HealthStatus {
        bool healthy;
        std::string status;
        std::map<std::string, std::string> details;
        std::vector<std::string> warnings;
        std::vector<std::string> errors;
    };
    
    HealthStatus health_check() const;
    
    // 调优建议
    struct TuningAdvice {
        struct Suggestion {
            std::string description;
            std::string action;
            double expected_improvement;  // 预期改善百分比
            int priority;                 // 优先级（1-10）
        };
        
        std::vector<Suggestion> suggestions;
        std::string summary;
    };
    
    TuningAdvice get_tuning_advice() const;
    
private:
    EnhancedBufferPool(FileManager* file_manager, const EnhancedBufferPoolConfig& config);
    
    // 初始化
    bool init();
    void cleanup();
    
    // 内部方法
    EnhancedBufferFrame* get_frame(size_t frame_id);
    size_t allocate_frame();
    void deallocate_frame(size_t frame_id);
    
    EnhancedBufferFrame* load_page(uint64_t page_id, bool exclusive);
    bool evict_frame(size_t frame_id);
    bool flush_frame(size_t frame_id);
    bool flush_frame_async(size_t frame_id);
    
    // 预取管理
    void prefetch_manager_loop();
    void process_prefetch_queue();
    std::vector<uint64_t> generate_prefetch_candidates(uint64_t current_page_id);
    
    // 后台线程
    void start_background_threads();
    void stop_background_threads();
    
    void flush_thread_func();
    void prefetch_thread_func();
    void stats_thread_func();
    void checkpoint_thread_func();
    void tuning_thread_func();
    
    // 动态调整
    void dynamic_adjust_loop();
    bool adjust_pool_size(size_t new_size);
    
    // 配置和组件
    EnhancedBufferPoolConfig config_;
    FileManager* file_manager_;
    std::unique_ptr<IAdvancedReplacer> replacer_;
    std::unique_ptr<IPrefetchPredictor> prefetch_predictor_;
    std::unique_ptr<WriteCombiner> write_combiner_;
    
    // 帧存储
    std::vector<std::unique_ptr<EnhancedBufferFrame>> frames_;
    std::vector<size_t> free_list_;
    mutable std::shared_mutex free_list_mutex_;
    
    // 页表
    std::unordered_map<uint64_t, size_t> page_table_;
    mutable std::shared_mutex page_table_mutex_;
    
    // 反向映射（用于快速查找）
    std::vector<uint64_t> frame_to_page_;
    
    // 预取队列
    struct PrefetchRequest {
        uint64_t page_id;
        uint64_t priority;
        uint64_t timestamp_ns;
        bool predictive;
        
        bool operator<(const PrefetchRequest& other) const {
            return priority < other.priority; // 优先级高的在前
        }
    };
    
    std::priority_queue<PrefetchRequest> prefetch_queue_;
    mutable std::mutex prefetch_queue_mutex_;
    
    // 后台线程
    std::atomic<bool> running_{false};
    std::thread flush_thread_;
    std::thread prefetch_thread_;
    std::thread stats_thread_;
    std::thread checkpoint_thread_;
    std::thread tuning_thread_;
    std::thread dynamic_adjust_thread_;
    
    // 统计和监控
    mutable std::mutex stats_mutex_;
    EnhancedStatistics stats_;
    BufferPoolPerformanceCounters perf_counters_;
    
    // 访问历史（用于模式分析）
    std::deque<uint64_t> access_history_;
    mutable std::mutex history_mutex_;
    
    // 检查点状态
    std::atomic<bool> checkpoint_in_progress_{false};
    std::string last_checkpoint_path_;
    
    // 性能监控
#ifdef BUFFER_POOL_ENABLE_PERF_COUNTERS
    BufferPoolPerformanceCounters* performance_counters_ = &perf_counters_;
#endif
    
    // 辅助方法
    static uint64_t get_current_time_ns();
    void update_statistics();
    void record_access_history(uint64_t page_id);
    
    // 锁优化
    class HierarchicalLock {
    public:
        HierarchicalLock(size_t num_buckets = 64);
        
        class BucketLock {
        public:
            void lock();
            void unlock();
            bool try_lock();
            
        private:
            std::mutex mutex_;
        };
        
        BucketLock& get_bucket(uint64_t page_id);
        
    private:
        std::vector<std::unique_ptr<BucketLock>> buckets_;
    };
    
    std::unique_ptr<HierarchicalLock> hierarchical_lock_;
    
    // 内存分配器
    class NumaAwareAllocator {
    public:
        NumaAwareAllocator(bool enable_huge_pages, int numa_node, size_t alignment);
        ~NumaAwareAllocator();
        
        void* allocate(size_t size);
        void deallocate(void* ptr, size_t size);
        
    private:
        bool enable_huge_pages_;
        int numa_node_;
        size_t alignment_;
    };
    
    std::unique_ptr<NumaAwareAllocator> memory_allocator_;
};

} // namespace core
} // namespace mementodb

#endif // BUFFER_POOL_H

