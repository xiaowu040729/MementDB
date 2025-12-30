// File: src/transaction/src/Snapshot.hpp
// 快照管理
#pragma once

#include "../include/Transaction.hpp"
#include "VectorCharHash.hpp"
#include <cstdint>
#include <vector>
#include <unordered_set>
#include <memory>
#include <shared_mutex>

namespace mementodb {
namespace transaction {

/**
 * Snapshot - 快照对象
 * 
 * 用于快照隔离级别，记录事务开始时的数据视图
 */
class Snapshot {
public:
    /**
     * 构造函数
     * @param tid 事务ID
     * @param timestamp 快照时间戳
     */
    Snapshot(TransactionID tid, uint64_t timestamp);
    
    ~Snapshot() = default;
    
    // 禁止拷贝
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;
    
    // 允许移动
    Snapshot(Snapshot&&) = default;
    Snapshot& operator=(Snapshot&&) = default;
    
    /**
     * get_transaction_id - 获取事务ID
     * @return 事务ID
     */
    TransactionID get_transaction_id() const { return transaction_id_; }
    
    /**
     * get_timestamp - 获取快照时间戳
     * @return 时间戳
     */
    uint64_t get_timestamp() const { return timestamp_; }
    
    /**
     * add_read_key - 添加读取的键（用于检测写冲突）
     * @param key 键
     */
    void add_read_key(const std::vector<char>& key);
    
    /**
     * has_read_key - 检查是否读取过指定键
     * @param key 键
     * @return 是否读取过
     */
    bool has_read_key(const std::vector<char>& key) const;
    
    /**
     * get_read_keys - 获取所有读取的键
     * @return 读取键集合
     */
    const std::unordered_set<std::vector<char>, 
                            VectorCharHash>& get_read_keys() const {
        return read_keys_;
    }
    
    /**
     * is_valid - 检查快照是否有效
     * @param current_timestamp 当前时间戳
     * @return 是否有效
     */
    bool is_valid(uint64_t current_timestamp) const;

private:
    TransactionID transaction_id_;
    uint64_t timestamp_;
    std::unordered_set<std::vector<char>, VectorCharHash> read_keys_;
    mutable std::shared_mutex read_keys_mutex_;
};

} // namespace transaction
} // namespace mementodb

