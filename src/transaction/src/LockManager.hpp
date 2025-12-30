// File: src/transaction/src/LockManager.hpp
// 锁管理（含死锁检测）
#pragma once

#include "../include/Transaction.hpp"
#include "LockTable.hpp"
#include "DeadlockDetector.hpp"
#include <string>
#include <memory>
#include <mutex>
#include <chrono>

namespace mementodb {
namespace transaction {

// 前向声明
class TransactionContext;

/**
 * LockManager - 锁管理器
 * 
 * 管理事务的锁获取和释放，集成死锁检测
 */
class LockManager {
public:
    /**
     * LockResult - 锁获取结果
     */
    enum class LockResult {
        SUCCESS,           // 成功获取
        TIMEOUT,           // 超时
        DEADLOCK,          // 检测到死锁
        ERROR              // 错误
    };
    
    LockManager();
    ~LockManager();
    
    // 禁止拷贝
    LockManager(const LockManager&) = delete;
    LockManager& operator=(const LockManager&) = delete;
    
    /**
     * acquire_read_lock - 获取读锁（共享锁）
     * @param key 键
     * @param tid 事务 ID
     * @param timeout_ms 超时时间（毫秒），0 表示不等待
     * @return 锁获取结果
     */
    LockResult acquire_read_lock(const std::string& key, TransactionID tid, 
                                 uint64_t timeout_ms = 0);
    
    /**
     * acquire_write_lock - 获取写锁（排他锁）
     * @param key 键
     * @param tid 事务 ID
     * @param timeout_ms 超时时间（毫秒），0 表示不等待
     * @return 锁获取结果
     */
    LockResult acquire_write_lock(const std::string& key, TransactionID tid,
                                  uint64_t timeout_ms = 0);
    
    /**
     * release_lock - 释放锁
     * @param key 键
     * @param tid 事务 ID
     * @return 是否成功释放
     */
    bool release_lock(const std::string& key, TransactionID tid);
    
    /**
     * release_all_locks - 释放事务的所有锁
     * @param tid 事务 ID
     * @return 释放的锁数量
     */
    size_t release_all_locks(TransactionID tid);
    
    /**
     * has_read_lock - 检查是否持有读锁
     * @param key 键
     * @param tid 事务 ID
     * @return 是否持有读锁
     */
    bool has_read_lock(const std::string& key, TransactionID tid) const;
    
    /**
     * has_write_lock - 检查是否持有写锁
     * @param key 键
     * @param tid 事务 ID
     * @return 是否持有写锁
     */
    bool has_write_lock(const std::string& key, TransactionID tid) const;
    
    /**
     * upgrade_lock - 锁升级（从读锁升级到写锁）
     * @param key 键
     * @param tid 事务 ID
     * @param timeout_ms 超时时间（毫秒）
     * @return 锁获取结果
     */
    LockResult upgrade_lock(const std::string& key, TransactionID tid,
                           uint64_t timeout_ms = 0);
    
    /**
     * enable_deadlock_detection - 启用死锁检测
     * @param interval_ms 检测间隔（毫秒）
     */
    void enable_deadlock_detection(uint64_t interval_ms = 1000);
    
    /**
     * disable_deadlock_detection - 禁用死锁检测
     */
    void disable_deadlock_detection();
    
    /**
     * handle_deadlock - 处理死锁（选择牺牲者事务）
     * @param deadlock_info 死锁信息
     * @return 被选择为牺牲者的事务 ID
     */
    TransactionID handle_deadlock(const DeadlockDetector::DeadlockInfo& deadlock_info);
    
    /**
     * get_lock_table - 获取锁表（用于调试）
     */
    LockTable* get_lock_table() { return lock_table_.get(); }
    
    /**
     * get_deadlock_detector - 获取死锁检测器（用于调试）
     */
    DeadlockDetector* get_deadlock_detector() { return deadlock_detector_.get(); }
    
    /**
     * get_stats - 获取统计信息
     */
    struct Stats {
        size_t total_locks_acquired;      // 获取的锁总数
        size_t total_locks_released;       // 释放的锁总数
        size_t total_timeouts;             // 超时次数
        size_t total_deadlocks;            // 死锁次数
        size_t current_held_locks;        // 当前持有的锁数量
    };
    
    Stats get_stats() const;
    
private:
    std::unique_ptr<LockTable> lock_table_;
    std::unique_ptr<DeadlockDetector> deadlock_detector_;
    
    // 统计信息
    std::atomic<size_t> total_locks_acquired_{0};
    std::atomic<size_t> total_locks_released_{0};
    std::atomic<size_t> total_timeouts_{0};
    std::atomic<size_t> total_deadlocks_{0};
    
    // 互斥锁
    mutable std::mutex mutex_;
    
    /**
     * select_victim - 选择牺牲者事务（用于死锁解决）
     * @param cycle 死锁环中的事务列表
     * @return 被选择的事务 ID
     */
    TransactionID select_victim(const std::vector<TransactionID>& cycle);
};

} // namespace transaction
} // namespace mementodb

