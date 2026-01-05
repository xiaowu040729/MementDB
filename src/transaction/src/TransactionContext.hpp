// File: src/transaction/src/TransactionContext.hpp
// 事务状态和上下文（增强版）
#pragma once

#include "../include/Transaction.hpp"
#include "../include/IsolationLevel.hpp"
#include "Snapshot.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mementodb {
namespace transaction {

// 简单类型别名（后续可与元数据模块对接）
using TableID = uint64_t;
using IndexID = uint64_t;

// 使用 LockTable 中的 LockMode 定义
enum class LockMode;

/**
 * 事务状态枚举（更细粒度）
 */
enum class TransactionState : uint8_t {
    ACTIVE = 0,          // 活跃中，可以执行操作
    PREPARING = 1,       // 正在准备提交（2PC第一阶段）
    PREPARED = 2,        // 已准备（等待提交指令）
    COMMITTING = 3,      // 正在提交
    COMMITTED = 4,       // 已提交，数据可见
    ABORTING = 5,        // 正在中止
    ABORTED = 6,         // 已中止
    ROLLBACK_ONLY = 7,   // 只能回滚（遇到不可恢复错误）
    SUSPENDED = 8,       // 挂起（等待资源）
    TIMED_OUT = 9,       // 超时
    RECOVERING = 10      // 正在恢复（崩溃后）
};

/**
 * 事务优先级
 */
enum class TransactionPriority : int32_t {
    LOWEST = -100,
    LOW = -50,
    NORMAL = 0,
    HIGH = 50,
    HIGHEST = 100,
    SYSTEM = 1000        // 系统事务（如DDL）
};

/**
 * 事务关键信息
 */
struct TransactionKey {
    enum class Type : uint8_t {
        SCALAR = 0,      // 标量键
        COMPOSITE = 1,   // 复合键
        RANGE_START = 2, // 范围开始
        RANGE_END = 3,   // 范围结束
        INDEX = 4        // 索引键
    };
    
    Type type{Type::SCALAR};
    TableID table_id{0};
    IndexID index_id{0};      // 0表示主键
    std::vector<char> data;
    std::vector<TransactionKey> components;  // 对于复合键
    
    // 比较操作
    bool operator==(const TransactionKey& other) const;
    bool operator<(const TransactionKey& other) const;
    
    // 哈希支持
    struct Hash {
        size_t operator()(const TransactionKey& key) const;
    };
};

/**
 * 事务上下文（核心类）
 */
class TransactionContext {
public:
    using Ptr = std::shared_ptr<TransactionContext>;
    
    /**
     * 锁引用信息（对外可见，用于调试和监控）
     */
    struct LockReference {
        void* lock_object{nullptr};
        std::string resource_id;
        LockMode mode{};  // 默认构造，由调用方设置
        uint64_t acquire_time{0};
    };
    
    /**
     * 创建事务上下文
     * @param tid 事务ID
     * @param isolation_level 隔离级别
     * @param priority 优先级
     */
    static Ptr create(
        TransactionID tid,
        IsolationLevel isolation_level,
        TransactionPriority priority = TransactionPriority::NORMAL
    );
    
    // 禁止拷贝
    TransactionContext(const TransactionContext&) = delete;
    TransactionContext& operator=(const TransactionContext&) = delete;
    
    // 允许移动
    TransactionContext(TransactionContext&&) = default;
    TransactionContext& operator=(TransactionContext&&) = default;
    
    virtual ~TransactionContext();
    
    // ========== 基本信息 ==========
    
    TransactionID get_id() const { return transaction_id_; }
    IsolationLevel get_isolation_level() const { return isolation_level_; }
    TransactionPriority get_priority() const { return priority_; }
    
    // ========== 状态管理 ==========
    
    TransactionState get_state() const { return state_.load(); }
    bool is_active() const;
    bool is_committed() const;
    bool is_aborted() const;
    bool is_prepared() const;
    bool is_read_only() const;
    
    /**
     * 状态转换（带检查）
     */
    bool transition_state(TransactionState new_state);
    
    /**
     * 强制状态转换（用于恢复等场景）
     */
    bool force_state(TransactionState new_state);
    
    // ========== 时间戳管理 ==========
    
    uint64_t get_start_timestamp() const { return start_timestamp_; }
    uint64_t get_commit_timestamp() const { return commit_timestamp_; }
    uint64_t get_prepare_timestamp() const { return prepare_timestamp_; }
    
    void set_commit_timestamp(uint64_t timestamp);
    void set_prepare_timestamp(uint64_t timestamp);
    
    uint64_t get_duration_ms() const;
    uint64_t get_wait_time_ms() const;
    
    // ========== 快照管理 ==========
    
    /**
     * 获取当前快照
     * @return 快照指针，可能为空（读未提交）
     */
    Snapshot::Ptr get_snapshot() const;
    
    /**
     * 设置快照（事务开始时调用）
     */
    void set_snapshot(Snapshot::Ptr snapshot);
    
    /**
     * 创建新的语句快照（读已提交隔离级别）
     */
    void create_statement_snapshot();
    
    // ========== 读/写集合管理 ==========
    
    /**
     * 添加读取的键
     */
    void add_read_key(
        const TransactionKey& key,
        uint64_t timestamp = 0
    );
    
    /**
     * 添加写入的键
     */
    void add_write_key(
        const TransactionKey& key,
        const std::vector<char>* old_value = nullptr,
        const std::vector<char>* new_value = nullptr
    );
    
    /**
     * 获取读集合
     */
    const std::vector<TransactionKey>& get_read_set() const { return read_set_; }
    
    /**
     * 获取写集合
     */
    const std::vector<TransactionKey>& get_write_set() const { return write_set_; }
    
    /**
     * 获取写操作的旧值（用于回滚）
     */
    std::optional<std::vector<char>> get_old_value(
        const TransactionKey& key
    ) const;
    
    /**
     * 获取写操作的新值
     */
    std::optional<std::vector<char>> get_new_value(
        const TransactionKey& key
    ) const;
    
    /**
     * 清除读/写集合（用于子事务或保存点）
     */
    void clear_read_write_sets();
    
    // ========== 锁管理 ==========
    
    /**
     * 添加锁引用（由锁管理器调用）
     */
    void add_lock_reference(
        void* lock_object,
        const std::string& resource_id,
        LockMode mode
    );
    
    /**
     * 移除锁引用
     */
    bool remove_lock_reference(void* lock_object);
    
    /**
     * 获取所有锁引用
     */
    const std::vector<LockReference>& get_lock_references() const {
        return lock_references_;
    }
    
    /**
     * 检查是否持有指定资源的锁
     */
    bool holds_lock(const std::string& resource_id, LockMode mode) const;
    
    // ========== WAL相关 ==========
    
    void set_first_lsn(uint64_t lsn) { first_lsn_ = lsn; }
    void set_last_lsn(uint64_t lsn) { last_lsn_ = lsn; }
    
    uint64_t get_first_lsn() const { return first_lsn_; }
    uint64_t get_last_lsn() const { return last_lsn_; }
    
    void add_prepared_lsn(uint64_t lsn);
    const std::vector<uint64_t>& get_prepared_lsns() const {
        return prepared_lsns_;
    }
    
    // ========== 保存点管理 ==========
    
    struct Savepoint {
        std::string name;
        uint64_t create_time{0};
        size_t read_set_size{0};
        size_t write_set_size{0};
        uint64_t savepoint_lsn{0};
    };
    
    /**
     * 创建保存点
     */
    bool create_savepoint(const std::string& name);
    
    /**
     * 回滚到保存点
     */
    bool rollback_to_savepoint(const std::string& name);
    
    /**
     * 释放保存点
     */
    bool release_savepoint(const std::string& name);
    
    // ========== 等待和超时 ==========
    
    void start_waiting();
    void stop_waiting();
    
    bool has_timed_out() const { return timed_out_.load(); }
    void set_timeout(uint64_t timeout_ms);
    
    // ========== 错误和回滚信息 ==========
    
    void set_error(const std::string& error);
    std::string get_error() const;
    
    void set_rollback_reason(const std::string& reason);
    std::string get_rollback_reason() const;
    
    // ========== 统计信息 ==========
    
    struct Statistics {
        size_t read_operations{0};
        size_t write_operations{0};
        size_t lock_acquisitions{0};
        size_t lock_waits{0};
        uint64_t total_wait_time_ms{0};
        size_t savepoints_created{0};
        size_t savepoints_rolled_back{0};
    };
    
    Statistics get_statistics() const;
    void increment_read_count();
    void increment_write_count();
    void increment_lock_waits();
    
    // ========== 分布式事务支持 ==========
    
    void set_global_transaction_id(const std::string& gxid) {
        global_transaction_id_ = gxid;
    }
    std::string get_global_transaction_id() const {
        return global_transaction_id_;
    }
    
    void add_branch_qualifier(const std::string& bqual) {
        branch_qualifiers_.push_back(bqual);
    }
    const std::vector<std::string>& get_branch_qualifiers() const {
        return branch_qualifiers_;
    }
    
    // ========== 序列化支持 ==========
    
    std::vector<char> serialize() const;
    static Ptr deserialize(const std::vector<char>& data);
    
private:
    // 保护构造函数，只能通过create()创建
    TransactionContext(
        TransactionID tid,
        IsolationLevel isolation_level,
        TransactionPriority priority
    );
    
    TransactionID transaction_id_;
    IsolationLevel isolation_level_;
    TransactionPriority priority_;
    
    // 状态和时间戳
    std::atomic<TransactionState> state_;
    uint64_t start_timestamp_{0};
    uint64_t prepare_timestamp_{0};
    uint64_t commit_timestamp_{0};
    
    // 快照
    Snapshot::Ptr snapshot_;
    std::vector<Snapshot::Ptr> statement_snapshots_;
    
    // 读/写集合
    std::vector<TransactionKey> read_set_;
    std::vector<TransactionKey> write_set_;
    std::unordered_map<TransactionKey, std::vector<char>,
                      TransactionKey::Hash> old_values_;
    std::unordered_map<TransactionKey, std::vector<char>,
                      TransactionKey::Hash> new_values_;
    
    // 锁引用
    std::vector<LockReference> lock_references_;
    
    // WAL信息
    uint64_t first_lsn_{0};
    uint64_t last_lsn_{0};
    std::vector<uint64_t> prepared_lsns_;
    
    // 保存点
    std::vector<Savepoint> savepoints_;
    
    // 等待和超时
    std::atomic<bool> waiting_{false};
    std::chrono::steady_clock::time_point wait_start_;
    uint64_t timeout_ms_{0};
    std::atomic<bool> timed_out_{false};
    
    // 错误信息
    std::string error_message_;
    std::string rollback_reason_;
    
    // 统计信息
    std::atomic<size_t> read_count_{0};
    std::atomic<size_t> write_count_{0};
    std::atomic<size_t> lock_waits_{0};
    std::atomic<size_t> lock_acquisitions_{0};
    std::atomic<size_t> savepoints_created_{0};
    std::atomic<size_t> savepoints_rolled_back_{0};
    
    // 分布式事务
    std::string global_transaction_id_;
    std::vector<std::string> branch_qualifiers_;
    
    // 互斥锁
    mutable std::shared_mutex mutex_;
};

/**
 * 事务上下文管理器
 */
class TransactionContextManager {
public:
    TransactionContextManager();
    ~TransactionContextManager();
    
    // 禁止拷贝
    TransactionContextManager(const TransactionContextManager&) = delete;
    TransactionContextManager& operator=(const TransactionContextManager&) = delete;
    
    // ========== 事务生命周期 ==========
    
    /**
     * 创建新事务上下文
     */
    TransactionContext::Ptr create_context(
        TransactionID tid,
        IsolationLevel isolation_level,
        TransactionPriority priority = TransactionPriority::NORMAL
    );
    
    /**
     * 获取事务上下文
     */
    TransactionContext::Ptr get_context(TransactionID tid) const;
    
    /**
     * 销毁事务上下文
     */
    bool destroy_context(TransactionID tid);
    
    // ========== 查询接口 ==========
    
    /**
     * 获取所有活跃事务
     */
    std::vector<TransactionContext::Ptr> get_active_contexts() const;
    
    /**
     * 按状态过滤事务
     */
    std::vector<TransactionContext::Ptr> get_contexts_by_state(
        TransactionState state
    ) const;
    
    /**
     * 获取最老的事务
     */
    TransactionContext::Ptr get_oldest_context() const;
    
    /**
     * 检查事务是否存在
     */
    bool exists(TransactionID tid) const;
    
    // ========== 管理接口 ==========
    
    /**
     * 强制中止事务
     */
    bool force_abort(TransactionID tid, const std::string& reason);
    
    /**
     * 标记事务超时
     */
    bool mark_timeout(TransactionID tid);
    
    /**
     * 获取统计信息
     */
    struct ManagerStatistics {
        size_t total_transactions_created{0};
        size_t active_transactions{0};
        size_t committed_transactions{0};
        size_t aborted_transactions{0};
        size_t timed_out_transactions{0};
        size_t suspended_transactions{0};
    };
    
    ManagerStatistics get_statistics() const;
    
    /**
     * 清理已完成的事务
     */
    size_t cleanup_completed_transactions();
    
    /**
     * 重置管理器
     */
    void reset();
    
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// ========== 工厂函数 ==========

/**
 * 创建事务上下文管理器
 */
std::unique_ptr<TransactionContextManager> create_transaction_context_manager();

/**
 * 创建高性能事务上下文管理器（使用无锁数据结构）
 */
std::unique_ptr<TransactionContextManager>
create_high_performance_context_manager();

} // namespace transaction
} // namespace mementodb


