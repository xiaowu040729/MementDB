// File: src/transaction/src/Snapshot.hpp
// 快照管理（增强版）
#pragma once

#include "../include/Transaction.hpp"
#include "VectorCharHash.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <shared_mutex>
#include <unordered_set>
#include <vector>

namespace mementodb {
namespace transaction {

/**
 * 快照类型
 */
enum class SnapshotType : uint8_t {
    READ_COMMITTED = 0,      // 读已提交快照（语句级）
    REPEATABLE_READ = 1,     // 可重复读快照（事务级）
    SNAPSHOT_ISOLATION = 2,  // 快照隔离（事务级，写写冲突检测）
    SERIALIZABLE = 3,        // 串行化快照
    CONSISTENT_READ = 4      // 一致性读快照（用于备份）
};

/**
 * 快照可见性规则（抽象基类）
 */
class Snapshot {
public:
    // 使用shared_ptr管理快照生命周期
    using Ptr = std::shared_ptr<Snapshot>;
    
    /**
     * 创建快照（工厂方法）
     * @param type 快照类型
     * @param snapshot_timestamp 快照时间戳
     * @param active_transactions 活跃事务ID集合
     * @param creator_txid 创建快照的事务ID（可选，0 表示无）
     */
    static Ptr create(
        SnapshotType type,
        uint64_t snapshot_timestamp,
        std::set<TransactionID> active_transactions = {},
        TransactionID creator_txid = 0
    );
    
    // 禁止拷贝
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;
    
    // 允许移动
    Snapshot(Snapshot&&) = default;
    Snapshot& operator=(Snapshot&&) = default;
    
    virtual ~Snapshot() = default;
    
    // ========== 核心可见性接口 ==========
    
    /**
     * 判断事务是否对快照可见
     * @param transaction_id 事务ID
     * @param transaction_commit_timestamp 事务提交时间戳（如果已提交）
     * @return 是否可见
     */
    virtual bool is_transaction_visible(
        TransactionID transaction_id,
        uint64_t transaction_commit_timestamp = 0
    ) const = 0;
    
    /**
     * 判断时间戳是否对快照可见
     * @param timestamp 时间戳
     * @return 是否可见
     */
    virtual bool is_timestamp_visible(uint64_t timestamp) const = 0;
    
    /**
     * 判断版本是否对快照可见（MVCC核心）
     * @param version_create_txid 版本创建事务ID
     * @param version_create_timestamp 版本创建时间戳
     * @param version_delete_txid 版本删除事务ID（0表示未删除）
     * @param version_delete_timestamp 版本删除时间戳
     * @return 是否可见
     */
    virtual bool is_version_visible(
        TransactionID version_create_txid,
        uint64_t version_create_timestamp,
        TransactionID version_delete_txid = 0,
        uint64_t version_delete_timestamp = 0
    ) const = 0;
    
    // ========== 信息获取接口 ==========
    
    /**
     * 获取快照ID（全局唯一）
     */
    uint64_t id() const { return snapshot_id_; }
    
    /**
     * 获取快照类型
     */
    SnapshotType type() const { return type_; }
    
    /**
     * 获取快照时间戳
     */
    uint64_t timestamp() const { return snapshot_timestamp_; }
    
    /**
     * 获取创建快照的事务ID（如果有）
     */
    std::optional<TransactionID> creator_transaction() const {
        return creator_txid_ > 0
            ? std::optional<TransactionID>(creator_txid_)
            : std::nullopt;
    }
    
    /**
     * 获取活跃事务集合（创建快照时）
     */
    const std::set<TransactionID>& active_transactions() const {
        return active_transactions_;
    }
    
    /**
     * 检查事务在快照创建时是否活跃
     */
    bool was_transaction_active(TransactionID txid) const {
        return active_transactions_.find(txid) != active_transactions_.end();
    }
    
    /**
     * 获取最早的可见时间戳（可见性地平线）
     */
    uint64_t get_visible_horizon() const;
    
    /**
     * 检查是否为最新快照
     */
    bool is_current() const;
    
    /**
     * 获取快照创建后的时间（年龄，毫秒）
     */
    uint64_t age() const;
    
    // ========== 序列化接口 ==========
    
    /**
     * 序列化快照（基础元信息，不包含读写集合）
     */
    virtual std::vector<char> serialize() const;
    
    /**
     * 反序列化快照（仅基础元信息）
     */
    static Ptr deserialize(const std::vector<char>& data);
    
protected:
    // 保护构造函数，只能通过派生类或 create() 创建
    Snapshot(
        uint64_t snapshot_id,
        SnapshotType type,
        uint64_t snapshot_timestamp,
        std::set<TransactionID> active_transactions,
        TransactionID creator_txid
    );
    
private:
    uint64_t snapshot_id_;
    SnapshotType type_;
    uint64_t snapshot_timestamp_;
    std::set<TransactionID> active_transactions_;
    TransactionID creator_txid_;
    uint64_t creation_time_; // 创建时间（毫秒，用于 age 计算）
};

// ========== 特定隔离级别的快照实现 ==========

/**
 * 读已提交快照（每次语句执行都可能不同）
 */
class ReadCommittedSnapshot : public Snapshot {
public:
    ReadCommittedSnapshot(
        uint64_t snapshot_id,
        uint64_t snapshot_timestamp,
        std::set<TransactionID> active_transactions,
        TransactionID creator_txid
    );
    
    bool is_transaction_visible(
        TransactionID transaction_id,
        uint64_t transaction_commit_timestamp = 0
    ) const override;
    
    bool is_timestamp_visible(uint64_t timestamp) const override;
    
    bool is_version_visible(
        TransactionID version_create_txid,
        uint64_t version_create_timestamp,
        TransactionID version_delete_txid = 0,
        uint64_t version_delete_timestamp = 0
    ) const override;
    
    // 读已提交快照可以更新为更晚的时间戳
    void update_timestamp(uint64_t new_timestamp);
};

/**
 * 可重复读快照（事务开始时创建）
 */
class RepeatableReadSnapshot : public Snapshot {
public:
    RepeatableReadSnapshot(
        uint64_t snapshot_id,
        uint64_t snapshot_timestamp,
        std::set<TransactionID> active_transactions,
        TransactionID creator_txid
    );
    
    bool is_transaction_visible(
        TransactionID transaction_id,
        uint64_t transaction_commit_timestamp = 0
    ) const override;
    
    bool is_timestamp_visible(uint64_t timestamp) const override;
    
    bool is_version_visible(
        TransactionID version_create_txid,
        uint64_t version_create_timestamp,
        TransactionID version_delete_txid = 0,
        uint64_t version_delete_timestamp = 0
    ) const override;
};

/**
 * 快照隔离快照（支持读写集合，用于冲突检测）
 */
class SnapshotIsolationSnapshot : public Snapshot {
public:
    SnapshotIsolationSnapshot(
        uint64_t snapshot_id,
        uint64_t snapshot_timestamp,
        std::set<TransactionID> active_transactions,
        TransactionID creator_txid
    );
    
    bool is_transaction_visible(
        TransactionID transaction_id,
        uint64_t transaction_commit_timestamp = 0
    ) const override;
    
    bool is_timestamp_visible(uint64_t timestamp) const override;
    
    bool is_version_visible(
        TransactionID version_create_txid,
        uint64_t version_create_timestamp,
        TransactionID version_delete_txid = 0,
        uint64_t version_delete_timestamp = 0
    ) const override;
    
    // 快照隔离需要记录读写集合用于冲突检测
    void add_read_key(const std::vector<char>& key);
    void add_write_key(const std::vector<char>& key);
    
    bool has_read_key(const std::vector<char>& key) const;
    bool has_write_key(const std::vector<char>& key) const;
    
private:
    // 读写集合（仅用于快照隔离的冲突检测）
    std::unordered_set<std::vector<char>, VectorCharHash> read_set_;
    std::unordered_set<std::vector<char>, VectorCharHash> write_set_;
    mutable std::shared_mutex rw_set_mutex_;
};

// ========== 快照管理器 ==========

class SnapshotManager {
public:
    SnapshotManager();
    ~SnapshotManager();
    
    // 禁止拷贝
    SnapshotManager(const SnapshotManager&) = delete;
    SnapshotManager& operator=(const SnapshotManager&) = delete;
    
    // ========== 快照创建接口 ==========
    
    /**
     * 为事务创建快照
     */
    Snapshot::Ptr create_snapshot_for_transaction(
        TransactionID txid,
        SnapshotType type
    );
    
    /**
     * 创建只读快照（不绑定事务）
     */
    Snapshot::Ptr create_readonly_snapshot(SnapshotType type);
    
    /**
     * 获取当前最新快照
     */
    Snapshot::Ptr get_current_snapshot() const;
    
    /**
     * 获取事务的快照（如果有）
     */
    Snapshot::Ptr get_snapshot_for_transaction(TransactionID txid) const;
    
    // ========== 快照管理接口 ==========
    
    /**
     * 注册事务快照
     */
    void register_transaction_snapshot(
        TransactionID txid,
        Snapshot::Ptr snapshot
    );
    
    /**
     * 注销事务快照
     */
    void unregister_transaction_snapshot(TransactionID txid);
    
    /**
     * 获取所有活跃快照
     */
    std::vector<Snapshot::Ptr> get_all_snapshots() const;
    
    /**
     * 获取最老的活跃快照（用于GC）
     */
    Snapshot::Ptr get_oldest_snapshot() const;
    
    /**
     * 垃圾回收旧快照
     * @param min_snapshot_age_ms 最小快照年龄（毫秒）
     * @return 清理的快照数量
     */
    size_t garbage_collect(uint64_t min_snapshot_age_ms = 60000);
    
    // ========== 时间戳管理 ==========
    
    /**
     * 分配新的时间戳
     */
    uint64_t allocate_timestamp();
    
    /**
     * 获取当前时间戳
     */
    uint64_t current_timestamp() const;
    
    /**
     * 获取活跃事务列表
     */
    std::set<TransactionID> get_active_transactions() const;
    
    // ========== 统计信息 ==========
    
    struct Statistics {
        uint64_t total_snapshots_created{0};
        uint64_t total_snapshots_active{0};
        uint64_t total_snapshots_collected{0};
        uint64_t current_timestamp_value{0};
        size_t active_transaction_count{0};
        uint64_t oldest_snapshot_timestamp{0};
        uint64_t newest_snapshot_timestamp{0};
    };
    
    Statistics get_statistics() const;
    
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// ========== 快照工具类 ==========

class SnapshotUtils {
public:
    /**
     * 比较两个快照的可见性范围
     */
    static bool are_snapshots_compatible(
        const Snapshot& snapshot1,
        const Snapshot& snapshot2
    );
    
    /**
     * 合并两个快照（用于分布式事务）
     */
    static Snapshot::Ptr merge_snapshots(
        const Snapshot& snapshot1,
        const Snapshot& snapshot2
    );
    
    /**
     * 创建一致性快照（用于备份）
     */
    static Snapshot::Ptr create_consistent_snapshot();
    
    /**
     * 验证快照有效性
     */
    static bool validate_snapshot(const Snapshot& snapshot);
};

// ========== 工厂函数 ==========

/**
 * 创建快照管理器
 */
std::unique_ptr<SnapshotManager> create_snapshot_manager();

/**
 * 创建高性能快照管理器（使用无锁数据结构，当前为别名）
 */
std::unique_ptr<SnapshotManager> create_high_performance_snapshot_manager();

} // namespace transaction
} // namespace mementodb


