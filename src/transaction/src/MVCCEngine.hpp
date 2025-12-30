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
    std::unique_ptr<Snapshot> create_snapshot(TransactionID tid);
    
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

private:
    MVCCConfig config_;
    std::atomic<uint64_t> current_timestamp_{1};  // 内联时间戳管理
    
    // 版本存储：key -> vector of (timestamp, value)
    struct Version {
        uint64_t timestamp;
        std::vector<char> value;
        TransactionID transaction_id;
        bool committed;
    };
    
    std::unordered_map<std::vector<char>, std::vector<Version>, 
                      VectorCharHash> versions_;
    mutable std::shared_mutex versions_mutex_;
    
    // 事务的写集合
    std::unordered_map<TransactionID, 
                      std::vector<std::vector<char>>,
                      std::hash<TransactionID>> write_sets_;
    mutable std::mutex write_sets_mutex_;
    
    // 辅助函数
    std::optional<Version> find_readable_version(
        const std::vector<char>& key,
        uint64_t snapshot_timestamp
    ) const;
    
    void remove_old_versions(
        std::vector<Version>& versions,
        uint64_t current_timestamp
    );
};

} // namespace transaction
} // namespace mementodb

