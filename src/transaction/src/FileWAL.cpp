// 基于文件的 WAL 实现（新设计，使用 PIMPL 封装旧实现）

#include "FileWAL.hpp"
#include "../../utils/LoggingSystem/LogMacros.hpp"
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <thread>

namespace mementodb {
namespace transaction {

// ==================== 内部 FileLogScanner 实现 ====================

class FileLogScanner : public LogScanner {
public:
    explicit FileLogScanner(const std::string& log_file_path)
        : log_file_path_(log_file_path)
        , current_lsn_(0)
        , file_offset_(0) {}
    
    ~FileLogScanner() override {
        if (file_.is_open()) {
            file_.close();
        }
    }
    
    bool open(uint64_t start_lsn = 0) override {
        file_.open(log_file_path_, std::ios::binary);
        if (!file_.is_open()) {
            LOG_ERROR("WAL", "Failed to open log file for scanning: " + log_file_path_);
            return false;
        }
        
        if (start_lsn > 0) {
            if (!seek_to_lsn(start_lsn)) {
                LOG_WARN("WAL", "Failed to seek to LSN: " + std::to_string(start_lsn));
                // 继续从文件开头扫描
            }
        }
        
        current_lsn_ = start_lsn;
        file_offset_ = static_cast<uint64_t>(file_.tellg());
        return true;
    }
    
    bool next(std::vector<char>& record) override {
        if (!file_.is_open() || eof()) {
            return false;
        }
        
        // 读取日志头
        LogHeader header;
        if (!read_log_header(header)) {
            return false;
        }
        
        // 读取完整记录
        record.resize(header.length);
        file_.seekg(file_offset_);
        file_.read(reinterpret_cast<char*>(record.data()), header.length);
        
        if (file_.gcount() != static_cast<std::streamsize>(header.length)) {
            LOG_ERROR("WAL", "Failed to read complete log record: LSN=" + std::to_string(header.lsn));
            return false;
        }
        
        current_lsn_ = header.lsn;
        file_offset_ = static_cast<uint64_t>(file_.tellg());
        return true;
    }
    
    uint64_t current_lsn() const override { return current_lsn_; }
    
    bool eof() const override {
        return !file_.is_open() || file_.eof();
    }
    
private:
    std::string log_file_path_;
    std::ifstream file_;
    uint64_t current_lsn_;
    uint64_t file_offset_;
    
    bool seek_to_lsn(uint64_t lsn) {
        // 简单实现：从文件开头顺序查找
        file_.seekg(0, std::ios::beg);
        
        while (!file_.eof()) {
            uint64_t current_pos = static_cast<uint64_t>(file_.tellg());
            LogHeader header;
            if (!read_log_header(header)) {
                break;
            }
            
            if (header.lsn == lsn) {
                file_.seekg(current_pos);
                file_offset_ = current_pos;
                return true;
            }
            
            // 跳过当前记录
            file_.seekg(current_pos + header.length);
        }
        
        return false;
    }
    
    bool read_log_header(LogHeader& header) {
        file_.read(reinterpret_cast<char*>(&header), sizeof(LogHeader));
        if (file_.gcount() != static_cast<std::streamsize>(sizeof(LogHeader))) {
            return false;
        }
        return true;
    }
};

// ==================== FileWAL::Impl ====================

class FileWAL::Impl {
public:
    explicit Impl(const WALConfig& config)
        : config_(config)
        , log_dir_(config.log_dir)
        , buffer_size_(config.buffer_size)
        , sync_on_commit_(config.sync_on_commit) {
        ensure_directory(log_dir_);
        log_file_path_ = get_log_file_path();
        write_buffer_.resize(buffer_size_);
        
        if (!open_log_file()) {
            LOG_ERROR("WAL", "Failed to open log file: " + log_file_path_);
            throw std::runtime_error("Failed to initialize FileWAL");
        }
        
        LOG_INFO("WAL", "FileWAL initialized: LogDir=" + log_dir_ + 
                        ", BufferSize=" + std::to_string(buffer_size_) +
                        ", SyncOnCommit=" + (sync_on_commit_ ? "true" : "false"));
    }
    
    ~Impl() {
        flush();
        close_log_file();
        LOG_INFO("WAL", "FileWAL shutdown");
    }
    
    // ---- 核心 WAL 接口 ----
    
    uint64_t log_begin(TransactionID tid) {
        return append_log_record(tid, LogType::BEGIN, nullptr, 0);
    }
    
    uint64_t log_commit(TransactionID tid) {
        uint64_t lsn = append_log_record(tid, LogType::COMMIT, nullptr, 0);
        if (sync_on_commit_) {
            flush();
        }
        return lsn;
    }
    
    uint64_t log_abort(TransactionID tid) {
        return append_log_record(tid, LogType::ABORT, nullptr, 0);
    }
    
    uint64_t log_physical_update(
        TransactionID tid,
        PageID page_id,
        uint32_t offset,
        const char* old_data,
        const char* new_data,
        uint32_t length) {
        
        struct {
            PhysicalLogRecord record;
            char redo_data[4096];
            char undo_data[4096];
        } physical_record;
        
        physical_record.record.page_id = page_id;
        physical_record.record.offset = offset;
        physical_record.record.data_length = length;
        
        if (new_data && length > 0) {
            std::memcpy(physical_record.redo_data, new_data, length);
        }
        if (old_data && length > 0) {
            std::memcpy(physical_record.undo_data, old_data, length);
        }
        
        size_t data_size = sizeof(PhysicalLogRecord) + length * 2;
        return append_log_record(tid, LogType::PHYSICAL_MUTATION,
                                 &physical_record, data_size);
    }
    
    uint64_t write_checkpoint(const std::set<TransactionID>& active_txns) {
        struct {
            CheckpointRecord record;
            TransactionID active_txns[1024];
        } checkpoint_data;
        
        checkpoint_data.record.checkpoint_lsn = get_current_lsn();
        checkpoint_data.record.active_txn_count =
            std::min(static_cast<uint32_t>(active_txns.size()), uint32_t(1024));
        
        size_t i = 0;
        for (auto tid : active_txns) {
            if (i >= checkpoint_data.record.active_txn_count) break;
            checkpoint_data.active_txns[i++] = tid;
        }
        
        size_t data_size = sizeof(CheckpointRecord) +
                          checkpoint_data.record.active_txn_count * sizeof(TransactionID);
        
        uint64_t lsn = append_log_record(0, LogType::CHECKPOINT, &checkpoint_data, data_size);
        flush();
        return lsn;
    }
    
    std::vector<char> read_log_record(uint64_t lsn) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto scanner = create_scanner();
        if (!scanner->open(lsn)) {
            return {};
        }
        
        std::vector<char> record;
        if (scanner->next(record) && !record.empty()) {
            LogHeader header;
            std::memcpy(&header, record.data(), sizeof(LogHeader));
            if (header.lsn == lsn) {
                return record;
            }
        }
        
        return {};
    }
    
    std::unique_ptr<LogScanner> create_scanner() {
        return std::make_unique<FileLogScanner>(log_file_path_);
    }
    
    void flush() {
        std::lock_guard<std::mutex> lock(mutex_);
        flush_buffer();
        log_file_.flush();
        flushed_lsn_.store(current_lsn_.load());
        LOG_DEBUG("WAL", "Flushed WAL: FlushedLSN=" + std::to_string(flushed_lsn_.load()));
        
        stats_.flush_count++;
    }
    
    void truncate(uint64_t upto_lsn) {
        std::lock_guard<std::mutex> lock(mutex_);
        (void)upto_lsn;
        LOG_INFO("WAL", "Truncate WAL: UptoLSN=" + std::to_string(upto_lsn));
    }
    
    uint64_t get_current_lsn() const {
        return current_lsn_.load();
    }
    
    uint64_t get_flushed_lsn() const {
        return flushed_lsn_.load();
    }
    
    // ---- 增强接口（初始实现）----
    
    uint64_t log_batch(const std::vector<std::vector<char>>& records) {
        uint64_t last_lsn = 0;
        for (const auto& rec : records) {
            last_lsn = append_log_record(0, LogType::LOGICAL_OPERATION,
                                         rec.data(), rec.size());
        }
        return last_lsn;
    }
    
    bool log_commit_async(TransactionID tid,
                          std::function<void(uint64_t)> callback) {
        uint64_t lsn = log_commit(tid);
        if (callback) {
            callback(lsn);
        }
        return true;
    }
    
    bool wait_for_persistence(uint64_t lsn, uint64_t timeout_ms) {
        auto start = std::chrono::steady_clock::now();
        while (true) {
            if (get_flushed_lsn() >= lsn) {
                return true;
            }
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
            if (elapsed >= static_cast<int64_t>(timeout_ms)) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    
    bool start() {
        started_ = true;
        return true;
    }
    
    void stop() {
        started_ = false;
        flush();
    }
    
    void pause() {
        paused_ = true;
    }
    
    void resume() {
        paused_ = false;
    }
    
    bool rotate_segment() {
        flush();
        stats_.segment_switch_count++;
        return true;
    }
    
    WALStatistics get_statistics() const {
        return stats_;
    }
    
    void reset_statistics() {
        stats_ = WALStatistics{};
    }
    
    WALConfig get_config() const {
        return config_;
    }
    
    void update_config(const WALConfig& new_config) {
        config_ = new_config;
        sync_on_commit_ = config_.sync_on_commit;
    }
    
    void add_event_listener(std::shared_ptr<WALEventListener> listener) {
        std::lock_guard<std::mutex> lock(listeners_mutex_);
        listeners_.push_back(std::move(listener));
    }
    
    void remove_event_listener(std::shared_ptr<WALEventListener> listener) {
        std::lock_guard<std::mutex> lock(listeners_mutex_);
        listeners_.erase(
            std::remove(listeners_.begin(), listeners_.end(), listener),
            listeners_.end());
    }
    
    std::vector<FileWAL::SegmentInfo> get_segment_info() const {
        FileWAL::SegmentInfo info{};
        info.segment_id = 0;
        info.start_lsn = 0;
        info.end_lsn = get_current_lsn();
        info.file_path = log_file_path_;
        info.is_active = true;
        if (std::filesystem::exists(log_file_path_)) {
            info.file_size = static_cast<size_t>(std::filesystem::file_size(log_file_path_));
        } else {
            info.file_size = 0;
        }
        return {info};
    }
    
    FileWAL::HealthStatus get_health_status() const {
        FileWAL::HealthStatus h{};
        h.is_healthy = true;
        h.status_message = "OK";
        h.oldest_unflushed_lsn = get_flushed_lsn() + 1;
        h.lag_behind_current = get_current_lsn() - get_flushed_lsn();
        h.fill_rate_percent = 0.0;
        return h;
    }
    
private:
    WALConfig config_;
    
    std::string log_dir_;
    std::string log_file_path_;
    size_t buffer_size_;
    bool sync_on_commit_;
    
    std::atomic<uint64_t> current_lsn_{1};
    std::atomic<uint64_t> flushed_lsn_{0};
    
    std::ofstream log_file_;
    std::vector<char> write_buffer_;
    size_t buffer_pos_{0};
    
    std::unordered_map<TransactionID, uint64_t> last_lsn_per_txn_;
    
    mutable std::mutex mutex_;
    
    bool started_{true};
    bool paused_{false};
    
    WALStatistics stats_{};
    mutable std::mutex listeners_mutex_;
    std::vector<std::shared_ptr<WALEventListener>> listeners_;
    
    static void ensure_directory(const std::string& path) {
        std::filesystem::create_directories(path);
    }
    
    std::string get_log_file_path() const {
        return log_dir_ + "/wal.log";
    }
    
    bool open_log_file() {
        log_file_.open(log_file_path_, std::ios::binary | std::ios::app);
        return log_file_.is_open();
    }
    
    void close_log_file() {
        if (log_file_.is_open()) {
            flush_buffer();
            log_file_.close();
        }
    }
    
    void write_to_buffer(const char* data, size_t size) {
        if (buffer_pos_ + size > buffer_size_) {
            flush_buffer();
        }
        
        std::memcpy(write_buffer_.data() + buffer_pos_, data, size);
        buffer_pos_ += size;
        
        stats_.total_bytes_written += size;
    }
    
    void flush_buffer() {
        if (buffer_pos_ > 0) {
            log_file_.write(write_buffer_.data(), buffer_pos_);
            buffer_pos_ = 0;
        }
    }
    
    uint64_t append_log_record(
        TransactionID tid,
        LogType type,
        const void* data,
        size_t data_size) {
        
        std::lock_guard<std::mutex> lock(mutex_);
        
        uint64_t lsn = current_lsn_.fetch_add(1);
        uint64_t prev_lsn = get_prev_lsn(tid);
        
        LogHeader header;
        header.lsn = lsn;
        header.prev_lsn = prev_lsn;
        header.transaction_id = tid;
        header.type = type;
        header.length = static_cast<uint32_t>(sizeof(LogHeader) + data_size);
        header.timestamp = get_current_timestamp();
        header.checksum = calculate_checksum(header, data, data_size);
        
        write_to_buffer(reinterpret_cast<const char*>(&header), sizeof(LogHeader));
        
        if (data && data_size > 0) {
            write_to_buffer(static_cast<const char*>(data), data_size);
        }
        
        if (buffer_pos_ >= buffer_size_ * 0.8) {
            flush_buffer();
        }
        
        update_prev_lsn(tid, lsn);
        
        stats_.total_records_written++;
        
        LOG_DEBUG("WAL", "Appended log record: LSN=" + std::to_string(lsn) +
                         ", TID=" + std::to_string(tid) +
                         ", Type=" + std::to_string(static_cast<int>(type)));
        
        return lsn;
    }
    
    uint32_t calculate_checksum(const LogHeader& header,
                                const void* data,
                                size_t data_size) const {
        uint32_t hash = 2166136261u;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&header);
        
        for (size_t i = 0; i < sizeof(LogHeader); ++i) {
            hash ^= bytes[i];
            hash *= 16777619u;
        }
        
        if (data && data_size > 0) {
            bytes = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < data_size; ++i) {
                hash ^= bytes[i];
                hash *= 16777619u;
            }
        }
        
        return hash;
    }
    
    uint64_t get_prev_lsn(TransactionID tid) {
        auto it = last_lsn_per_txn_.find(tid);
        return (it != last_lsn_per_txn_.end()) ? it->second : 0;
    }
    
    void update_prev_lsn(TransactionID tid, uint64_t lsn) {
        last_lsn_per_txn_[tid] = lsn;
    }
    
    static uint64_t get_current_timestamp() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
};

// ==================== FileWAL 外部接口 ====================

FileWAL::FileWAL(const WALConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

FileWAL::~FileWAL() = default;

uint64_t FileWAL::log_begin(TransactionID tid) {
    return impl_->log_begin(tid);
}

uint64_t FileWAL::log_commit(TransactionID tid) {
    return impl_->log_commit(tid);
}

uint64_t FileWAL::log_abort(TransactionID tid) {
    return impl_->log_abort(tid);
}

uint64_t FileWAL::log_physical_update(
    TransactionID tid,
    PageID page_id,
    uint32_t offset,
    const char* old_data,
    const char* new_data,
    uint32_t length) {
    return impl_->log_physical_update(tid, page_id, offset, old_data, new_data, length);
}

uint64_t FileWAL::write_checkpoint(const std::set<TransactionID>& active_txns) {
    return impl_->write_checkpoint(active_txns);
}

std::vector<char> FileWAL::read_log_record(uint64_t lsn) {
    return impl_->read_log_record(lsn);
}

std::unique_ptr<LogScanner> FileWAL::create_scanner() {
    return impl_->create_scanner();
}

void FileWAL::flush() {
    impl_->flush();
}

void FileWAL::truncate(uint64_t upto_lsn) {
    impl_->truncate(upto_lsn);
}

uint64_t FileWAL::get_current_lsn() const {
    return impl_->get_current_lsn();
}

uint64_t FileWAL::get_flushed_lsn() const {
    return impl_->get_flushed_lsn();
}

uint64_t FileWAL::log_batch(const std::vector<std::vector<char>>& records) {
    return impl_->log_batch(records);
}

bool FileWAL::log_commit_async(TransactionID tid,
                               std::function<void(uint64_t)> callback) {
    return impl_->log_commit_async(tid, std::move(callback));
}

bool FileWAL::wait_for_persistence(uint64_t lsn, uint64_t timeout_ms) {
    return impl_->wait_for_persistence(lsn, timeout_ms);
}

bool FileWAL::start() {
    return impl_->start();
}

void FileWAL::stop() {
    impl_->stop();
}

void FileWAL::pause() {
    impl_->pause();
}

void FileWAL::resume() {
    impl_->resume();
}

bool FileWAL::rotate_segment() {
    return impl_->rotate_segment();
}

WALStatistics FileWAL::get_statistics() const {
    return impl_->get_statistics();
}

void FileWAL::reset_statistics() {
    impl_->reset_statistics();
}

WALConfig FileWAL::get_config() const {
    return impl_->get_config();
}

void FileWAL::update_config(const WALConfig& new_config) {
    impl_->update_config(new_config);
}

void FileWAL::add_event_listener(std::shared_ptr<WALEventListener> listener) {
    impl_->add_event_listener(std::move(listener));
}

void FileWAL::remove_event_listener(std::shared_ptr<WALEventListener> listener) {
    impl_->remove_event_listener(std::move(listener));
}

std::vector<FileWAL::SegmentInfo> FileWAL::get_segment_info() const {
    return impl_->get_segment_info();
}

FileWAL::HealthStatus FileWAL::get_health_status() const {
    return impl_->get_health_status();
}

// ==================== 工厂函数 ====================

std::unique_ptr<WAL> create_file_wal(
    const std::string& log_dir,
    size_t buffer_size,
    bool sync_on_commit) {
    WALConfig cfg;
    cfg.log_dir = log_dir;
    cfg.buffer_size = buffer_size;
    cfg.sync_on_commit = sync_on_commit;
    return std::make_unique<FileWAL>(cfg);
}

std::unique_ptr<FileWAL> create_high_performance_wal(
    const WALConfig& config) {
    WALConfig cfg = config;
    return std::make_unique<FileWAL>(cfg);
}

std::unique_ptr<FileWAL> create_safe_wal(
    const WALConfig& config) {
    WALConfig cfg = config;
    cfg.sync_on_commit = true;
    cfg.enable_group_commit = false;
    return std::make_unique<FileWAL>(cfg);
}

// ==================== WALRecoveryTool ====================

bool WALRecoveryTool::validate_wal(const std::string& log_dir) {
    return std::filesystem::exists(log_dir);
}

bool WALRecoveryTool::repair_wal(const std::string& log_dir) {
    (void)log_dir;
    return false;
}

bool WALRecoveryTool::export_wal(const std::string& log_dir,
                                 const std::string& output_file) {
    std::string log_path = log_dir + "/wal.log";
    if (!std::filesystem::exists(log_path)) {
        return false;
    }
    std::error_code ec;
    std::filesystem::copy_file(log_path, output_file,
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    return !ec;
}

WALStatistics WALRecoveryTool::analyze_wal(const std::string& log_dir) {
    WALStatistics stats;
    std::string log_path = log_dir + "/wal.log";
    if (std::filesystem::exists(log_path)) {
        stats.total_bytes_written =
            static_cast<uint64_t>(std::filesystem::file_size(log_path));
    }
    return stats;
}

} // namespace transaction
} // namespace mementodb
