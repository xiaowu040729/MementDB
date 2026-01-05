#pragma once

#include "../include/WALInterface.hpp"
#include <string>
#include <atomic>
#include <memory>
#include <functional>
#include <vector>

namespace mementodb {
namespace transaction {

// 前向声明
class LogBuffer;
class LogSegment;
class LogSegmentManager;

/**
 * WAL统计信息
 */
struct WALStatistics {
    //==================== 写入统计 ====================
    // 总字节数
    uint64_t total_bytes_written{0};
    // 总记录数
    uint64_t total_records_written{0};
    // 刷盘次数
    uint64_t flush_count{0};
    // 段切换次数
    uint64_t segment_switch_count{0};
    
    //==================== 性能统计 ====================
    // 总刷盘时间
    uint64_t total_flush_time_ns{0};
    // 最大刷盘时间
    uint64_t max_flush_time_ns{0};
    // 总写入时间
    uint64_t total_write_time_ns{0};
    // 最大写入时间
    uint64_t max_write_time_ns{0};
    
    //==================== 缓冲区统计 ====================
    // 缓冲区命中次数
    size_t buffer_hits{0};
    // 缓冲区未命中次数
    size_t buffer_misses{0};
    // 缓冲区溢出次数
    size_t buffer_overflow_count{0};
    
    //==================== 计算平均值 ====================
    // 平均刷盘时间
    double avg_flush_time_ms() const {
        return flush_count > 0 ?
            (total_flush_time_ns / (flush_count * 1000000.0)) : 0.0;
    }
    
    // 缓冲区命中率
    double buffer_hit_rate() const {
        size_t total = buffer_hits + buffer_misses;
        return total > 0 ? (buffer_hits * 100.0 / total) : 0.0;
    }
};

/**
 * WAL配置
 */
struct WALConfig {
    //==================== 目录和文件 ====================
    // 日志目录
    std::string log_dir;
    // 日志文件前缀
    std::string log_file_prefix = "wal";
    // 日志文件后缀
    std::string log_file_suffix = ".log";
    // 日志文件路径
    std::string log_file_path = log_dir + "/" + log_file_prefix + log_file_suffix;
    
    /*
    段是将 WAL 日志文件分割成多个较小文件的管理方式。每个段是一个独立的日志文件，达到大小限制后会创建新段。
    段管理：-
    1. 最大段大小：每个段的最大大小
    2. 保留的段数量：保留的段数量
    3. 是否压缩旧段：是否压缩旧段
    */
    //==================== 段管理 ====================
    // 最大段大小 
    size_t max_segment_size = 1 * 1024 * 1024 * 1024;  // 1GB
    // 保留的段数量
    size_t max_segments_to_keep = 10;                  // 保留的段数量
    // 是否压缩旧段
    bool enable_segment_compression = false;           // 是否压缩旧段
    
    //==================== 缓冲区配置 ====================
    // 缓冲区大小
    size_t buffer_size = 8 * 1024 * 1024;              // 8MB
    // 触发刷盘的阈值
    size_t flush_threshold = 6 * 1024 * 1024;          // 6MB触发刷盘
    // 定时刷盘间隔
    uint64_t flush_interval_ms = 1000;                 // 定时刷盘间隔
    
    //==================== 性能配置 ====================
    // 提交时同步刷盘
    bool sync_on_commit = true;                        // 提交时同步刷盘
    // 启用组提交
    bool enable_group_commit = true;                   // 启用组提交
    // 组提交大小
    size_t group_commit_size = 16;                     // 组提交大小
    // 组提交超时
    uint64_t group_commit_timeout_ms = 10;             // 组提交超时
    
    //==================== 恢复配置 ====================
    // 读取时验证校验和
    bool verify_checksum_on_read = true;               // 读取时验证校验和
    // 启用损坏检测
    bool enable_corruption_detection = true;           // 启用损坏检测
};

/**
 * WAL事件监听器
 */
class WALEventListener {
public:
    virtual ~WALEventListener() = default;
    
    //==================== 生命周期事件 ====================
    // WAL启动事件   
    virtual void on_wal_started() {}
    // WAL停止事件
    virtual void on_wal_stopped() {}
    
    //==================== 段事件 ====================
    // 段创建事件
    virtual void on_segment_created(uint32_t segment_id, uint64_t start_lsn) {}
    // 段关闭事件
    virtual void on_segment_closed(uint32_t segment_id, uint64_t end_lsn) {}
    // 段切换事件
    virtual void on_segment_switched(uint32_t old_segment_id, uint32_t new_segment_id) {}
    
    //==================== 刷盘事件 ====================
    // 刷盘开始事件
    virtual void on_flush_started(uint64_t lsn) {}
    // 刷盘完成事件
    virtual void on_flush_completed(uint64_t flushed_lsn, uint64_t duration_ns) {}
    
    //==================== 错误事件 ====================
    // 写入错误事件
    virtual void on_write_error(uint64_t lsn, const std::string& error) {}
    // 损坏检测事件
    virtual void on_corruption_detected(uint64_t lsn, const std::string& details) {}
};

/**
 * 文件WAL实现
 */
class FileWAL : public WAL {
public:
    explicit FileWAL(const WALConfig& config);
    ~FileWAL();
    
    // 禁止拷贝
    FileWAL(const FileWAL&) = delete;
    FileWAL& operator=(const FileWAL&) = delete;
    
    // 允许移动
    FileWAL(FileWAL&&) = default;
    FileWAL& operator=(FileWAL&&) = default;
    
    // ========== 核心接口实现 ==========

    // 开始事务
    uint64_t log_begin(TransactionID tid) override;
    // 提交事务
    uint64_t log_commit(TransactionID tid) override;
    // 中止事务
    uint64_t log_abort(TransactionID tid) override;
    
    // 物理更新
    uint64_t log_physical_update(
        TransactionID tid,
        PageID page_id,
        uint32_t offset,
        const char* old_data,
        const char* new_data,
        uint32_t length
    ) override;
    
    // 写检查点
    uint64_t write_checkpoint(
        const std::set<TransactionID>& active_txns
    ) override;
    
    // 读取日志记录
    std::vector<char> read_log_record(uint64_t lsn) override;
    // 创建扫描器
    std::unique_ptr<LogScanner> create_scanner() override;
    
    // 刷盘
    void flush() override;
    // 截断
    void truncate(uint64_t upto_lsn) override;
    // 获取当前LSN
    uint64_t get_current_lsn() const override;
    // 获取已刷盘LSN
    uint64_t get_flushed_lsn() const override;
    
    // ========== 增强接口 ==========
    
    /**
     * 批量写入（提高性能）
     */
    uint64_t log_batch(const std::vector<std::vector<char>>& records);
    
    /**
     * 异步提交（当前实现为同步调用的包装）
     */
    bool log_commit_async(TransactionID tid,
                          std::function<void(uint64_t)> callback = nullptr);
    
    /**
     * 等待直到指定LSN被持久化
     */
    bool wait_for_persistence(uint64_t lsn, uint64_t timeout_ms = 1000);
    
    // ========== 管理接口 ==========
    
    // 启动WAL
    bool start();
    // 停止WAL
    void stop();
    // 暂停WAL
    void pause();
    // 恢复WAL
    void resume();
    // 切换段
    bool rotate_segment();
    
    // ========== 监控接口 ==========
    // 获取统计信息
    WALStatistics get_statistics() const;
    // 重置统计信息
    void reset_statistics();
    // 获取配置
    WALConfig get_config() const;
    // 更新配置
    void update_config(const WALConfig& new_config);
    // 添加事件监听器
    void add_event_listener(std::shared_ptr<WALEventListener> listener);
    // 移除事件监听器
    void remove_event_listener(std::shared_ptr<WALEventListener> listener);
    
    //==================== 段信息 ====================
    struct SegmentInfo {
        // 段ID
        uint32_t segment_id;
        // 段开始LSN
        uint64_t start_lsn;
        // 段结束LSN
        uint64_t end_lsn;
        // 段文件大小
        size_t file_size;
        // 段文件路径
        std::string file_path;
        // 是否活跃
        bool is_active;
    };
    
    std::vector<SegmentInfo> get_segment_info() const;
    
    struct HealthStatus {
        // 是否健康
        bool is_healthy;
        // 状态消息
        std::string status_message;
        // 最旧的未刷盘LSN
        uint64_t oldest_unflushed_lsn;
        // 落后当前LSN的量
        uint64_t lag_behind_current;
        // 填充率百分比
        double fill_rate_percent;
    };
    
    HealthStatus get_health_status() const;
    
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// ========== 工具类和工厂函数 ==========

/**
 * 高性能文件WAL（使用内存映射文件等优化）
 */
std::unique_ptr<FileWAL> create_high_performance_wal(
    const WALConfig& config);

/**
 * 安全文件WAL（每个提交都同步刷盘）
 */
std::unique_ptr<FileWAL> create_safe_wal(
    const WALConfig& config);

/**
 * WAL恢复工具
 */
class WALRecoveryTool {
public:
    // 验证WAL
    static bool validate_wal(const std::string& log_dir);
    // 修复WAL
    static bool repair_wal(const std::string& log_dir);
    // 导出WAL
    static bool export_wal(const std::string& log_dir,
                           const std::string& output_file);
    // 分析WAL
    static WALStatistics analyze_wal(const std::string& log_dir);
};

} // namespace transaction
} // namespace mementodb

