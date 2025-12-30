// File: src/transaction/src/FileWAL.cpp
// 基于文件的 WAL 实现

#include "FileWAL.hpp"
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <chrono>

namespace mementodb {
namespace transaction {

// ==================== FileLogScanner 实现 ====================

FileLogScanner::FileLogScanner(const std::string& log_file_path)
    : log_file_path_(log_file_path), current_lsn_(0), file_offset_(0) {
}

FileLogScanner::~FileLogScanner() {
    if (file_.is_open()) {
        file_.close();
    }
}

bool FileLogScanner::open(uint64_t start_lsn) {
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
    file_offset_ = file_.tellg();
    return true;
}

bool FileLogScanner::next(std::vector<char>& record) {
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
    file_offset_ = file_.tellg();
    return true;
}

bool FileLogScanner::eof() const {
    return !file_.is_open() || file_.eof();
}

bool FileLogScanner::seek_to_lsn(uint64_t lsn) {
    // 简单实现：从文件开头顺序查找
    // TODO: 优化为索引查找
    file_.seekg(0, std::ios::beg);
    
    while (!file_.eof()) {
        uint64_t current_pos = file_.tellg();
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

bool FileLogScanner::read_log_header(LogHeader& header) {
    uint64_t pos = file_.tellg();
    file_.read(reinterpret_cast<char*>(&header), sizeof(LogHeader));
    
    if (file_.gcount() != sizeof(LogHeader)) {
        return false;
    }
    
    return true;
}

// ==================== FileWAL 实现 ====================

FileWAL::FileWAL(const std::string& log_dir, size_t buffer_size, bool sync_on_commit)
    : log_dir_(log_dir), buffer_size_(buffer_size), sync_on_commit_(sync_on_commit) {
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

FileWAL::~FileWAL() {
    flush();
    close_log_file();
    LOG_INFO("WAL", "FileWAL shutdown");
}

uint64_t FileWAL::log_begin(TransactionID tid) {
    return append_log_record(tid, LogType::BEGIN, nullptr, 0);
}

uint64_t FileWAL::log_commit(TransactionID tid) {
    uint64_t lsn = append_log_record(tid, LogType::COMMIT, nullptr, 0);
    if (sync_on_commit_) {
        flush();
    }
    return lsn;
}

uint64_t FileWAL::log_abort(TransactionID tid) {
    return append_log_record(tid, LogType::ABORT, nullptr, 0);
}

uint64_t FileWAL::log_physical_update(
    TransactionID tid,
    PageID page_id,
    uint32_t offset,
    const char* old_data,
    const char* new_data,
    uint32_t length) {
    
    // 构建物理日志记录
    struct {
        PhysicalLogRecord record;
        char redo_data[4096];  // 最大支持4KB数据
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
    
    size_t data_size = sizeof(PhysicalLogRecord) + length * 2;  // redo + undo
    return append_log_record(tid, LogType::PHYSICAL_MUTATION, 
                            &physical_record, data_size);
}

uint64_t FileWAL::write_checkpoint(const std::set<TransactionID>& active_txns) {
    struct {
        CheckpointRecord record;
        TransactionID active_txns[1024];  // 最大支持1024个活跃事务
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

std::vector<char> FileWAL::read_log_record(uint64_t lsn) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 简单实现：使用扫描器查找
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

std::unique_ptr<LogScanner> FileWAL::create_scanner() {
    return std::make_unique<FileLogScanner>(log_file_path_);
}

void FileWAL::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    flush_buffer();
    log_file_.flush();
    flushed_lsn_.store(current_lsn_.load());
    LOG_DEBUG("WAL", "Flushed WAL: FlushedLSN=" + std::to_string(flushed_lsn_.load()));
}

void FileWAL::truncate(uint64_t upto_lsn) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // TODO: 实现日志截断
    // 1. 创建新日志文件
    // 2. 复制 upto_lsn 之后的记录
    // 3. 删除旧文件
    // 4. 重命名新文件
    
    LOG_INFO("WAL", "Truncate WAL: UptoLSN=" + std::to_string(upto_lsn));
}

void FileWAL::ensure_directory(const std::string& path) {
    std::filesystem::create_directories(path);
}

std::string FileWAL::get_log_file_path() const {
    return log_dir_ + "/wal.log";
}

bool FileWAL::open_log_file() {
    log_file_.open(log_file_path_, std::ios::binary | std::ios::app);
    return log_file_.is_open();
}

void FileWAL::close_log_file() {
    if (log_file_.is_open()) {
        flush_buffer();
        log_file_.close();
    }
}

void FileWAL::write_to_buffer(const char* data, size_t size) {
    if (buffer_pos_ + size > buffer_size_) {
        flush_buffer();
    }
    
    std::memcpy(write_buffer_.data() + buffer_pos_, data, size);
    buffer_pos_ += size;
}

void FileWAL::flush_buffer() {
    if (buffer_pos_ > 0) {
        log_file_.write(write_buffer_.data(), buffer_pos_);
        buffer_pos_ = 0;
    }
}

uint64_t FileWAL::append_log_record(
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
    header.length = sizeof(LogHeader) + data_size;
    header.timestamp = get_current_timestamp();
    header.checksum = calculate_checksum(header, data, data_size);
    
    // 写入日志头
    write_to_buffer(reinterpret_cast<const char*>(&header), sizeof(LogHeader));
    
    // 写入数据
    if (data && data_size > 0) {
        write_to_buffer(static_cast<const char*>(data), data_size);
    }
    
    // 如果缓冲区满了，自动刷盘
    if (buffer_pos_ >= buffer_size_ * 0.8) {
        flush_buffer();
    }
    
    // 更新事务的上一条日志LSN
    update_prev_lsn(tid, lsn);
    
    LOG_DEBUG("WAL", "Appended log record: LSN=" + std::to_string(lsn) +
                     ", TID=" + std::to_string(tid) +
                     ", Type=" + std::to_string(static_cast<int>(type)));
    
    return lsn;
}

uint32_t FileWAL::calculate_checksum(const LogHeader& header, 
                                     const void* data, 
                                     size_t data_size) const {
    // 简单的校验和算法（FNV-1a）
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

uint64_t FileWAL::get_prev_lsn(TransactionID tid) {
    auto it = last_lsn_per_txn_.find(tid);
    return (it != last_lsn_per_txn_.end()) ? it->second : 0;
}

void FileWAL::update_prev_lsn(TransactionID tid, uint64_t lsn) {
    last_lsn_per_txn_[tid] = lsn;
}

uint64_t FileWAL::get_current_timestamp() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// ==================== 工厂函数 ====================

std::unique_ptr<WAL> create_file_wal(
    const std::string& log_dir,
    size_t buffer_size,
    bool sync_on_commit) {
    return std::make_unique<FileWAL>(log_dir, buffer_size, sync_on_commit);
}

} // namespace transaction
} // namespace mementodb

