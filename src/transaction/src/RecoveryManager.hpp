// File: src/transaction/src/RecoveryManager.hpp
// 恢复管理
#pragma once

#include "../include/Transaction.hpp"
#include "../include/WALInterface.hpp"
#include <string>
#include <vector>
#include <unordered_set>
#include <memory>
#include <functional>
#include <cstddef>

namespace mementodb {
namespace transaction {

// 前向声明
class TransactionManager;

/**
 * RecoveryManager - 恢复管理器
 * 
 * 负责从 WAL 恢复未完成的事务
 */
class RecoveryManager {
public:
    /**
     * RecoveryCallback - 恢复回调函数
     * @param page_id 页面ID（物理日志）
     * @param offset 页面内偏移
     * @param old_data 旧数据（用于回滚）
     * @param new_data 新数据（用于重做）
     * @param length 数据长度
     */
    using RecoveryCallback = std::function<void(
        PageID page_id,
        uint32_t offset,
        const char* old_data,
        const char* new_data,
        uint32_t length
    )>;
    
    RecoveryManager(WAL* wal);
    ~RecoveryManager() = default;
    
    // 禁止拷贝
    RecoveryManager(const RecoveryManager&) = delete;
    RecoveryManager& operator=(const RecoveryManager&) = delete;
    
    /**
     * recover_from_wal - 从 WAL 恢复
     * @param redo_callback 重做回调（用于重做已提交的事务）
     * @param undo_callback 回滚回调（用于回滚未提交的事务）
     * @return 恢复的事务数量
     */
    size_t recover_from_wal(
        RecoveryCallback redo_callback,
        RecoveryCallback undo_callback
    );
    
    /**
     * recover_transaction - 恢复指定事务
     * @param tid 事务 ID
     * @param redo_callback 重做回调
     * @param undo_callback 回滚回调
     * @return 是否成功恢复
     */
    bool recover_transaction(
        TransactionID tid,
        RecoveryCallback redo_callback,
        RecoveryCallback undo_callback
    );
    
    /**
     * get_uncommitted_transactions - 获取未提交的事务列表
     * @return 事务 ID 列表
     */
    std::vector<TransactionID> get_uncommitted_transactions();
    
    /**
     * get_committed_transactions - 获取已提交但未持久化的事务列表
     * @return 事务 ID 列表
     */
    std::vector<TransactionID> get_committed_transactions();
    
    /**
     * redo_transaction - 重做事务（重放已提交的操作）
     * @param tid 事务 ID
     * @param callback 重做回调
     * @return 是否成功
     */
    bool redo_transaction(TransactionID tid, RecoveryCallback callback);
    
    /**
     * undo_transaction - 回滚事务（撤销未提交的操作）
     * @param tid 事务 ID
     * @param callback 回滚回调
     * @return 是否成功
     */
    bool undo_transaction(TransactionID tid, RecoveryCallback callback);
    
    /**
     * scan_and_recover - 扫描日志并恢复
     * @param start_lsn 起始LSN（0表示从检查点开始）
     * @param redo_callback 重做回调
     * @param undo_callback 回滚回调
     * @return 恢复的事务数量
     */
    size_t scan_and_recover(
        uint64_t start_lsn,
        RecoveryCallback redo_callback,
        RecoveryCallback undo_callback
    );
    
    /**
     * analyze_wal - 分析 WAL，找出需要恢复的事务
     * @return 恢复信息
     */
    struct RecoveryInfo {
        std::vector<TransactionID> uncommitted;  // 未提交的事务
        std::vector<TransactionID> committed;     // 已提交的事务
        size_t total_transactions;                // 总事务数
        size_t total_log_records;                // 总日志记录数
    };
    
    RecoveryInfo analyze_wal();
    
private:
    WAL* wal_;
    
    /**
     * is_transaction_committed - 检查事务是否已提交
     * @param tid 事务 ID
     * @return 是否已提交
     */
    bool is_transaction_committed(TransactionID tid);
    
    /**
     * get_transaction_logs - 获取事务的所有日志记录
     * @param tid 事务 ID
     * @return 日志记录列表（按 LSN 排序）
     */
    std::vector<TransactionLogRecord> get_transaction_logs(TransactionID tid);
};

} // namespace transaction
} // namespace mementodb

