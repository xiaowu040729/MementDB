// File: src/transaction/src/LockTable.hpp
// 锁表实现
#pragma once

#include "../include/Transaction.hpp"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <memory>
#include <optional>
#include <vector>

namespace mementodb {
namespace transaction {

// 前向声明
class LockManager;

/**
 * LockMode - 锁模式
 */
enum class LockMode {
    SHARED,      // 共享锁（读锁）
    EXCLUSIVE    // 排他锁（写锁）
};

/**
 * LockRequest - 锁请求
 */
struct LockRequest {
    TransactionID transaction_id;
    LockMode mode;
    std::chrono::steady_clock::time_point request_time;
    
    LockRequest(TransactionID tid, LockMode m)
        : transaction_id(tid), mode(m),
          request_time(std::chrono::steady_clock::now()) {}
};

/**
 * LockEntry - 锁表项
 * 
 * 表示一个键上的锁信息
 */
class LockEntry {
public:
    LockEntry();
    ~LockEntry() = default;
    
    // 禁止拷贝
    LockEntry(const LockEntry&) = delete;
    LockEntry& operator=(const LockEntry&) = delete;
    
    /**
     * try_acquire - 尝试获取锁
     * @param tid 事务 ID
     * @param mode 锁模式
     * @return 是否成功获取
     */
    bool try_acquire(TransactionID tid, LockMode mode);
    
    /**
     * release - 释放锁
     * @param tid 事务 ID
     * @return 是否成功释放
     */
    bool release(TransactionID tid);
    
    /**
     * has_lock - 检查事务是否持有锁
     * @param tid 事务 ID
     * @return 是否持有锁
     */
    bool has_lock(TransactionID tid) const;
    
    /**
     * get_lock_mode - 获取锁模式（如果持有排他锁，返回 EXCLUSIVE）
     * @param tid 事务 ID
     * @return 锁模式
     */
    LockMode get_olock_mode(TransactionID tid) const;
    
    /**
     * add_request - 添加锁请求到等待队列
     * @param request 锁请求
     */
    void add_request(const LockRequest& request);
    
    /**
     * remove_request - 从等待队列移除请求
     * @param tid 事务 ID
     */
    void remove_request(TransactionID tid);
    
    /**
     * get_waiting_transactions - 获取等待的事务 ID 列表
     */
    std::vector<TransactionID> get_waiting_transactions() const;
    
    /**
     * get_holders - 获取当前持有锁的事务 ID 列表
     */
    std::vector<TransactionID> get_holders() const;
    
    /**
     * is_exclusive_held - 是否持有排他锁
     */
    bool is_exclusive_held() const { return exclusive_holder_.has_value(); }
    
    /**
     * get_exclusive_holder - 获取排他锁持有者
     */
    std::optional<TransactionID> get_exclusive_holder() const { return exclusive_holder_; }
    
    /**
     * get_shared_holders_count - 获取共享锁持有者数量
     */
    size_t get_shared_holders_count() const { return shared_holders_.size(); }
    
private:
    // 排他锁持有者（最多一个）
    std::optional<TransactionID> exclusive_holder_;
    
    // 共享锁持有者集合
    std::unordered_set<TransactionID> shared_holders_;
    
    // 等待队列（FIFO）
    std::queue<LockRequest> waiting_queue_;
    
    // 互斥锁
    mutable std::mutex mutex_;
    std::condition_variable cv_;
};

/**
 * LockTable - 锁表
 * 
 * 管理所有键上的锁信息
 * 
 * 锁表：键 -> LockEntry
 * LockEntry：排他锁持有者 -> 共享锁持有者集合 -> 等待队列
 */
class LockTable {
public:
    LockTable();
    ~LockTable() = default;
    
    // 禁止拷贝
    LockTable(const LockTable&) = delete;
    LockTable& operator=(const LockTable&) = delete;
    
    /**
     * acquire_lock - 获取锁
     * @param key 键
     * @param tid 事务 ID
     * @param mode 锁模式
     * @param timeout_ms 超时时间（毫秒），0 表示不等待
     * @return 是否成功获取
     */
    bool acquire_lock(const std::string& key, TransactionID tid, 
                     LockMode mode, uint64_t timeout_ms = 0);
    
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
     * has_lock - 检查事务是否持有指定键的锁
     * @param key 键
     * @param tid 事务 ID
     * @return 是否持有锁
     */
    bool has_lock(const std::string& key, TransactionID tid) const;
    
    /**
     * get_lock_mode - 获取锁模式
     * @param key 键
     * @param tid 事务 ID
     * @return 锁模式
     */
    LockMode get_lock_mode(const std::string& key, TransactionID tid) const;
    
    /**
     * get_waiting_transactions - 获取等待指定键的事务列表
     * @param key 键
     * @return 事务 ID 列表
     */
    std::vector<TransactionID> get_waiting_transactions(const std::string& key) const;
    
    /**
     * get_holders - 获取持有指定键的锁的事务列表
     * @param key 键
     * @return 事务 ID 列表
     */
    std::vector<TransactionID> get_holders(const std::string& key) const;
    
    /**
     * get_all_locks - 获取事务持有的所有锁
     * @param tid 事务 ID
     * @return 键列表
     */
    std::vector<std::string> get_all_locks(TransactionID tid) const;
    
    /**
     * clear - 清空锁表
     */
    void clear();
    
    /**
     * get_all_keys - 获取所有键（用于遍历）
     * @return 所有键的列表
     */
    std::vector<std::string> get_all_keys() const;
    
    /**
     * get_stats - 获取统计信息
     */
    struct Stats {
        size_t total_entries;           // 锁表项总数
        size_t total_waiting;           // 等待的事务总数
        size_t total_held_locks;        // 持有的锁总数
    };
    

    // 获取统计信息
    Stats get_stats() const;
    
private:
    
    // 锁表：键 -> LockEntry
    std::unordered_map<std::string, std::unique_ptr<LockEntry>> lock_table_;
    
    // 事务 -> 键的映射（用于快速查找事务持有的所有锁）
    std::unordered_map<TransactionID, std::unordered_set<std::string>> transaction_locks_;
    
    // 互斥锁
    mutable std::mutex mutex_;
    
    /**
     * get_or_create_entry - 获取或创建锁表项
     */
    LockEntry* get_or_create_entry(const std::string& key);
    
    /**
     * get_entry - 获取锁表项（不创建）
     */
    LockEntry* get_entry(const std::string& key) const;
};

} // namespace transaction
} // namespace mementodb

