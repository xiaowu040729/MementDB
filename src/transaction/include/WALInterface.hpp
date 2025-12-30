// File: src/transaction/include/WALInterface.hpp
// WAL 抽象接口定义
#pragma once

#include "Transaction.hpp"
#include <cstdint>
#include <vector>
#include <string>
#include <optional>
#include <memory>
#include <set>

namespace mementodb {
namespace transaction {

// PageID 类型定义（与 core 模块的 page_id 对应）
using PageID = uint64_t;

// 前向声明
class LogScanner;

/**
 * LogType - 基础日志类型
 */
enum class LogType : uint8_t {
    BEGIN = 0x01,        // 事务开始
    COMMIT = 0x02,       // 事务提交
    ABORT = 0x03,        // 事务中止
    CHECKPOINT = 0x04,   // 检查点记录
    
    // 数据操作类型
    PHYSICAL_MUTATION = 0x10,  // 物理修改（页面级）
    LOGICAL_OPERATION = 0x11,  // 逻辑操作（操作级）
    COMPENSATION = 0x12,       // 补偿记录
    
    // 管理类型
    FILE_SWITCH = 0x20,  // 日志文件切换
};

/**
 * LogHeader - 通用日志头（所有日志都有的信息）
 */
struct LogHeader {
    uint64_t lsn;                    // 日志序列号
    uint64_t prev_lsn;               // 同一事务的上一条日志LSN
    TransactionID transaction_id;    // 事务ID
    LogType type;                    // 日志类型
    uint32_t length;                 // 记录总长度
    uint32_t checksum;               // 校验和
    uint64_t timestamp;              // 时间戳
    
    LogHeader() : lsn(0), prev_lsn(0), transaction_id(0), 
                  type(LogType::BEGIN), length(0), checksum(0), timestamp(0) {}
};

/**
 * PhysicalLogRecord - 物理数据修改记录（页面级）
 */
struct PhysicalLogRecord {
    PageID page_id;                  // 修改的页面
    uint32_t offset;                 // 页面内偏移
    uint16_t data_length;            // 数据长度
    // 变长数据部分：
    // char redo_data[data_length];  // REDO数据
    // char undo_data[data_length];  // UNDO数据（可选）
};

/**
 * CheckpointRecord - 检查点记录
 */
struct CheckpointRecord {
    uint64_t checkpoint_lsn;         // 检查点LSN
    uint32_t active_txn_count;       // 活跃事务数量
    // 变长数据部分：
    // TransactionID active_txns[active_txn_count];
};

/**
 * LogScanner - 日志扫描器接口（用于恢复）
 */
class LogScanner {
public:
    virtual ~LogScanner() = default;
    
    /**
     * open - 从指定LSN开始扫描
     * @param start_lsn 起始LSN，0表示从文件开头
     * @return 是否成功打开
     */
    virtual bool open(uint64_t start_lsn = 0) = 0;
    
    /**
     * next - 获取下一条日志记录
     * @param record 输出参数，存储日志记录（二进制格式）
     * @return 是否成功读取
     */
    virtual bool next(std::vector<char>& record) = 0;
    
    /**
     * current_lsn - 获取当前LSN
     * @return 当前LSN
     */
    virtual uint64_t current_lsn() const = 0;
    
    /**
     * eof - 是否到达文件末尾
     * @return 是否到达文件末尾
     */
    virtual bool eof() const = 0;
};

/**
 * WAL - WAL接口（抽象基类）
 * 
 * 提供统一的WAL接口，支持多种实现
 */
class WAL {
public:
    virtual ~WAL() = default;
    
    // ========== 事务日志接口 ==========
    
    /**
     * log_begin - 记录事务开始
     * @param tid 事务ID
     * @return LSN
     */
    virtual uint64_t log_begin(TransactionID tid) = 0;
    
    /**
     * log_commit - 记录事务提交
     * @param tid 事务ID
     * @return LSN
     */
    virtual uint64_t log_commit(TransactionID tid) = 0;
    
    /**
     * log_abort - 记录事务中止
     * @param tid 事务ID
     * @return LSN
     */
    virtual uint64_t log_abort(TransactionID tid) = 0;
    
    // ========== 数据修改接口 ==========
    
    /**
     * log_physical_update - 物理日志：记录页面级别的修改
     * @param tid 事务ID
     * @param page_id 页面ID
     * @param offset 页面内偏移
     * @param old_data 旧数据
     * @param new_data 新数据
     * @param length 数据长度
     * @return LSN
     */
    virtual uint64_t log_physical_update(
        TransactionID tid,
        PageID page_id,
        uint32_t offset,
        const char* old_data,
        const char* new_data,
        uint32_t length
    ) = 0;
    
    // ========== 检查点接口 ==========
    
    /**
     * write_checkpoint - 写入检查点
     * @param active_txns 活跃事务集合
     * @return 检查点LSN
     */
    virtual uint64_t write_checkpoint(
        const std::set<TransactionID>& active_txns
    ) = 0;
    
    // ========== 读取接口 ==========
    
    /**
     * read_log_record - 读取单个日志记录
     * @param lsn 日志序列号
     * @return 日志记录（二进制格式）
     */
    virtual std::vector<char> read_log_record(uint64_t lsn) = 0;
    
    /**
     * create_scanner - 创建日志扫描器（用于恢复）
     * @return 日志扫描器指针
     */
    virtual std::unique_ptr<LogScanner> create_scanner() = 0;
    
    // ========== 管理接口 ==========
    
    /**
     * flush - 强制刷盘
     */
    virtual void flush() = 0;
    
    /**
     * truncate - 截断日志（在检查点之后）
     * @param upto_lsn 截断到此LSN（不包含）
     */
    virtual void truncate(uint64_t upto_lsn) = 0;
    
    /**
     * get_current_lsn - 获取当前LSN
     * @return 当前LSN
     */
    virtual uint64_t get_current_lsn() const = 0;
    
    /**
     * get_flushed_lsn - 获取最后刷盘的LSN
     * @return 已刷盘的LSN
     */
    virtual uint64_t get_flushed_lsn() const = 0;
};

/**
 * create_file_wal - 工厂函数：创建基于文件的WAL实现
 * @param log_dir 日志目录
 * @param buffer_size 日志缓冲区大小（字节）
 * @param sync_on_commit 提交时是否同步刷盘
 * @return WAL指针
 */
std::unique_ptr<WAL> create_file_wal(
    const std::string& log_dir,
    size_t buffer_size = 8192,
    bool sync_on_commit = true
);

} // namespace transaction
} // namespace mementodb

