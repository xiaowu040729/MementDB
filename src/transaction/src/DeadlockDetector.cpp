// File: src/transaction/src/DeadlockDetector.cpp
// 死锁检测实现
#include "DeadlockDetector.hpp"
#include <algorithm>
#include <random>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

namespace mementodb {
namespace transaction {

// ========== DeadlockInfo 方法实现 ==========

uint64_t DeadlockInfo::estimate_resolution_cost() const {
    // 简单估算：环的大小 * 100
    return cycle.size() * 100;
}

std::vector<TransactionID> DeadlockInfo::get_possible_victims() const {
    return cycle;  // 环中的所有事务都可能成为牺牲者
}

// ========== DeadlockDetector::Impl ==========

class DeadlockDetector::Impl {
public:
    Impl(std::unique_ptr<WaitForGraphBuilder> graph_builder,
         std::unique_ptr<CycleDetector> cycle_detector,
         std::unique_ptr<VictimSelector> victim_selector)
        : graph_builder_(std::move(graph_builder))
        , cycle_detector_(std::move(cycle_detector))
        , victim_selector_(std::move(victim_selector))
        , running_(false)
        , paused_(false)
    {
        // 如果没有提供，使用默认实现
        if (!cycle_detector_) {
            cycle_detector_ = std::make_unique<DfsCycleDetector>();
        }
        if (!victim_selector_) {
            victim_selector_ = std::make_unique<CostBasedVictimSelector>(
                DeadlockResolutionStrategy::YOUNGEST_VICTIM);
        }
    }
    
    std::vector<TransactionID> detect_and_resolve() {
        auto deadlocks = detect_only();
        std::vector<TransactionID> victims;
        
        for (const auto& info : deadlocks) {
            if (!info.cycle.empty()) {
                auto victim = victim_selector_->choose_victim(
                    info.cycle, graph_builder_.get());
                victims.push_back(victim);
                
                // 调用回调
                if (config_.on_victim_chosen) {
                    config_.on_victim_chosen(victim);
                }
            }
        }
        
        return victims;
    }
    
    std::optional<DeadlockInfo> detect_for_transaction(TransactionID tid) {
        auto edges = graph_builder_->get_edges_for_transaction(tid);
        if (edges.empty()) {
            return std::nullopt;
        }
        
        auto cycles = cycle_detector_->find_all_cycles(edges);
        for (const auto& cycle : cycles) {
            // 检查 tid 是否在环中
            if (std::find(cycle.begin(), cycle.end(), tid) != cycle.end()) {
                DeadlockInfo info;
                info.cycle = cycle;
                info.edges = edges;
                info.detection_time = get_current_time();
                info.estimated_cost = estimate_resolution_cost(info);
                return info;
            }
        }
        
        return std::nullopt;
    }
    
    std::vector<DeadlockInfo> detect_only() {
        if (!config_.enable_cycle_detection) {
            return {};
        }
        
        auto edges = graph_builder_->get_current_edges();
        auto cycles = cycle_detector_->find_all_cycles(edges);
        
        std::vector<DeadlockInfo> deadlocks;
        size_t max_cycles = std::min(cycles.size(), config_.max_cycles_per_detection);
        
        for (size_t i = 0; i < max_cycles; ++i) {
            DeadlockInfo info;
            info.cycle = cycles[i];
            info.edges = edges;
            info.detection_time = get_current_time();
            info.estimated_cost = estimate_resolution_cost(info);
            deadlocks.push_back(info);
            
            // 调用回调
            if (config_.on_deadlock_detected) {
                config_.on_deadlock_detected(info);
            }
        }
        
        return deadlocks;
    }
    
    void set_config(const DeadlockDetectionConfig& cfg) {
        std::lock_guard<std::mutex> lock(config_mutex_);
        config_ = cfg;
    }
    
    DeadlockDetectionConfig get_config() const {
        std::lock_guard<std::mutex> lock(config_mutex_);
        return config_;
    }
    
    void start() {
        if (running_) {
            return;
        }
        
        running_ = true;
        paused_ = false;
        detection_thread_ = std::thread([this]() { detection_loop(); });
    }
    
    void stop() {
        if (!running_) {
            return;
        }
        
        running_ = false;
        cv_.notify_all();
        
        if (detection_thread_.joinable()) {
            detection_thread_.join();
        }
    }
    
    bool is_running() const {
        return running_;
    }
    
    void pause() {
        paused_ = true;
    }
    
    void resume() {
        paused_ = false;
        cv_.notify_all();
    }
    
    std::shared_ptr<DeadlockMonitor> get_monitor() {
        return monitor_;
    }
    
private:
    std::unique_ptr<WaitForGraphBuilder> graph_builder_;
    std::unique_ptr<CycleDetector> cycle_detector_;
    std::unique_ptr<VictimSelector> victim_selector_;
    
    DeadlockDetectionConfig config_;
    mutable std::mutex config_mutex_;
    
    std::atomic<bool> running_;
    std::atomic<bool> paused_;
    std::thread detection_thread_;
    std::condition_variable cv_;
    std::mutex cv_mutex_;
    
    std::shared_ptr<DeadlockMonitor> monitor_;
    
    void detection_loop() {
        while (running_) {
            {
                std::unique_lock<std::mutex> lock(cv_mutex_);
                uint64_t interval = config_.adaptive_interval 
                    ? calculate_adaptive_interval() 
                    : config_.max_detection_interval_ms;
                
                cv_.wait_for(lock, std::chrono::milliseconds(interval), 
                            [this]() { return !running_ || !paused_; });
            }
            
            if (!running_) {
                break;
            }
            
            if (!paused_) {
                detect_and_resolve();
            }
        }
    }
    
    uint64_t calculate_adaptive_interval() const {
        // 简单的自适应逻辑：根据检测到的死锁数量调整间隔
        // 死锁多 -> 缩短间隔，死锁少 -> 延长间隔
        return config_.min_detection_interval_ms;
    }
    
    static uint64_t get_current_time() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
    
    static uint64_t estimate_resolution_cost(const DeadlockInfo& info) {
        return info.cycle.size() * 100;
    }
};

// ========== DeadlockDetector 实现 ==========

DeadlockDetector::DeadlockDetector(
    std::unique_ptr<WaitForGraphBuilder> graph_builder,
    std::unique_ptr<CycleDetector> cycle_detector,
    std::unique_ptr<VictimSelector> victim_selector)
    : impl_(std::make_unique<Impl>(
          std::move(graph_builder),
          std::move(cycle_detector),
          std::move(victim_selector)))
{
}

DeadlockDetector::~DeadlockDetector() {
    stop();
}

std::vector<TransactionID> DeadlockDetector::detect_and_resolve() {
    return impl_->detect_and_resolve();
}

std::optional<DeadlockInfo> DeadlockDetector::detect_for_transaction(TransactionID tid) {
    return impl_->detect_for_transaction(tid);
}

std::vector<DeadlockInfo> DeadlockDetector::detect_only() {
    return impl_->detect_only();
}

void DeadlockDetector::set_config(const DeadlockDetectionConfig& config) {
    impl_->set_config(config);
}

DeadlockDetectionConfig DeadlockDetector::get_config() const {
    return impl_->get_config();
}

void DeadlockDetector::start() {
    impl_->start();
}

void DeadlockDetector::stop() {
    impl_->stop();
}

bool DeadlockDetector::is_running() const {
    return impl_->is_running();
}

void DeadlockDetector::pause() {
    impl_->pause();
}

void DeadlockDetector::resume() {
    impl_->resume();
}

std::shared_ptr<DeadlockMonitor> DeadlockDetector::get_monitor() {
    return impl_->get_monitor();
}

uint64_t DeadlockDetector::estimate_resolution_cost(const DeadlockInfo& info) {
    return info.cycle.size() * 100;
}

bool DeadlockDetector::has_cycle(const std::vector<WaitForEdge>& edges) {
    DfsCycleDetector detector;
    auto cycles = detector.find_all_cycles(edges);
    return !cycles.empty();
}

// ========== DfsCycleDetector 实现 ==========

std::vector<std::vector<TransactionID>> DfsCycleDetector::find_all_cycles(
    const std::vector<WaitForEdge>& edges) {
    
    // 构建邻接表
    std::unordered_map<TransactionID, std::vector<TransactionID>> graph;
    for (const auto& edge : edges) {
        graph[edge.waiter].push_back(edge.holder);
    }
    
    std::vector<std::vector<TransactionID>> cycles;
    std::unordered_set<TransactionID> visited;
    std::unordered_set<TransactionID> rec_stack;
    std::vector<TransactionID> path;
    
    for (const auto& [node, _] : graph) {
        if (visited.find(node) == visited.end()) {
            dfs_find_cycle(graph, node, visited, rec_stack, path, cycles);
        }
    }
    
    return cycles;
}

bool DfsCycleDetector::is_in_cycle(
    TransactionID tid,
    const std::vector<WaitForEdge>& edges) {
    
    auto cycles = find_all_cycles(edges);
    for (const auto& cycle : cycles) {
        if (std::find(cycle.begin(), cycle.end(), tid) != cycle.end()) {
            return true;
        }
    }
    return false;
}

void DfsCycleDetector::dfs_find_cycle(
    const std::unordered_map<TransactionID, std::vector<TransactionID>>& graph,
    TransactionID node,
    std::unordered_set<TransactionID>& visited,
    std::unordered_set<TransactionID>& rec_stack,
    std::vector<TransactionID>& path,
    std::vector<std::vector<TransactionID>>& cycles) {
    
    visited.insert(node);
    rec_stack.insert(node);
    path.push_back(node);
    
    auto it = graph.find(node);
    if (it != graph.end()) {
        for (TransactionID neighbor : it->second) {
            if (visited.find(neighbor) == visited.end()) {
                dfs_find_cycle(graph, neighbor, visited, rec_stack, path, cycles);
            } else if (rec_stack.find(neighbor) != rec_stack.end()) {
                // 找到环
                auto cycle_start = std::find(path.begin(), path.end(), neighbor);
                std::vector<TransactionID> cycle(cycle_start, path.end());
                cycle.push_back(neighbor);  // 闭合环
                cycles.push_back(cycle);
            }
        }
    }
    
    rec_stack.erase(node);
    path.pop_back();
}

// ========== CostBasedVictimSelector 实现 ==========

CostBasedVictimSelector::CostBasedVictimSelector(DeadlockResolutionStrategy strategy)
    : strategy_(strategy)
{
}

TransactionID CostBasedVictimSelector::choose_victim(
    const std::vector<TransactionID>& cycle,
    WaitForGraphBuilder* graph_builder) {
    
    if (cycle.empty()) {
        return 0;
    }
    
    auto ranked = rank_victims(cycle, graph_builder);
    return ranked.empty() ? cycle[0] : ranked[0];
}

std::vector<TransactionID> CostBasedVictimSelector::rank_victims(
    const std::vector<TransactionID>& candidates,
    WaitForGraphBuilder* graph_builder) {
    
    if (candidates.empty()) {
        return {};
    }
    
    std::vector<std::pair<TransactionID, uint64_t>> costs;
    for (TransactionID tid : candidates) {
        costs.emplace_back(tid, calculate_cost(tid, graph_builder));
    }
    
    // 根据策略排序
    switch (strategy_) {
        case DeadlockResolutionStrategy::YOUNGEST_VICTIM:
            // 代价小的在前（年轻事务）
            std::sort(costs.begin(), costs.end(),
                     [](const auto& a, const auto& b) { return a.second < b.second; });
            break;
        case DeadlockResolutionStrategy::OLDEST_VICTIM:
            // 代价大的在前（老事务）
            std::sort(costs.begin(), costs.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
            break;
        case DeadlockResolutionStrategy::SMALLEST_WORK: {
            // 修改少的事务优先
            std::sort(costs.begin(), costs.end(),
                     [graph_builder](const auto& a, const auto& b) {
                         auto info_a = graph_builder->get_transaction_info(a.first);
                         auto info_b = graph_builder->get_transaction_info(b.first);
                         return info_a.modifications_count < info_b.modifications_count;
                     });
            break;
        }
        case DeadlockResolutionStrategy::RANDOM_VICTIM: {
            // 随机打乱
            std::random_device rd;
            std::mt19937 g(rd());
            std::shuffle(costs.begin(), costs.end(), g);
            break;
        }
        default:
            // 默认按代价排序
            std::sort(costs.begin(), costs.end(),
                     [](const auto& a, const auto& b) { return a.second < b.second; });
            break;
    }
    
    std::vector<TransactionID> result;
    for (const auto& [tid, _] : costs) {
        result.push_back(tid);
    }
    
    return result;
}

uint64_t CostBasedVictimSelector::calculate_cost(
    TransactionID tid,
    WaitForGraphBuilder* graph_builder) {
    
    if (!graph_builder) {
        return 0;
    }
    
    auto info = graph_builder->get_transaction_info(tid);
    // 简单代价计算：启动时间越早，代价越大
    return info.start_time;
}

// ========== 工厂函数 ==========

std::unique_ptr<DeadlockDetector> create_default_deadlock_detector(
    std::unique_ptr<WaitForGraphBuilder> graph_builder) {
    
    return std::make_unique<DeadlockDetector>(
        std::move(graph_builder),
        std::make_unique<DfsCycleDetector>(),
        std::make_unique<CostBasedVictimSelector>(
            DeadlockResolutionStrategy::YOUNGEST_VICTIM));
}

std::unique_ptr<DeadlockDetector> create_high_performance_detector(
    std::unique_ptr<WaitForGraphBuilder> graph_builder) {
    
    auto detector = std::make_unique<DeadlockDetector>(
        std::move(graph_builder),
        std::make_unique<DfsCycleDetector>(),
        std::make_unique<CostBasedVictimSelector>(
            DeadlockResolutionStrategy::MINIMUM_COST));
    
    DeadlockDetectionConfig config;
    config.min_detection_interval_ms = 50;
    config.max_detection_interval_ms = 2000;
    config.adaptive_interval = true;
    detector->set_config(config);
    
    return detector;
}

} // namespace transaction
} // namespace mementodb

