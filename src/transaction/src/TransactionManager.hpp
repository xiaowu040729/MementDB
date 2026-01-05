// File: src/transaction/src/TransactionManager.hpp
// 事务生命周期管理
#pragma once

#include "../include/Transaction.hpp"
#include "../include/IsolationLevel.hpp"
#include "TransactionContext.hpp"
#include "LockManager.hpp"
#include "../include/WALInterface.hpp"
#include "RecoveryManager.hpp"
#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <atomic>
#include <functional>
#include <set>

namespace mementodb {
namespace transaction {

/**
 * TransactionManager - 事务管理器
 * 
 * 管理所有事务的生命周期：创建、提交、回滚
 */
class TransactionManager {
public:
    /**
     * TransactionConfig - 事务配置
     */
    struct TransactionConfig {
        IsolationLevel default_isolation_level; // 默认隔离级别
        uint64_t default_lock_timeout_ms;  // 5秒
        bool enable_deadlock_detection; // 是否启用死锁检测
        uint64_t deadlock_detection_interval_ms; // 死锁检测间隔
        std::string wal_data_dir;  // WAL 数据目录
        
        TransactionConfig()
            : default_isolation_level(IsolationLevel::REPEATABLE_READ)
            , default_lock_timeout_ms(5000)
            , enable_deadlock_detection(true)
            , deadlock_detection_interval_ms(1000)
        {}
    };
    
    TransactionManager(const TransactionConfig& config = TransactionConfig{});
    ~TransactionManager();
    
    // 禁止拷贝
    TransactionManager(const TransactionManager&) = delete;
    TransactionManager& operator=(const TransactionManager&) = delete;
    
    // ==================== 事务生命周期 ====================
    
    /**
     * begin_transaction - 开始新事务
     * @param isolation_level 隔离级别（使用默认值如果未指定）
     * @return 事务 ID
     */
    TransactionID begin_transaction(IsolationLevel isolation_level = IsolationLevel::REPEATABLE_READ);
    
    /**
     * commit_transaction - 提交事务
     * @param tid 事务 ID
     * @return 是否成功
     */
    bool commit_transaction(TransactionID tid);
    
    /**
     * abort_transaction - 中止事务
     * @param tid 事务 ID
     * @return 是否成功
     */
    bool abort_transaction(TransactionID tid);
    
    /**
     * rollback_transaction - 回滚事务（与 abort 相同）
     * @param tid 事务 ID
     * @return 是否成功
     */
    bool rollback_transaction(TransactionID tid) { return abort_transaction(tid); }
    
    // ==================== 事务查询 ====================
    
    /**
     * get_transaction - 获取事务上下文
     * @param tid 事务 ID
     * @return 事务上下文指针（如果存在）
     */
    std::shared_ptr<TransactionContext> get_transaction(TransactionID tid) const;
    
    /**
     * is_active - 检查事务是否活跃
     * @param tid 事务 ID
     * @return 是否活跃
     */
    bool is_active(TransactionID tid) const;
    
    /**
     * is_committed - 检查事务是否已提交
     * @param tid 事务 ID
     * @return 是否已提交
     */
    bool is_committed(TransactionID tid) const;
    
    /**
     * is_aborted - 检查事务是否已中止
     * @param tid 事务 ID
     * @return 是否已中止
     */
    bool is_aborted(TransactionID tid) const;
    
    // ==================== 锁管理 ====================
    
    /**
     * acquire_read_lock - 为事务获取读锁
     * @param tid 事务 ID
     * @param key 键
     * @param timeout_ms 超时时间（毫秒）
     * @return 是否成功
     */
    bool acquire_read_lock(TransactionID tid, const std::string& key, 
                          uint64_t timeout_ms = 0);
    
    /**
     * acquire_write_lock - 为事务获取写锁
     * @param tid 事务 ID
     * @param key 键
     * @param timeout_ms 超时时间（毫秒）
     * @return 是否成功
     */
    bool acquire_write_lock(TransactionID tid, const std::string& key,
                           uint64_t timeout_ms = 0);
    
    /**
     * release_lock - 释放锁
     * @param tid 事务 ID
     * @param key 键
     * @return 是否成功
     */
    bool release_lock(TransactionID tid, const std::string& key);
    
    // ==================== WAL 操作 ====================
    
    /**
     * log_begin - 记录事务开始
     * @param tid 事务 ID
     * @return LSN
     */
    uint64_t log_begin(TransactionID tid);
    
    /**
     * log_physical_update - 记录物理更新操作（页面级）
     * @param tid 事务 ID
     * @param page_id 页面ID
     * @param offset 页面内偏移
     * @param old_data 旧数据
     * @param new_data 新数据
     * @param length 数据长度
     * @return LSN
     */
    uint64_t log_physical_update(
        TransactionID tid,
        PageID page_id,
        uint32_t offset,
        const char* old_data,
        const char* new_data,
        uint32_t length
    );
    
    /**
     * log_commit - 记录事务提交
     * @param tid 事务 ID
     * @return LSN
     */
    uint64_t log_commit(TransactionID tid);
    
    /**
     * log_abort - 记录事务中止
     * @param tid 事务 ID
     * @return LSN
     */
    uint64_t log_abort(TransactionID tid);
    
    // ==================== 恢复 ====================
    
    /**
     * recover - 从 WAL 恢复
     * @param redo_callback 重做回调
     * @param undo_callback 回滚回调
     * @return 恢复的事务数量
     */
    size_t recover(
        RecoveryManager::RecoveryCallback redo_callback,
        RecoveryManager::RecoveryCallback undo_callback
    );
    
    // ==================== 统计信息 ====================
    
    /**
     * get_stats - 获取统计信息
     */
    struct Stats {
        size_t total_transactions;        // 总事务数
        size_t active_transactions;       // 活跃事务数
        size_t committed_transactions;    // 已提交事务数
        size_t aborted_transactions;      // 已中止事务数
    };
    
    Stats get_stats() const;
    
    /**
     * get_lock_manager - 获取锁管理器（用于调试）
     */
    LockManager* get_lock_manager() { return lock_manager_.get(); }
    
    /**
     * get_wal - 获取 WAL（用于调试）
     */
    WAL* get_wal() { return wal_.get(); }
    
private:
    TransactionConfig config_;
    
    // 事务存储：事务 ID -> 事务上下文
    std::unordered_map<TransactionID, std::shared_ptr<TransactionContext>> transactions_;
    
    // 下一个事务 ID
    std::atomic<TransactionID> next_transaction_id_{1};
    
    // 组件
    std::unique_ptr<LockManager> lock_manager_;
    std::unique_ptr<WAL> wal_;
    std::unique_ptr<RecoveryManager> recovery_manager_;
    
    // 互斥锁
    mutable std::mutex mutex_;
    
    /**
     * generate_transaction_id - 生成新的事务 ID
     */
    TransactionID generate_transaction_id();
    
    /**
     * remove_transaction - 移除事务（内部方法）
     */
    void remove_transaction(TransactionID tid);
};

} // namespace transaction
} // namespace mementodb

