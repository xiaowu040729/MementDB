// File: src/transaction/src/FileWAL.hpp
// 基于文件的 WAL 实现
#pragma once

#include "../include/WALInterface.hpp"
#include "../../utils/LoggingSystem/LogMacros.hpp"
#include <string>
#include <fstream>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <vector>
#include <memory>

namespace mementodb {
namespace transaction {

/**
 * FileLogScanner - 文件日志扫描器实现
 */
class FileLogScanner : public LogScanner {
public:
    FileLogScanner(const std::string& log_file_path);
    ~FileLogScanner();
    
    bool open(uint64_t start_lsn = 0) override;
    bool next(std::vector<char>& record) override;
    uint64_t current_lsn() const override { return current_lsn_; }
    bool eof() const override;
    
private:
    std::string log_file_path_;
    std::ifstream file_;
    uint64_t current_lsn_;
    uint64_t file_offset_;
    
    /**
     * seek_to_lsn - 定位到指定LSN
     */
    bool seek_to_lsn(uint64_t lsn);
    
    /**
     * read_log_header - 读取日志头
     */
    bool read_log_header(LogHeader& header);
};

/**
 * FileWAL - 基于文件的WAL实现
 * 
 * 实现WAL接口，提供文件持久化
 */
class FileWAL : public WAL {
public:
    FileWAL(const std::string& log_dir, size_t buffer_size, bool sync_on_commit);
    ~FileWAL();
    
    // 禁止拷贝
    FileWAL(const FileWAL&) = delete;
    FileWAL& operator=(const FileWAL&) = delete;
    
    // ========== 事务日志接口 ==========
    uint64_t log_begin(TransactionID tid) override;
    uint64_t log_commit(TransactionID tid) override;
    uint64_t log_abort(TransactionID tid) override;
    
    // ========== 数据修改接口 ==========
    uint64_t log_physical_update(
        TransactionID tid,
        PageID page_id,
        uint32_t offset,
        const char* old_data,
        const char* new_data,
        uint32_t length
    ) override;
    
    // ========== 检查点接口 ==========
    uint64_t write_checkpoint(
        const std::set<TransactionID>& active_txns
    ) override;
    
    // ========== 读取接口 ==========
    std::vector<char> read_log_record(uint64_t lsn) override;
    std::unique_ptr<LogScanner> create_scanner() override;
    
    // ========== 管理接口 ==========
    void flush() override;
    void truncate(uint64_t upto_lsn) override;
    uint64_t get_current_lsn() const override { return current_lsn_.load(); }
    uint64_t get_flushed_lsn() const override { return flushed_lsn_.load(); }
    
private:
    std::string log_dir_;
    std::string log_file_path_;
    size_t buffer_size_;
    bool sync_on_commit_;
    
    // LSN 管理
    std::atomic<uint64_t> current_lsn_{1};
    std::atomic<uint64_t> flushed_lsn_{0};
    
    // 文件管理
    std::ofstream log_file_;
    std::vector<char> write_buffer_;
    size_t buffer_pos_{0};
    
    // 事务管理（用于 prev_lsn 链）
    std::unordered_map<TransactionID, uint64_t> last_lsn_per_txn_;
    
    // 互斥锁
    mutable std::mutex mutex_;
    
    /**
     * ensure_directory - 确保目录存在
     */
    static void ensure_directory(const std::string& path);
    
    /**
     * get_log_file_path - 获取日志文件路径
     */
    std::string get_log_file_path() const;
    
    /**
     * open_log_file - 打开日志文件
     */
    bool open_log_file();
    
    /**
     * close_log_file - 关闭日志文件
     */
    void close_log_file();
    
    /**
     * write_to_buffer - 写入缓冲区
     */
    void write_to_buffer(const char* data, size_t size);
    
    /**
     * flush_buffer - 刷写缓冲区
     */
    void flush_buffer();
    
    /**
     * append_log_record - 追加日志记录（内部方法）
     */
    uint64_t append_log_record(
        TransactionID tid,
        LogType type,
        const void* data,
        size_t data_size
    );
    
    /**
     * calculate_checksum - 计算校验和
     */
    uint32_t calculate_checksum(const LogHeader& header, const void* data, size_t data_size) const;
    
    /**
     * get_prev_lsn - 获取事务的上一条日志LSN
     */
    uint64_t get_prev_lsn(TransactionID tid);
    
    /**
     * update_prev_lsn - 更新事务的上一条日志LSN
     */
    void update_prev_lsn(TransactionID tid, uint64_t lsn);
    
    /**
     * get_current_timestamp - 获取当前时间戳
     */
    static uint64_t get_current_timestamp();
};

} // namespace transaction
} // namespace mementodb

