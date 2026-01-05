// File: src/transaction/src/DeadlockDetector.hpp
// 死锁检测（新设计 - 可扩展架构）
#pragma once

#include "../include/Transaction.hpp"
#include <cstdint>
#include <vector>
#include <memory>
#include <functional>
#include <chrono>
#include <optional>
#include <unordered_set>
#include <unordered_map>

namespace mementodb {
namespace transaction {

// TransactionID 已在 Transaction.hpp 中定义为 using TransactionID = uint64_t;


/*
事务 T1: 持有锁 A，等待锁 B
事务 T2: 持有锁 B，等待锁 C
事务 T3: 持有锁 C，等待锁 A

等待图：
T1 -> T2
T2 -> T3
T3 -> T1

死锁环：
T1 -> T2 -> T3 -> T1
*/

/**
 * 等待图边
 */
struct WaitForEdge {
    TransactionID waiter;      // 等待锁的事务
    TransactionID holder;      // 持有锁的事务
    void* resource;            // 等待的资源（不限定为字符串）
    uint64_t wait_start_time;  // 等待开始时间
    
    WaitForEdge(TransactionID w, TransactionID h, void* res, uint64_t start_time = 0)
        : waiter(w), holder(h), resource(res), wait_start_time(start_time) {}
    
    bool operator==(const WaitForEdge& other) const {
        return waiter == other.waiter && 
               holder == other.holder && 
               resource == other.resource;
    }
};

/**
 * 死锁信息
 *
 * 死锁环：
 * T1 -> T2 -> T3 -> T1
 *
 * 死锁信息：
 * cycle: [T1, T2, T3]
 * edges: [T1 -> T2, T2 -> T3, T3 -> T1]
 * detection_time: 1719852000000
 * estimated_cost: 300
 */
struct DeadlockInfo {
    std::vector<TransactionID> cycle;        // 死锁环中的事务
    std::vector<WaitForEdge> edges;          // 环中的边
    uint64_t detection_time;                 // 检测时间
    uint64_t estimated_cost;                 // 预估代价（用于选择牺牲者）
    
    DeadlockInfo() : detection_time(0), estimated_cost(0) {}
    
    DeadlockInfo(const std::vector<TransactionID>& c, 
                 const std::vector<WaitForEdge>& e,
                 uint64_t det_time = 0,
                 uint64_t cost = 0)
        : cycle(c), edges(e), detection_time(det_time), estimated_cost(cost) {}
    
    // 估算解决这个死锁的代价
    uint64_t estimate_resolution_cost() const;
    
    // 获取环中可能成为牺牲者的事务
    std::vector<TransactionID> get_possible_victims() const;
};

/**
 * 死锁解决策略
 *
 * 解决策略：
 * 1. 终止最年轻的事务（基于启动时间）
 * 2. 终止最老的事务
 * 3. 终止修改最少的事务
 * 4. 终止等待时间最长的事务
 * 5. 终止代价最小的事务
 * 6. 随机选择
 * 7. 自定义策略
 */
enum class DeadlockResolutionStrategy {
    YOUNGEST_VICTIM,      // 终止最年轻的事务（基于启动时间）
    OLDEST_VICTIM,        // 终止最老的事务
    SMALLEST_WORK,        // 终止修改最少的事务
    LONGEST_WAIT,         // 终止等待时间最长的事务
    MINIMUM_COST,         // 终止代价最小的事务
    RANDOM_VICTIM,        // 随机选择
    CUSTOM                // 自定义策略
};

/**
 * 死锁检测配置
 *
 * 检测配置：
 * enable_cycle_detection: true 启用环检测
 * enable_wait_timeout: true 启用等待超时
 * enable_deadlock_preventio
 * min_detection_interval_ms: 100 最小检测间隔
 * max_detection_interval_ms: 5000 最大检测间隔
 * wait_timeout_ms: 10000 等待超时时间
 * max_cycles_per_detection: 5 每次最多检测几个环
 * resolution_strategy: YOUNGEST_VICTIM 解决策略
 * adaptive_interval: true 根据死锁频率调整间隔
 * load_factor_threshold: 0.8 负载因子阈值
 */
struct DeadlockDetectionConfig {
    // 检测策略
    bool enable_cycle_detection = true;           // 启用环检测
    bool enable_wait_timeout = true;              // 启用等待超时
    bool enable_deadlock_prevention = false;      // 启用死锁预防
    
    // 检测参数
    uint64_t min_detection_interval_ms = 100;     // 最小检测间隔
    uint64_t max_detection_interval_ms = 5000;    // 最大检测间隔
    uint64_t wait_timeout_ms = 10000;             // 等待超时时间
    size_t max_cycles_per_detection = 5;          // 每次最多检测几个环
    
    // 解决策略
    DeadlockResolutionStrategy resolution_strategy = 
        DeadlockResolutionStrategy::YOUNGEST_VICTIM;
    
    // 回调函数
    std::function<void(TransactionID)> on_victim_chosen;
    std::function<void(const DeadlockInfo&)> on_deadlock_detected;
    
    // 自适应调整
    bool adaptive_interval = true;                // 根据死锁频率调整间隔
    double load_factor_threshold = 0.8;           // 负载因子阈值
};

/**
 * 等待图构建器接口（抽象依赖）
 *
 * 等待图构建器：
 * 1. 构建等待图
 * 2. 获取当前的等待边
 * 3. 获取指定事务的等待边
 * 4. 获取所有活跃事务
 * 5. 获取事务信息（用于代价计算）
 *
 * 等待图：
 * T1 -> T2
 * T2 -> T3
 * T3 -> T1
 *
 * 等待边：
 * T1 -> T2
 * T2 -> T3
 * T3 -> T1
 *
 * 等待图构建器：
 * 1. 构建等待图
 * 2. 获取当前的等待边
 * 3. 获取指定事务的等待边
 * 4. 获取所有活跃事务
 * 5. 获取事务信息（用于代价计算）
 *
 * 等待图构建器：
 * 1. 构建等待图
 * 2. 获取当前的等待边
 * 3. 获取指定事务的等待边
 * 4. 获取所有活跃事务
 * 5. 获取事务信息（用于代价计算）
 */
class WaitForGraphBuilder {
public:
    virtual ~WaitForGraphBuilder() = default;
    
    // 获取当前的等待边
    virtual std::vector<WaitForEdge> get_current_edges() = 0;
    
    // 获取指定事务的等待边
    virtual std::vector<WaitForEdge> get_edges_for_transaction(
        TransactionID tid) = 0;
    
    // 获取所有活跃事务
    virtual std::vector<TransactionID> get_active_transactions() = 0;
    
    // 获取事务信息（用于代价计算）
    struct TransactionInfo {
        uint64_t start_time; // 开始时间
        size_t modifications_count; // 修改次数
        size_t locks_held; // 持有的锁数量
    };
    
    virtual TransactionInfo get_transaction_info(TransactionID tid) = 0;
};

/**
 * 环检测算法接口
 *
 * 环检测算法：
 * 1. 查找所有环
 * 2. 检查指定事务是否在环中
 */
class CycleDetector {
public:
    virtual ~CycleDetector() = default;
    
    // 在图中查找所有环
    virtual std::vector<std::vector<TransactionID>> find_all_cycles(
        const std::vector<WaitForEdge>& edges) = 0;
    
    // 检查指定事务是否在环中
    virtual bool is_in_cycle(
        TransactionID tid, // 事务ID
        const std::vector<WaitForEdge>& edges) = 0; // 等待边
};

/**
 * 牺牲者选择器接口
 *
 * 牺牲者选择器：
 * 1. 选择牺牲者
 * 2. 排序牺牲者
 */
class VictimSelector {
public:
    virtual ~VictimSelector() = default;
    
    // 选择牺牲者
    virtual TransactionID choose_victim(
        const std::vector<TransactionID>& cycle,
        WaitForGraphBuilder* graph_builder) = 0;
    
    // 排序牺牲者
    virtual std::vector<TransactionID> rank_victims(
        const std::vector<TransactionID>& candidates,
        WaitForGraphBuilder* graph_builder) = 0;
};

/**
 * 死锁监控器接口（用于外部监控）
 *
 * 死锁监控器：
 * 1. 注册死锁事件监听器
 * 2. 获取统计信息
 * 3. 获取当前等待图
 */
class DeadlockMonitor {
public:
    virtual ~DeadlockMonitor() = default;
    
    // 注册死锁事件监听器
    virtual void register_listener(
        std::function<void(const DeadlockInfo&)> listener) = 0;
    
    // 获取统计信息
    struct Statistics {
        uint64_t total_detections;
        uint64_t total_deadlocks_found;
        uint64_t total_resolutions;
        uint64_t last_deadlock_time;
        double average_detection_latency_ms;
    };
    
    virtual Statistics get_statistics() = 0;
    
    // 获取当前等待图
    virtual std::vector<WaitForEdge> get_current_wait_for_graph() = 0;
};

/**
 * 主死锁检测器类
 *
 * 死锁检测器：
 * 1. 检测并解决死锁
 * 2. 检测指定事务是否参与死锁
 * 3. 只检测不解决
 * 4. 配置管理
 * 5. 生命周期管理
 * 6. 监控接口
 */
class DeadlockDetector {
public:
    DeadlockDetector(
        std::unique_ptr<WaitForGraphBuilder> graph_builder,
        std::unique_ptr<CycleDetector> cycle_detector = nullptr,
        std::unique_ptr<VictimSelector> victim_selector = nullptr);
    
    ~DeadlockDetector();
    
    // 禁止拷贝
    DeadlockDetector(const DeadlockDetector&) = delete;
    DeadlockDetector& operator=(const DeadlockDetector&) = delete;
    
    // 移动语义
    DeadlockDetector(DeadlockDetector&&) = default;
    DeadlockDetector& operator=(DeadlockDetector&&) = default;
    
    // ========== 核心API ==========
    
    /**
     * 检测并解决死锁
     * @return 被选为牺牲者的事务ID列表（空表示没有死锁或解决失败）
     */
    std::vector<TransactionID> detect_and_resolve();
    
    /**
     * 检测指定事务是否参与死锁
     * @param tid 事务ID
     * @return 如果参与死锁，返回死锁信息；否则返回空
     */
    std::optional<DeadlockInfo> detect_for_transaction(TransactionID tid);
    
    /**
     * 只检测不解决
     * @return 所有检测到的死锁信息
     */
    std::vector<DeadlockInfo> detect_only();
    
    // ========== 配置管理 ==========
    
    void set_config(const DeadlockDetectionConfig& config);
    DeadlockDetectionConfig get_config() const;
    
    // ========== 生命周期管理 ==========
    
    void start();
    void stop();
    bool is_running() const;
    
    void pause();
    void resume();
    
    // ========== 监控接口 ==========
    
    std::shared_ptr<DeadlockMonitor> get_monitor();
    
    // ========== 工具方法 ==========
    
    /**
     * 估算解决死锁的代价
     */
    static uint64_t estimate_resolution_cost(const DeadlockInfo& info);
    
    /**
     * 检查等待图是否有环
     */
    static bool has_cycle(const std::vector<WaitForEdge>& edges);
    
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// ========== 内置算法实现 ==========

/**
 * 基于DFS的环检测器
 *
 * 环检测算法：
 * 1. 使用DFS查找所有环
 * 2. 检查指定事务是否在环中
 */
class DfsCycleDetector : public CycleDetector {
public:
    std::vector<std::vector<TransactionID>> find_all_cycles(
        const std::vector<WaitForEdge>& edges) override;
    
    bool is_in_cycle(
        TransactionID tid, 
        const std::vector<WaitForEdge>& edges) override;
    
private:
    void dfs_find_cycle(
        const std::unordered_map<TransactionID, std::vector<TransactionID>>& graph,
        TransactionID node,
        std::unordered_set<TransactionID>& visited,
        std::unordered_set<TransactionID>& rec_stack,
        std::vector<TransactionID>& path,
        std::vector<std::vector<TransactionID>>& cycles
    );
};

/**
 * 基于代价的牺牲者选择器
 *
 * 牺牲者选择器：
 * 1. 选择牺牲者
 * 2. 排序牺牲者
 */
class CostBasedVictimSelector : public VictimSelector {
public:
    explicit CostBasedVictimSelector(DeadlockResolutionStrategy strategy);
    
    // 选择牺牲者
    TransactionID choose_victim(
        const std::vector<TransactionID>& cycle,
        WaitForGraphBuilder* graph_builder) override;
    
    // 排序牺牲者
    std::vector<TransactionID> rank_victims(
        const std::vector<TransactionID>& candidates,
        WaitForGraphBuilder* graph_builder) override;
    
private:
    DeadlockResolutionStrategy strategy_;
    
    uint64_t calculate_cost(TransactionID tid, WaitForGraphBuilder* graph_builder);
};

// ========== 工厂函数 ==========

/**
 * 创建默认的死锁检测器
 */
std::unique_ptr<DeadlockDetector> create_default_deadlock_detector(
    std::unique_ptr<WaitForGraphBuilder> graph_builder);

/**
 * 创建高性能死锁检测器（使用增量检测）
 */
std::unique_ptr<DeadlockDetector> create_high_performance_detector(
    std::unique_ptr<WaitForGraphBuilder> graph_builder);

} // namespace transaction
} // namespace mementodb
