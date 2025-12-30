// File: src/transaction/src/TransactionContext.hpp
// 事务状态和上下文
#pragma once

#include "../include/Transaction.hpp"
#include "../include/IsolationLevel.hpp"
#include <cstdint>
#include <chrono>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>

namespace mementodb {
namespace transaction {

/**
 * TransactionContext - 事务上下文
 * 
 * 存储事务的状态、隔离级别、时间戳等信息
 */
class TransactionContext {
public:
    /**
     * 事务状态枚举
     */
    enum class TransactionState {
        ACTIVE,      // 活跃状态
        COMMITTED,   // 已提交
        ABORTED,     // 已中止
        PREPARING    // 准备提交（两阶段提交）
    };
    
    /**
     * 构造函数
     * @param tid 事务 ID
     * @param isolation_level 隔离级别
     */
    TransactionContext(TransactionID tid, IsolationLevel isolation_level);
    
    /**
     * 析构函数
     */
    ~TransactionContext() = default;
    
    // 禁止拷贝
    TransactionContext(const TransactionContext&) = delete;
    TransactionContext& operator=(const TransactionContext&) = delete;
    
    // 允许移动
    TransactionContext(TransactionContext&&) = default;
    TransactionContext& operator=(TransactionContext&&) = default;
    
    // ==================== 状态查询 ====================
    
    /**
     * get_transaction_id - 获取事务 ID
     */
    TransactionID get_transaction_id() const { return transaction_id_; }
    
    /**
     * get_isolation_level - 获取隔离级别
     */
    IsolationLevel get_isolation_level() const { return isolation_level_; }
    
    /**
     * get_state - 获取事务状态
     */
    TransactionState get_state() const { return state_.load(); }
    
    /**
     * is_active - 事务是否活跃
     */
    bool is_active() const { return state_.load() == TransactionState::ACTIVE; }
    
    /**
     * is_committed - 事务是否已提交
     */
    bool is_committed() const { return state_.load() == TransactionState::COMMITTED; }
    
    /**
     * is_aborted - 事务是否已中止
     */
    bool is_aborted() const { return state_.load() == TransactionState::ABORTED; }
    
    // ==================== 状态修改 ====================
    
    /**
     * set_state - 设置事务状态
     * @param state 新状态
     */
    void set_state(TransactionState state);
    
    /**
     * mark_committed - 标记为已提交
     */
    void mark_committed();
    
    /**
     * mark_aborted - 标记为已中止
     */
    void mark_aborted();
    
    // ==================== 时间戳管理 ====================
    
    /**
     * get_start_timestamp - 获取开始时间戳
     */
    uint64_t get_start_timestamp() const { return start_timestamp_; }
    
    /**
     * get_commit_timestamp - 获取提交时间戳（如果已提交）
     */
    uint64_t get_commit_timestamp() const { return commit_timestamp_; }
    
    /**
     * set_commit_timestamp - 设置提交时间戳
     * @param timestamp 时间戳
     */
    void set_commit_timestamp(uint64_t timestamp) { commit_timestamp_ = timestamp; }
    
    // ==================== 操作记录 ====================
    
    /**
     * add_read_key - 添加读取的键（用于可串行化检查）
     * @param key 键
     */
    void add_read_key(const std::string& key);
    
    /**
     * add_write_key - 添加写入的键（用于可串行化检查）
     * @param key 键
     */
    void add_write_key(const std::string& key);
    
    /**
     * get_read_keys - 获取所有读取的键
     */
    const std::vector<std::string>& get_read_keys() const { return read_keys_; }
    
    /**
     * get_write_keys - 获取所有写入的键
     */
    const std::vector<std::string>& get_write_keys() const { return write_keys_; }
    
    // ==================== 锁信息 ====================
    
    /**
     * add_lock - 添加持有的锁
     * @param key 键
     * @param lock_type 锁类型（共享/排他）
     */
    void add_lock(const std::string& key, bool exclusive);
    
    /**
     * remove_lock - 移除锁
     * @param key 键
     */
    void remove_lock(const std::string& key);
    
    /**
     * get_held_locks - 获取所有持有的锁
     */
    const std::vector<std::string>& get_held_locks() const { return held_locks_; }
    
    // ==================== 统计信息 ====================
    
    /**
     * get_read_count - 获取读取操作次数
     */
    size_t get_read_count() const { return read_count_; }
    
    /**
     * get_write_count - 获取写入操作次数
     */
    size_t get_write_count() const { return write_count_; }
    
    /**
     * increment_read_count - 增加读取计数
     */
    void increment_read_count() { read_count_++; }
    
    /**
     * increment_write_count - 增加写入计数
     */
    void increment_write_count() { write_count_++; }
    
private:
    TransactionID transaction_id_;
    IsolationLevel isolation_level_;
    std::atomic<TransactionState> state_;
    
    // 时间戳
    uint64_t start_timestamp_;
    uint64_t commit_timestamp_{0};
    
    // 操作记录
    std::vector<std::string> read_keys_;
    std::vector<std::string> write_keys_;
    std::vector<std::string> held_locks_;
    
    // 统计信息
    std::atomic<size_t> read_count_{0};
    std::atomic<size_t> write_count_{0};
    
    // 互斥锁（保护非原子成员）
    mutable std::mutex mutex_;
};

} // namespace transaction
} // namespace mementodb

