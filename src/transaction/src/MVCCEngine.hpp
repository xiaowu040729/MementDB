// File: src/transaction/src/MVCCEngine.hpp
// MVCC实现
#pragma once

#include "../include/Transaction.hpp"
#include "../include/IsolationLevel.hpp"
#include "Snapshot.hpp"
#include "VectorCharHash.hpp"
#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <atomic>
#include <vector>
#include <optional>
#include <limits>

namespace mementodb {
namespace transaction {

// Forward declarations
class TransactionContext;

/**
 * MVCCEngine - 多版本并发控制引擎
 * 
 * 实现 MVCC（Multi-Version Concurrency Control）机制
 * 支持快照隔离和读已提交隔离级别
 */
class MVCCEngine {
public:
    /**
     * MVCCConfig - MVCC配置
     */
    struct MVCCConfig {
        bool enable_snapshot_isolation;
        uint64_t version_retention_time_ms;  // 60秒
        size_t max_version_count;  // 每个key的最大版本数
        
        MVCCConfig() 
            : enable_snapshot_isolation(true)
            , version_retention_time_ms(60000)
            , max_version_count(1000) {}
    };
    
    MVCCEngine(const MVCCConfig& config = MVCCConfig{});
    ~MVCCEngine();
    
    // 禁止拷贝
    MVCCEngine(const MVCCEngine&) = delete;
    MVCCEngine& operator=(const MVCCEngine&) = delete;
    
    /**
     * create_snapshot - 为事务创建快照
     * @param tid 事务ID
     * @return 快照对象
     */
    Snapshot::Ptr create_snapshot(TransactionID tid);
    
    /**
     * read_version - 读取指定版本的数据
     * @param key 键
     * @param snapshot 快照
     * @return 数据值（如果存在）
     */
    std::optional<std::vector<char>> read_version(
        const std::vector<char>& key,
        const Snapshot& snapshot
    );
    
    /**
     * write_version - 写入新版本
     * @param tid 事务ID
     * @param key 键
     * @param value 值
     * @return 是否成功
     */
    bool write_version(
        TransactionID tid,
        const std::vector<char>& key,
        const std::vector<char>& value
    );
    
    /**
     * commit_version - 提交版本
     * @param tid 事务ID
     * @param commit_timestamp 提交时间戳
     * @return 是否成功
     */
    bool commit_version(TransactionID tid, uint64_t commit_timestamp);
    
    /**
     * abort_version - 回滚版本
     * @param tid 事务ID
     * @return 是否成功
     */
    bool abort_version(TransactionID tid);
    
    /**
     * cleanup_old_versions - 清理旧版本
     * @param current_timestamp 当前时间戳
     */
    void cleanup_old_versions(uint64_t current_timestamp);
    
    /**
     * get_version_count - 获取指定key的版本数量
     * @param key 键
     * @return 版本数量
     */
    size_t get_version_count(const std::vector<char>& key) const;

    /**
     * EngineStats - 引擎统计信息（借鉴更完整 MVCC 设计）
     */
    struct EngineStats {
        size_t total_keys{0};                 // 不同 key 的数量
        size_t total_versions{0};             // 所有版本总数
        size_t active_transactions{0};        // 有未提交写集合的事务数量
        size_t approximate_memory_bytes{0};   // 版本占用的近似内存
        uint64_t oldest_version_timestamp{0}; // 最老版本时间戳
        uint64_t newest_version_timestamp{0}; // 最新版本时间戳
    };

    /**
     * get_stats - 获取当前 MVCC 引擎的近似统计信息
     */
    EngineStats get_stats() const;

    /**
     * GCStats - 版本垃圾回收统计信息（借鉴通用 GC 设计）
     */
    struct GCStats {
        size_t total_versions{0};        // 当前剩余版本数
        size_t collected_versions{0};    // 最近一次 GC 收集的版本数
        size_t memory_reclaimed{0};      // 近似回收的内存字节数
        uint64_t last_collection_time{0}; // 最近一次 GC 的时间戳（ms）
    };

    /**
     * get_gc_stats - 获取最近一次 GC 的统计信息
     */
    GCStats get_gc_stats() const;

private:
    MVCCConfig config_;
    std::atomic<uint64_t> current_timestamp_{1};  // 内联时间戳管理
    
    // 版本存储：key -> vector of (timestamp, value)
    struct Version {
        uint64_t timestamp;
        std::vector<char> value;
        TransactionID transaction_id;
        bool committed;

        // 扩展元信息（预留给更复杂的 MVCC 策略）
        uint64_t create_txid{0};
        uint64_t delete_txid{0};
        uint64_t begin_timestamp{0};
        uint64_t end_timestamp{std::numeric_limits<uint64_t>::max()};
        bool deleted{false};

        // 简单可见性判断（当前引擎暂未使用，可用于后续演进）
        bool is_visible(uint64_t snapshot_ts) const {
            if (!committed || deleted) {
                return false;
            }
            uint64_t begin_ts = (begin_timestamp == 0) ? timestamp : begin_timestamp;
            uint64_t end_ts = end_timestamp;
            return begin_ts <= snapshot_ts && snapshot_ts < end_ts;
        }
    };
    
    std::unordered_map<std::vector<char>, std::vector<Version>, 
                      VectorCharHash> versions_;
    mutable std::shared_mutex versions_mutex_;
    
    // 事务的写集合
    std::unordered_map<TransactionID, 
                      std::vector<std::vector<char>>,
                      std::hash<TransactionID>> write_sets_;
    mutable std::mutex write_sets_mutex_;

    // GC 统计
    mutable GCStats gc_stats_;
    
    // 辅助函数
    std::optional<Version> find_readable_version(
        const std::vector<char>& key,
        uint64_t snapshot_timestamp
    ) const;
    
    void remove_old_versions(
        std::vector<Version>& versions,
        uint64_t cutoff_timestamp,
        size_t& removed_count,
        size_t& reclaimed_bytes
    );
};

} // namespace transaction
} // namespace mementodb

