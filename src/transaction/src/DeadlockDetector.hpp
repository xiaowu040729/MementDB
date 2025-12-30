// File: src/transaction/src/DeadlockDetector.hpp
// 死锁检测
#pragma once

#include "../include/Transaction.hpp"
#include "LockTable.hpp"
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <optional>

namespace mementodb {
namespace transaction {

// 前向声明
class LockManager;

/**
 * DeadlockDetector - 死锁检测器
 * 
 * 使用等待图（Wait-For Graph）算法检测死锁
 */
class DeadlockDetector {
public:
    /**
     * WaitForEdge - 等待边
     * 表示事务 A 等待事务 B 释放锁
     */
    struct WaitForEdge {
        TransactionID from;  // 等待的事务
        TransactionID to;    // 被等待的事务
        std::string key;     // 等待的键
        
        WaitForEdge(TransactionID f, TransactionID t, const std::string& k)
            : from(f), to(t), key(k) {}
    };
    
    /**
     * DeadlockInfo - 死锁信息
     */
    struct DeadlockInfo {
        std::vector<TransactionID> cycle;  // 死锁环中的事务 ID
        std::vector<WaitForEdge> edges;  // 死锁环中的边
        
        DeadlockInfo() = default;
        DeadlockInfo(const std::vector<TransactionID>& c, const std::vector<WaitForEdge>& e)
            : cycle(c), edges(e) {}
    };
    
    DeadlockDetector();
    ~DeadlockDetector();
    
    // 禁止拷贝
    DeadlockDetector(const DeadlockDetector&) = delete;
    DeadlockDetector& operator=(const DeadlockDetector&) = delete;
    
    /**
     * set_lock_table - 设置锁表（用于检测）
     * @param lock_table 锁表指针
     */
    void set_lock_table(LockTable* lock_table);
    
    /**
     * detect_deadlock - 检测死锁
     * @return 死锁信息列表（如果存在）
     */
    std::vector<DeadlockInfo> detect_deadlock();
    
    /**
     * detect_deadlock_for_transaction - 检测指定事务是否参与死锁
     * @param tid 事务 ID
     * @return 死锁信息（如果存在）
     */
    std::optional<DeadlockInfo> detect_deadlock_for_transaction(TransactionID tid);
    
    /**
     * build_wait_for_graph - 构建等待图
     * @return 等待边列表
     */
    std::vector<WaitForEdge> build_wait_for_graph();
    
    /**
     * find_cycle - 在等待图中查找环
     * @param edges 等待边列表
     * @return 环中的事务 ID 列表
     */
    std::vector<std::vector<TransactionID>> find_cycles(const std::vector<WaitForEdge>& edges);
    
    /**
     * start_background_detection - 启动后台死锁检测线程
     * @param interval_ms 检测间隔（毫秒）
     */
    void start_background_detection(uint64_t interval_ms = 1000);
    
    /**
     * stop_background_detection - 停止后台死锁检测线程
     */
    void stop_background_detection();
    
    /**
     * get_detection_count - 获取检测次数
     */
    size_t get_detection_count() const { return detection_count_.load(); }
    
    /**
     * get_deadlock_count - 获取检测到的死锁数量
     */
    size_t get_deadlock_count() const { return deadlock_count_.load(); }
    
private:
    LockTable* lock_table_;
    
    // 后台检测线程
    std::atomic<bool> running_{false};
    std::thread detection_thread_;
    uint64_t detection_interval_ms_{1000};
    
    // 统计信息
    std::atomic<size_t> detection_count_{0};
    std::atomic<size_t> deadlock_count_{0};
    
    // 互斥锁
    mutable std::mutex mutex_;
    
    /**
     * detection_loop - 后台检测循环
     */
    void detection_loop();
    
    /**
     * dfs_find_cycle - 使用 DFS 查找环
     * @param graph 邻接表表示的图
     * @param node 当前节点
     * @param visited 访问标记
     * @param rec_stack 递归栈
     * @param path 当前路径
     * @param cycles 找到的环
     */
    void dfs_find_cycle(
        const std::unordered_map<TransactionID, std::vector<TransactionID>>& graph,
        TransactionID node,
        std::unordered_set<TransactionID>& visited,
        std::unordered_set<TransactionID>& rec_stack,
        std::vector<TransactionID>& path,
        std::vector<std::vector<TransactionID>>& cycles
    );
};

} // namespace transaction
} // namespace mementodb

