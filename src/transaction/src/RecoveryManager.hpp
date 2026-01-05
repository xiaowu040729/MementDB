// File: src/transaction/src/RecoveryManager.hpp
// 恢复管理（增强版）
#pragma once

#include "../include/Transaction.hpp"
#include "../include/WALInterface.hpp"
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <memory>
#include <functional>
#include <cstddef>
#include <chrono>
#include <optional>
#include <set>
#include <map>
#include <mutex>
#include <algorithm>

namespace mementodb {
namespace transaction {

// 前向声明
class TransactionManager;

// ========== 恢复类型和配置（需要先定义，供其他接口使用）==========

/**
 * RecoveryType - 恢复类型
 */
enum class RecoveryType : uint8_t {
    CRASH_RECOVERY = 0,       // 崩溃恢复（系统异常终止）
    MEDIA_RECOVERY = 1,       // 介质恢复（磁盘损坏）
    POINT_IN_TIME = 2,        // 时间点恢复
    TRANSACTION_ROLLBACK = 3,  // 单事务回滚
    SYSTEM_RESTART = 4        // 系统重启恢复
};

/**
 * RecoveryPhase - 恢复阶段
 */
enum class RecoveryPhase : uint8_t {
    INITIALIZING = 0,         // 初始化
    ANALYSIS = 1,             // 分析阶段
    REDO = 2,                 // 重做阶段
    UNDO = 3,                 // 撤销阶段
    CLEANUP = 4,              // 清理阶段
    COMPLETED = 5,            // 完成
    FAILED = 6                // 失败
};

/**
 * RecoveryResult - 恢复结果
 */
struct RecoveryResult {
    bool success{false};
    RecoveryType type{RecoveryType::CRASH_RECOVERY};
    RecoveryPhase final_phase{RecoveryPhase::FAILED};
    uint64_t start_lsn{0};
    uint64_t end_lsn{0};
    size_t transactions_recovered{0};
    size_t pages_recovered{0};
    std::chrono::milliseconds duration{0};
    std::string error_message;
    
    // 统计信息
    struct Statistics {
        size_t log_records_processed{0};
        size_t log_records_skipped{0};
        size_t redo_operations{0};
        size_t undo_operations{0};
        size_t compensation_logs{0};
        size_t checkpoint_used{0};
    } stats;
};

/**
 * RecoveryConfig - 恢复配置
 */
struct RecoveryConfig {
    RecoveryType type{RecoveryType::CRASH_RECOVERY};
    
    // 检查点配置
    bool use_checkpoint{true};           // 是否使用检查点
    bool force_full_scan{false};         // 强制全扫描（忽略检查点）
    
    // 并行恢复配置
    bool enable_parallel_recovery{false}; // 启用并行恢复
    size_t parallel_threads{4};          // 并行线程数
    
    // 性能配置
    uint64_t redo_batch_size{1000};      // 重做批次大小
    uint64_t undo_batch_size{100};      // 撤销批次大小
    uint64_t memory_limit_mb{1024};      // 恢复内存限制
    
    // 超时配置
    uint64_t analysis_timeout_ms{30000};  // 分析阶段超时
    uint64_t redo_timeout_ms{60000};     // 重做阶段超时
    uint64_t undo_timeout_ms{30000};     // 撤销阶段超时
    
    // 安全配置
    bool verify_checksums{true};          // 验证校验和
    bool create_recovery_log{true};       // 创建恢复日志
    bool stop_on_corruption{false};       // 遇到损坏时停止
};

// ========== 存储访问抽象接口 ==========

/**
 * StorageAccessor - 存储访问器接口
 * 抽象存储层的读写操作，用于恢复时访问页面
 */
class StorageAccessor {
public:
    virtual ~StorageAccessor() = default;
    
    /**
     * read_page - 读取页面
     * @param page_id 页面ID
     * @param data 输出缓冲区（至少 page_size 字节）
     * @param page_size 页面大小
     * @return 是否成功
     */
    virtual bool read_page(PageID page_id, char* data, size_t page_size) = 0;
    
    /**
     * write_page - 写入页面
     * @param page_id 页面ID
     * @param data 数据缓冲区
     * @param page_size 页面大小
     * @return 是否成功
     */
    virtual bool write_page(PageID page_id, const char* data, size_t page_size) = 0;
    
    /**
     * get_page_size - 获取页面大小
     */
    virtual size_t get_page_size() const = 0;
    
    /**
     * flush - 刷盘
     */
    virtual void flush() = 0;
};

/**
 * CheckpointManager - 检查点管理器接口
 */
class CheckpointManager {
public:
    virtual ~CheckpointManager() = default;
    
    /**
     * get_latest_checkpoint - 获取最新检查点LSN
     * @return 检查点LSN（0表示无检查点）
     */
    virtual uint64_t get_latest_checkpoint() const = 0;
    
    /**
     * get_checkpoint_info - 获取检查点信息
     */
    struct CheckpointInfo {
        uint64_t checkpoint_lsn{0};
        uint64_t timestamp{0};
        std::set<TransactionID> active_transactions;
    };
    
    virtual CheckpointInfo get_checkpoint_info() const = 0;
    
    /**
     * create_checkpoint - 创建检查点
     * @param active_txns 活跃事务列表
     * @return 检查点LSN
     */
    virtual uint64_t create_checkpoint(const std::set<TransactionID>& active_txns) = 0;
};

/**
 * RecoveryProtocol - 恢复协议接口
 */
class RecoveryProtocol {
public:
    virtual ~RecoveryProtocol() = default;
    
    // 协议标识
    virtual std::string name() const = 0;
    virtual uint32_t version() const = 0;
    
    // 能力检测
    virtual bool supports_checkpoint() const = 0;
    virtual bool supports_parallel_recovery() const = 0;
    virtual bool supports_incremental_recovery() const = 0;
    
    // 恢复执行
    virtual RecoveryResult recover(
        WAL* wal,
        StorageAccessor* storage,
        CheckpointManager* checkpoint_mgr,
        const RecoveryConfig& config) = 0;
    
    // 验证日志记录
    virtual bool validate_log_record(const std::vector<char>& record) = 0;
    
    // 创建补偿日志记录
    virtual std::vector<char> create_compensation_log(
        uint64_t transaction_id,
        const std::vector<char>& original_log) = 0;
};

// ========== 恢复协议实现 ==========

/**
 * SimpleRedoUndoProtocol - 简单重做/撤销协议
 */
class SimpleRedoUndoProtocol : public RecoveryProtocol {
public:
    std::string name() const override { return "SimpleRedoUndo"; }
    uint32_t version() const override { return 1; }
    
    bool supports_checkpoint() const override { return false; }
    bool supports_parallel_recovery() const override { return false; }
    bool supports_incremental_recovery() const override { return false; }
    
    RecoveryResult recover(
        WAL* wal,
        StorageAccessor* storage,
        CheckpointManager* checkpoint_mgr,
        const RecoveryConfig& config) override;
    
    bool validate_log_record(const std::vector<char>& record) override;
    
    std::vector<char> create_compensation_log(
        uint64_t transaction_id,
        const std::vector<char>& original_log) override;
};

/**
 * ARIESRecoveryProtocol - ARIES恢复协议（工业标准）
 */
class ARIESRecoveryProtocol : public RecoveryProtocol {
public:
    std::string name() const override { return "ARIES"; }
    uint32_t version() const override { return 3; }
    
    bool supports_checkpoint() const override { return true; }
    bool supports_parallel_recovery() const override { return true; }
    bool supports_incremental_recovery() const override { return true; }
    
    RecoveryResult recover(
        WAL* wal,
        StorageAccessor* storage,
        CheckpointManager* checkpoint_mgr,
        const RecoveryConfig& config) override;
    
    bool validate_log_record(const std::vector<char>& record) override;
    
    std::vector<char> create_compensation_log(
        uint64_t transaction_id,
        const std::vector<char>& original_log) override;
    
    // ARIES特定方法
    struct AnalysisResult {
        std::set<uint64_t> active_transactions;
        std::map<uint64_t, uint64_t> transaction_last_lsn;
        std::map<uint64_t, uint64_t> dirty_page_table;
    };
    
    AnalysisResult perform_analysis_phase(
        WAL* wal,
        CheckpointManager* checkpoint_mgr);
};

/**
 * RecoveryMonitor - 恢复状态监控接口
 */
class RecoveryMonitor {
public:
    virtual ~RecoveryMonitor() = default;
    
    // 阶段变化
    virtual void on_phase_started(RecoveryPhase phase) {}
    virtual void on_phase_completed(RecoveryPhase phase) {}
    
    // 进度更新
    virtual void on_progress_update(
        RecoveryPhase phase,
        uint64_t processed,
        uint64_t total,
        double percentage) {}
    
    // 事务恢复事件
    virtual void on_transaction_recovered(
        uint64_t transaction_id,
        bool committed) {}
    
    // 页面恢复事件
    virtual void on_page_recovered(
        uint64_t page_id,
        uint64_t lsn) {}
    
    // 错误事件
    virtual void on_recovery_error(
        const std::string& error,
        uint64_t lsn) {}
    
    // 警告事件
    virtual void on_recovery_warning(
        const std::string& warning,
        uint64_t lsn) {}
};

/**
 * RecoveryManager - 恢复管理器（增强版）
 * 
 * 负责从 WAL 恢复未完成的事务，支持多种恢复类型和协议
 */
class RecoveryManager {
public:
    /**
     * RecoveryCallback - 恢复回调函数（保持向后兼容）
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
    ~RecoveryManager();
    
    // 禁止拷贝
    RecoveryManager(const RecoveryManager&) = delete;
    RecoveryManager& operator=(const RecoveryManager&) = delete;
    
    // ========== 核心恢复接口（增强版）==========
    
    /**
     * perform_crash_recovery - 执行崩溃恢复（新接口）
     * @param config 恢复配置
     * @return 恢复结果
     */
    RecoveryResult perform_crash_recovery(
        const RecoveryConfig& config = RecoveryConfig{});
    
    /**
     * recover_from_wal - 从 WAL 恢复（保持向后兼容）
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
     * rollback_transaction - 回滚单个事务（新接口）
     * @param transaction_id 事务ID
     * @return 是否成功
     */
    bool rollback_transaction(uint64_t transaction_id);
    
    // ========== 查询接口 ==========
    
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
        size_t total_transactions{0};            // 总事务数
        size_t total_log_records{0};             // 总日志记录数
    };
    
    RecoveryInfo analyze_wal();
    
    // ========== 监控接口 ==========
    
    /**
     * add_monitor - 添加恢复监控器
     */
    void add_monitor(std::shared_ptr<RecoveryMonitor> monitor);
    
    /**
     * remove_monitor - 移除恢复监控器
     */
    void remove_monitor(std::shared_ptr<RecoveryMonitor> monitor);
    
    /**
     * get_current_status - 获取当前恢复状态（如果正在恢复）
     */
    struct RecoveryStatus {
        bool is_recovering{false};
        RecoveryPhase current_phase{RecoveryPhase::INITIALIZING};
        double progress_percentage{0.0};
        uint64_t start_time{0};
        uint64_t estimated_completion_time{0};
        std::string current_operation;
    };
    
    std::optional<RecoveryStatus> get_current_status() const;
    
    // ========== 统计接口 ==========
    
    /**
     * get_statistics - 获取恢复统计信息
     */
    struct RecoveryStatistics {
        uint64_t total_recoveries{0};
        uint64_t successful_recoveries{0};
        uint64_t failed_recoveries{0};
        uint64_t total_recovery_time_ms{0};
        uint64_t average_recovery_time_ms{0};
        size_t max_transactions_recovered{0};
        size_t max_pages_recovered{0};
    };
    
    RecoveryStatistics get_statistics() const;
    
    /**
     * reset_statistics - 重置统计信息
     */
    void reset_statistics();
    
    /**
     * estimate_recovery_time - 估算恢复时间
     * @param start_lsn 起始LSN
     * @return 预估时间（毫秒）
     */
    uint64_t estimate_recovery_time(uint64_t start_lsn) const;
    
    /**
     * validate_recovery_readiness - 验证恢复准备状态
     */
    bool validate_recovery_readiness() const;
    
    // ========== 协议管理 ==========
    
    /**
     * register_protocol - 注册恢复协议
     */
    void register_protocol(std::unique_ptr<RecoveryProtocol> protocol);
    
    /**
     * set_default_protocol - 设置默认恢复协议
     */
    void set_default_protocol(const std::string& protocol_name);
    
    /**
     * get_supported_protocols - 获取支持的协议列表
     */
    std::vector<std::string> get_supported_protocols() const;
    
private:
    WAL* wal_;
    StorageAccessor* storage_accessor_;  // 存储访问器（可选）
    CheckpointManager* checkpoint_mgr_;  // 检查点管理器（可选）
    
    // 恢复协议
    std::map<std::string, std::unique_ptr<RecoveryProtocol>> protocols_;
    std::string default_protocol_name_;
    mutable std::mutex protocols_mutex_;
    
    // 监控器列表
    std::vector<std::weak_ptr<RecoveryMonitor>> monitors_;
    mutable std::mutex monitors_mutex_;
    
    // 恢复统计
    mutable RecoveryStatistics stats_;
    mutable std::mutex stats_mutex_;
    
    // 当前恢复状态
    std::optional<RecoveryStatus> current_status_;
    mutable std::mutex status_mutex_;
    
    /**
     * is_transaction_committed - 检查事务是否已提交
     * @param tid 事务 ID
     * @return 是否已提交
     */
    bool is_transaction_committed(TransactionID tid);
    
    /**
     * get_transaction_logs - 获取事务的所有日志记录
     * @param tid 事务 ID
     * @return 日志记录列表（按 LSN 排序，二进制格式）
     */
    std::vector<std::vector<char>> get_transaction_logs(TransactionID tid);
    
    /**
     * notify_monitors - 通知所有监控器
     */
    void notify_monitors(std::function<void(RecoveryMonitor*)> func);
};

} // namespace transaction
} // namespace mementodb

