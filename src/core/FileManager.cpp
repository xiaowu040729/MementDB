// File: src/core/FileManager.cpp
// 文件管理器实现：提供底层文件I/O、内存映射、异步IO等功能

#include "FileManager.hpp"
#include "../utils/LoggingSystem/LogMacros.hpp"
#include <thread>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <limits>
#include <sstream>
#include <iomanip>
#include <vector>
#include <algorithm>
#ifndef _WIN32
#include <sys/sendfile.h>
#include <sys/uio.h>
#endif

namespace mementodb {
namespace core {

// ==================== FileHandle 实现 ====================

FileHandle::FileHandle(const std::string& path, OpenMode mode, const FileManagerConfig* config)
    : file_path_(path)
    , open_mode_(mode)
    , config_(config ? config : &default_config_)
#ifdef _WIN32
    , file_handle_(INVALID_HANDLE_VALUE)
    , mapping_handle_(nullptr)
#else
    , file_descriptor_(-1)
#endif
{
    open_file();
}

FileHandle::~FileHandle() {
    close_file();
    // 清理所有内存映射
    std::lock_guard<std::mutex> lock(map_mutex_);
    for (const auto& region : mapped_regions_) {
        unmap(region.address, region.size);
    }
    mapped_regions_.clear();
}

FileHandle::FileHandle(FileHandle&& other) noexcept
    : file_path_(std::move(other.file_path_))
    , open_mode_(other.open_mode_)
#ifdef _WIN32
    , file_handle_(other.file_handle_)
    , mapping_handle_(other.mapping_handle_)
#else
    , file_descriptor_(other.file_descriptor_)
#endif
    , mapped_regions_(std::move(other.mapped_regions_))
    , stats_(other.stats_)
    , config_(other.config_) {
    
#ifdef _WIN32
    other.file_handle_ = INVALID_HANDLE_VALUE;
    other.mapping_handle_ = nullptr;
#else
    other.file_descriptor_ = -1;
#endif
}

FileHandle& FileHandle::operator=(FileHandle&& other) noexcept {
    if (this != &other) {
        close_file();
        
        file_path_ = std::move(other.file_path_);
        open_mode_ = other.open_mode_;
#ifdef _WIN32
        file_handle_ = other.file_handle_;
        mapping_handle_ = other.mapping_handle_;
        other.file_handle_ = INVALID_HANDLE_VALUE;
        other.mapping_handle_ = nullptr;
#else
        file_descriptor_ = other.file_descriptor_;
        other.file_descriptor_ = -1;
#endif
        mapped_regions_ = std::move(other.mapped_regions_);
        stats_ = other.stats_;
        config_ = other.config_;
    }
    return *this;
}

void FileHandle::open_file() {
#ifdef _WIN32
    DWORD desired_access = get_desired_access();
    DWORD creation_disposition = get_creation_disposition();
    DWORD flags_and_attributes = get_flags_and_attributes();
    
    file_handle_ = CreateFileA(
        file_path_.c_str(),
        desired_access,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        creation_disposition,
        flags_and_attributes,
        nullptr
    );
    
    if (file_handle_ == INVALID_HANDLE_VALUE) {
        LOG_ERROR("FileManager", "Failed to open file: " + file_path_ + 
                 ", Error: " + std::to_string(GetLastError()));
        throw FileSystemError("Failed to open file: " + file_path_, GetLastError());
    }
#else
    int flags = get_native_flags();
    mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH; // 0644
    
    file_descriptor_ = open(file_path_.c_str(), flags, mode);
    
    if (file_descriptor_ < 0) {
        LOG_ERROR("FileManager", "Failed to open file: " + file_path_ + 
                 ", Error: " + std::to_string(errno));
        throw FileSystemError("Failed to open file: " + file_path_, errno);
    }
    
    // 设置文件描述符标志
    if (config_->use_direct_io) {
        // Linux 上直接IO需要对齐
        // 这里只设置标志，实际对齐由调用者保证
    }
#endif
    
    LOG_DEBUG("FileManager", "Opened file: " + file_path_);
}

void FileHandle::close_file() {
#ifdef _WIN32
    if (file_handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_handle_);
        file_handle_ = INVALID_HANDLE_VALUE;
    }
    if (mapping_handle_) {
        CloseHandle(mapping_handle_);
        mapping_handle_ = nullptr;
    }
#else
    if (file_descriptor_ >= 0) {
        close(file_descriptor_);
        file_descriptor_ = -1;
    }
#endif
}

#ifdef _WIN32
DWORD FileHandle::get_desired_access() const {
    switch (open_mode_) {
        case OpenMode::ReadOnly:
            return GENERIC_READ;
        case OpenMode::WriteOnly:
            return GENERIC_WRITE;
        case OpenMode::ReadWrite:
            return GENERIC_READ | GENERIC_WRITE;
        default:
            return GENERIC_READ | GENERIC_WRITE;
    }
}

DWORD FileHandle::get_creation_disposition() const {
    switch (open_mode_) {
        case OpenMode::Create:
            return CREATE_ALWAYS;
        case OpenMode::Truncate:
            return TRUNCATE_EXISTING;
        case OpenMode::Append:
            return OPEN_ALWAYS;
        default:
            return OPEN_EXISTING;
    }
}

DWORD FileHandle::get_flags_and_attributes() const {
    DWORD flags = FILE_ATTRIBUTE_NORMAL;
    
    if (config_->use_direct_io) {
        flags |= FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH;
    }
    if (config_->use_write_through) {
        flags |= FILE_FLAG_WRITE_THROUGH;
    }
    if (config_->use_async_io) {
        flags |= FILE_FLAG_OVERLAPPED;
    }
    
    return flags;
}
#else
int FileHandle::get_native_flags() const {
    int flags = 0;
    
    switch (open_mode_) {
        case OpenMode::ReadOnly:
            flags = O_RDONLY;
            break;
        case OpenMode::WriteOnly:
            flags = O_WRONLY;
            break;
        case OpenMode::ReadWrite:
            flags = O_RDWR;
            break;
        case OpenMode::Create:
            flags = O_RDWR | O_CREAT | O_TRUNC;
            break;
        case OpenMode::Truncate:
            flags = O_RDWR | O_TRUNC;
            break;
        case OpenMode::Append:
            flags = O_RDWR | O_APPEND;
            break;
    }
    
    if (config_->use_direct_io) {
        flags |= O_DIRECT;
    }
    if (config_->use_no_cache) {
        flags |= O_DIRECT; // O_DIRECT 会绕过缓存
    }
    
    return flags;
}
#endif

size_t FileHandle::read(void* buffer, size_t size) {
    if (!is_open()) {
        LOG_ERROR("FileManager", "File not open: " + file_path_);
        return 0;
    }
    
    auto start_time = std::chrono::steady_clock::now();
    size_t bytes_read = 0;
    
#ifdef _WIN32
    DWORD bytes_read_dword = 0;
    if (!ReadFile(file_handle_, buffer, static_cast<DWORD>(size), &bytes_read_dword, nullptr)) {
        LOG_ERROR("FileManager", "Read failed: " + file_path_ + 
                 ", Error: " + std::to_string(GetLastError()));
        return 0;
    }
    bytes_read = bytes_read_dword;
#else
    ssize_t result = ::read(file_descriptor_, buffer, size);
    if (result < 0) {
        LOG_ERROR("FileManager", "Read failed: " + file_path_ + 
                 ", Error: " + std::to_string(errno));
        return 0;
    }
    bytes_read = static_cast<size_t>(result);
#endif
    
    auto end_time = std::chrono::steady_clock::now();
    auto latency = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    update_read_stats(bytes_read, latency);
    
    return bytes_read;
}

size_t FileHandle::write(const void* buffer, size_t size) {
    if (!is_open()) {
        LOG_ERROR("FileManager", "File not open: " + file_path_);
        return 0;
    }
    
    auto start_time = std::chrono::steady_clock::now();
    size_t bytes_written = 0;
    
#ifdef _WIN32
    DWORD bytes_written_dword = 0;
    if (!WriteFile(file_handle_, buffer, static_cast<DWORD>(size), &bytes_written_dword, nullptr)) {
        LOG_ERROR("FileManager", "Write failed: " + file_path_ + 
                 ", Error: " + std::to_string(GetLastError()));
        return 0;
    }
    bytes_written = bytes_written_dword;
#else
    ssize_t result = ::write(file_descriptor_, buffer, size);
    if (result < 0) {
        LOG_ERROR("FileManager", "Write failed: " + file_path_ + 
                 ", Error: " + std::to_string(errno));
        return 0;
    }
    bytes_written = static_cast<size_t>(result);
#endif
    
    auto end_time = std::chrono::steady_clock::now();
    auto latency = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    update_write_stats(bytes_written, latency);
    
    return bytes_written;
}

size_t FileHandle::pread(void* buffer, size_t size, uint64_t offset) {
    if (!is_open()) {
        LOG_ERROR("FileManager", "File not open: " + file_path_);
        return 0;
    }
    
    auto start_time = std::chrono::steady_clock::now();
    size_t bytes_read = 0;
    
#ifdef _WIN32
    OVERLAPPED overlapped = {};
    overlapped.Offset = static_cast<DWORD>(offset);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
    
    DWORD bytes_read_dword = 0;
    if (!ReadFile(file_handle_, buffer, static_cast<DWORD>(size), &bytes_read_dword, &overlapped)) {
        DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            LOG_ERROR("FileManager", "Pread failed: " + file_path_ + 
                     ", Error: " + std::to_string(error));
            return 0;
        }
        // 等待IO完成
        if (!GetOverlappedResult(file_handle_, &overlapped, &bytes_read_dword, TRUE)) {
            LOG_ERROR("FileManager", "Pread GetOverlappedResult failed: " + file_path_ + 
                     ", Error: " + std::to_string(GetLastError()));
            return 0;
        }
    }
    bytes_read = bytes_read_dword;
#else
    ssize_t result = ::pread(file_descriptor_, buffer, size, static_cast<off_t>(offset));
    if (result < 0) {
        LOG_ERROR("FileManager", "Pread failed: " + file_path_ + 
                 ", Error: " + std::to_string(errno));
        return 0;
    }
    bytes_read = static_cast<size_t>(result);
#endif
    
    auto end_time = std::chrono::steady_clock::now();
    auto latency = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    update_read_stats(bytes_read, latency);
    
    return bytes_read;
}

size_t FileHandle::pwrite(const void* buffer, size_t size, uint64_t offset) {
    if (!is_open()) {
        LOG_ERROR("FileManager", "File not open: " + file_path_);
        return 0;
    }
    
    auto start_time = std::chrono::steady_clock::now();
    size_t bytes_written = 0;
    
#ifdef _WIN32
    OVERLAPPED overlapped = {};
    overlapped.Offset = static_cast<DWORD>(offset);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
    
    DWORD bytes_written_dword = 0;
    if (!WriteFile(file_handle_, buffer, static_cast<DWORD>(size), &bytes_written_dword, &overlapped)) {
        DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            LOG_ERROR("FileManager", "Pwrite failed: " + file_path_ + 
                     ", Error: " + std::to_string(error));
            return 0;
        }
        // 等待IO完成
        if (!GetOverlappedResult(file_handle_, &overlapped, &bytes_written_dword, TRUE)) {
            LOG_ERROR("FileManager", "Pwrite GetOverlappedResult failed: " + file_path_ + 
                     ", Error: " + std::to_string(GetLastError()));
            return 0;
        }
    }
    bytes_written = bytes_written_dword;
#else
    ssize_t result = ::pwrite(file_descriptor_, buffer, size, static_cast<off_t>(offset));
    if (result < 0) {
        LOG_ERROR("FileManager", "Pwrite failed: " + file_path_ + 
                 ", Error: " + std::to_string(errno));
        return 0;
    }
    bytes_written = static_cast<size_t>(result);
#endif
    
    auto end_time = std::chrono::steady_clock::now();
    auto latency = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    update_write_stats(bytes_written, latency);
    
    return bytes_written;
}

size_t FileHandle::readv(const std::vector<iovec>& iov) {
    if (!is_open()) {
        LOG_ERROR("FileManager", "File not open: " + file_path_);
        return 0;
    }
    
    size_t total_bytes = 0;
    
#ifdef _WIN32
    // Windows 需要逐个读取
    for (const auto& vec : iov) {
        size_t bytes = read(vec.iov_base, vec.iov_len);
        if (bytes == 0) break;
        total_bytes += bytes;
        if (bytes < vec.iov_len) break; // 部分读取
    }
#else
    ssize_t result = ::readv(file_descriptor_, iov.data(), static_cast<int>(iov.size()));
    if (result < 0) {
        LOG_ERROR("FileManager", "Readv failed: " + file_path_ + 
                 ", Error: " + std::to_string(errno));
        return 0;
    }
    total_bytes = static_cast<size_t>(result);
    
    auto start_time = std::chrono::steady_clock::now();
    auto end_time = std::chrono::steady_clock::now();
    auto latency = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    update_read_stats(total_bytes, latency);
#endif
    
    return total_bytes;
}

size_t FileHandle::writev(const std::vector<iovec>& iov) {
    if (!is_open()) {
        LOG_ERROR("FileManager", "File not open: " + file_path_);
        return 0;
    }
    
    size_t total_bytes = 0;
    
#ifdef _WIN32
    // Windows 需要逐个写入
    for (const auto& vec : iov) {
        size_t bytes = write(vec.iov_base, vec.iov_len);
        if (bytes == 0) break;
        total_bytes += bytes;
        if (bytes < vec.iov_len) break; // 部分写入
    }
#else
    ssize_t result = ::writev(file_descriptor_, iov.data(), static_cast<int>(iov.size()));
    if (result < 0) {
        LOG_ERROR("FileManager", "Writev failed: " + file_path_ + 
                 ", Error: " + std::to_string(errno));
        return 0;
    }
    total_bytes = static_cast<size_t>(result);
    
    auto start_time = std::chrono::steady_clock::now();
    auto end_time = std::chrono::steady_clock::now();
    auto latency = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    update_write_stats(total_bytes, latency);
#endif
    
    return total_bytes;
}

uint64_t FileHandle::seek(int64_t offset, SeekOrigin origin) {
    if (!is_open()) {
        LOG_ERROR("FileManager", "File not open: " + file_path_);
        return 0;
    }
    
#ifdef _WIN32
    DWORD move_method = FILE_BEGIN;
    switch (origin) {
        case SeekOrigin::Current:
            move_method = FILE_CURRENT;
            break;
        case SeekOrigin::End:
            move_method = FILE_END;
            break;
        default:
            break;
    }
    
    LARGE_INTEGER li_offset;
    li_offset.QuadPart = offset;
    LARGE_INTEGER li_new_pos;
    
    if (!SetFilePointerEx(file_handle_, li_offset, &li_new_pos, move_method)) {
        LOG_ERROR("FileManager", "Seek failed: " + file_path_ + 
                 ", Error: " + std::to_string(GetLastError()));
        return 0;
    }
    
    return static_cast<uint64_t>(li_new_pos.QuadPart);
#else
    int whence = SEEK_SET;
    switch (origin) {
        case SeekOrigin::Current:
            whence = SEEK_CUR;
            break;
        case SeekOrigin::End:
            whence = SEEK_END;
            break;
        default:
            break;
    }
    
    off_t result = lseek(file_descriptor_, static_cast<off_t>(offset), whence);
    if (result < 0) {
        LOG_ERROR("FileManager", "Seek failed: " + file_path_ + 
                 ", Error: " + std::to_string(errno));
        return 0;
    }
    
    return static_cast<uint64_t>(result);
#endif
}

uint64_t FileHandle::tell() {
    return seek(0, SeekOrigin::Current);
}

void FileHandle::rewind() {
    seek(0, SeekOrigin::Begin);
}

void FileHandle::flush() {
    if (!is_open()) {
        return;
    }
    
#ifdef _WIN32
    FlushFileBuffers(file_handle_);
#else
    fsync(file_descriptor_);
#endif
}

void FileHandle::sync() {
    if (!is_open()) {
        return;
    }
    
#ifdef _WIN32
    FlushFileBuffers(file_handle_);
#else
    fsync(file_descriptor_);
#endif
}

void FileHandle::truncate(uint64_t size) {
    if (!is_open()) {
        LOG_ERROR("FileManager", "File not open: " + file_path_);
        return;
    }
    
#ifdef _WIN32
    seek(static_cast<int64_t>(size), SeekOrigin::Begin);
    SetEndOfFile(file_handle_);
#else
    if (ftruncate(file_descriptor_, static_cast<off_t>(size)) < 0) {
        LOG_ERROR("FileManager", "Truncate failed: " + file_path_ + 
                 ", Error: " + std::to_string(errno));
    }
#endif
}

void FileHandle::allocate(uint64_t offset, uint64_t length) {
    if (!is_open()) {
        LOG_ERROR("FileManager", "File not open: " + file_path_);
        return;
    }
    
#ifdef _WIN32
    // Windows 预分配
    LARGE_INTEGER li_offset, li_length;
    li_offset.QuadPart = static_cast<LONGLONG>(offset);
    li_length.QuadPart = static_cast<LONGLONG>(length);
    
    if (!SetFilePointerEx(file_handle_, li_offset, nullptr, FILE_BEGIN)) {
        LOG_ERROR("FileManager", "Allocate SetFilePointerEx failed: " + file_path_);
        return;
    }
    
    if (!SetEndOfFile(file_handle_)) {
        LOG_ERROR("FileManager", "Allocate SetEndOfFile failed: " + file_path_);
        return;
    }
#else
#ifdef __linux__
    // Linux fallocate
    if (fallocate(file_descriptor_, FALLOC_FL_KEEP_SIZE, 
                  static_cast<off_t>(offset), static_cast<off_t>(length)) < 0) {
        // 如果 fallocate 不支持，使用 ftruncate
        if (errno == EOPNOTSUPP || errno == ENOSYS) {
            uint64_t current_size = this->size();
            uint64_t new_size = std::max(current_size, offset + length);
            truncate(new_size);
        } else {
            LOG_ERROR("FileManager", "Allocate failed: " + file_path_ + 
                     ", Error: " + std::to_string(errno));
        }
    }
#else
    // 其他平台使用 truncate
    uint64_t current_size = this->size();
    uint64_t new_size = std::max(current_size, offset + length);
    truncate(new_size);
#endif
#endif
}

void* FileHandle::map(uint64_t offset, size_t length) {
    if (!is_open()) {
        LOG_ERROR("FileManager", "File not open: " + file_path_);
        return nullptr;
    }
    
    if (!config_->use_memory_mapping) {
        LOG_WARN("FileManager", "Memory mapping disabled in config");
        return nullptr;
    }
    
    // 如果 length 为 0，映射整个文件
    if (length == 0) {
        length = static_cast<size_t>(this->size() - offset);
    }
    
    void* mapped_addr = nullptr;
    
#ifdef _WIN32
    if (!mapping_handle_) {
        mapping_handle_ = CreateFileMappingA(
            file_handle_,
            nullptr,
            PAGE_READWRITE,
            0,
            0,
            nullptr
        );
        
        if (!mapping_handle_) {
            LOG_ERROR("FileManager", "CreateFileMapping failed: " + file_path_ + 
                     ", Error: " + std::to_string(GetLastError()));
            return nullptr;
        }
    }
    
    DWORD access = FILE_MAP_READ | FILE_MAP_WRITE;
    mapped_addr = MapViewOfFile(
        mapping_handle_,
        access,
        static_cast<DWORD>(offset >> 32),
        static_cast<DWORD>(offset),
        length
    );
    
    if (!mapped_addr) {
        LOG_ERROR("FileManager", "MapViewOfFile failed: " + file_path_ + 
                 ", Error: " + std::to_string(GetLastError()));
        return nullptr;
    }
#else
    int prot = PROT_READ | PROT_WRITE;
    int flags = MAP_SHARED;
    
    mapped_addr = mmap(
        nullptr,
        length,
        prot,
        flags,
        file_descriptor_,
        static_cast<off_t>(offset)
    );
    
    if (mapped_addr == MAP_FAILED) {
        LOG_ERROR("FileManager", "Mmap failed: " + file_path_ + 
                 ", Error: " + std::to_string(errno));
        return nullptr;
    }
#endif
    
    // 记录映射区域
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        mapped_regions_.push_back({mapped_addr, length, offset});
    }
    
    LOG_DEBUG("FileManager", "Mapped " + std::to_string(length) + 
             " bytes at offset " + std::to_string(offset) + " for " + file_path_);
    
    return mapped_addr;
}

void FileHandle::unmap(void* addr, size_t length) {
    if (!addr) {
        return;
    }
    
#ifdef _WIN32
    UnmapViewOfFile(addr);
#else
    if (munmap(addr, length) < 0) {
        LOG_ERROR("FileManager", "Munmap failed: " + file_path_ + 
                 ", Error: " + std::to_string(errno));
    }
#endif
    
    // 从记录中移除
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        mapped_regions_.erase(
            std::remove_if(mapped_regions_.begin(), mapped_regions_.end(),
                [addr](const MappedRegion& region) {
                    return region.address == addr;
                }),
            mapped_regions_.end()
        );
    }
}

void FileHandle::sync_mapped(void* addr, size_t length, bool async) {
    if (!addr) {
        return;
    }
    
#ifdef _WIN32
    FlushViewOfFile(addr, length);
#else
    int flags = async ? MS_ASYNC : MS_SYNC;
    if (msync(addr, length, flags) < 0) {
        LOG_ERROR("FileManager", "Msync failed: " + file_path_ + 
                 ", Error: " + std::to_string(errno));
    }
#endif
}

FileHandle::FileInfo FileHandle::get_info() const {
    FileInfo info;
    info.path = file_path_;
    
    if (!is_open()) {
        info.size = 0;
        return info;
    }
    
#ifdef _WIN32
    LARGE_INTEGER file_size;
    if (GetFileSizeEx(file_handle_, &file_size)) {
        info.size = static_cast<uint64_t>(file_size.QuadPart);
    }
    
    FILETIME create_time, modify_time, access_time;
    if (GetFileTime(file_handle_, &create_time, &access_time, &modify_time)) {
        // 转换时间（简化版本）
        // 实际应该使用 FileTimeToSystemTime 和转换
    }
#else
    struct stat st;
    if (fstat(file_descriptor_, &st) == 0) {
        info.size = static_cast<uint64_t>(st.st_size);
        info.is_directory = S_ISDIR(st.st_mode);
        info.is_regular_file = S_ISREG(st.st_mode);
        info.is_symlink = S_ISLNK(st.st_mode);
        info.permissions = st.st_mode & 0777;
        
        // 转换时间
        info.create_time = std::chrono::system_clock::from_time_t(st.st_ctime);
        info.modify_time = std::chrono::system_clock::from_time_t(st.st_mtime);
        info.access_time = std::chrono::system_clock::from_time_t(st.st_atime);
    }
#endif
    
    return info;
}

uint64_t FileHandle::size() const {
    return get_info().size;
}

bool FileHandle::is_open() const {
#ifdef _WIN32
    return file_handle_ != INVALID_HANDLE_VALUE;
#else
    return file_descriptor_ >= 0;
#endif
}

bool FileHandle::lock_shared(uint64_t offset, uint64_t length, bool wait) {
    if (!is_open()) {
        return false;
    }
    
#ifdef _WIN32
    // Windows 文件锁实现
    OVERLAPPED overlapped = {};
    overlapped.Offset = static_cast<DWORD>(offset);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
    
    if (!LockFileEx(file_handle_, 0, wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY,
                     static_cast<DWORD>(length), static_cast<DWORD>(length >> 32), &overlapped)) {
        return false;
    }
    return true;
#else
    struct flock lock;
    lock.l_type = F_RDLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = static_cast<off_t>(offset);
    lock.l_len = static_cast<off_t>(length);
    
    int cmd = wait ? F_SETLKW : F_SETLK;
    if (fcntl(file_descriptor_, cmd, &lock) < 0) {
        return false;
    }
    return true;
#endif
}

bool FileHandle::lock_exclusive(uint64_t offset, uint64_t length, bool wait) {
    if (!is_open()) {
        return false;
    }
    
#ifdef _WIN32
    OVERLAPPED overlapped = {};
    overlapped.Offset = static_cast<DWORD>(offset);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
    
    if (!LockFileEx(file_handle_, LOCKFILE_EXCLUSIVE_LOCK, 
                     wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY,
                     static_cast<DWORD>(length), static_cast<DWORD>(length >> 32), &overlapped)) {
        return false;
    }
    return true;
#else
    struct flock lock;
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = static_cast<off_t>(offset);
    lock.l_len = static_cast<off_t>(length);
    
    int cmd = wait ? F_SETLKW : F_SETLK;
    if (fcntl(file_descriptor_, cmd, &lock) < 0) {
        return false;
    }
    return true;
#endif
}

bool FileHandle::unlock(uint64_t offset, uint64_t length) {
    if (!is_open()) {
        return false;
    }
    
#ifdef _WIN32
    OVERLAPPED overlapped = {};
    overlapped.Offset = static_cast<DWORD>(offset);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
    
    if (!UnlockFileEx(file_handle_, 0,
                       static_cast<DWORD>(length), static_cast<DWORD>(length >> 32), &overlapped)) {
        return false;
    }
    return true;
#else
    struct flock lock;
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = static_cast<off_t>(offset);
    lock.l_len = static_cast<off_t>(length);
    
    if (fcntl(file_descriptor_, F_SETLK, &lock) < 0) {
        return false;
    }
    return true;
#endif
}

FileHandle::IOStats FileHandle::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void FileHandle::reset_stats() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = IOStats{};
}

std::future<size_t> FileHandle::async_read(void* buffer, size_t size, uint64_t offset) {
    return std::async(std::launch::async, [this, buffer, size, offset]() {
        return pread(buffer, size, offset);
    });
}

std::future<size_t> FileHandle::async_write(const void* buffer, size_t size, uint64_t offset) {
    return std::async(std::launch::async, [this, buffer, size, offset]() {
        return pwrite(buffer, size, offset);
    });
}

size_t FileHandle::send_to(FileHandle& dest, uint64_t src_offset, uint64_t dest_offset, size_t size) {
    if (!is_open() || !dest.is_open()) {
        LOG_ERROR("FileManager", "File not open for send_to");
        return 0;
    }
    
#ifdef _WIN32
    // Windows 需要手动复制
    std::vector<char> buffer(size);
    size_t bytes_read = pread(buffer.data(), size, src_offset);
    if (bytes_read == 0) {
        return 0;
    }
    return dest.pwrite(buffer.data(), bytes_read, dest_offset);
#else
    off_t offset = static_cast<off_t>(src_offset);
    ssize_t result = sendfile(dest.file_descriptor_, file_descriptor_, 
                              &offset, size);
    if (result < 0) {
        LOG_ERROR("FileManager", "Sendfile failed: " + file_path_ + 
                 ", Error: " + std::to_string(errno));
        return 0;
    }
    return static_cast<size_t>(result);
#endif
}

void FileHandle::update_read_stats(size_t bytes, std::chrono::microseconds latency) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.read_count++;
    stats_.read_bytes += bytes;
    
    // 更新平均延迟（指数移动平均）
    if (stats_.read_count == 1) {
        stats_.avg_read_latency_us = latency.count();
    } else {
        stats_.avg_read_latency_us = 0.9 * stats_.avg_read_latency_us + 0.1 * latency.count();
    }
}

void FileHandle::update_write_stats(size_t bytes, std::chrono::microseconds latency) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.write_count++;
    stats_.write_bytes += bytes;
    
    // 更新平均延迟（指数移动平均）
    if (stats_.write_count == 1) {
        stats_.avg_write_latency_us = latency.count();
    } else {
        stats_.avg_write_latency_us = 0.9 * stats_.avg_write_latency_us + 0.1 * latency.count();
    }
}

// 静态默认配置
FileManagerConfig FileHandle::default_config_;

// ==================== FileHandle 扩展接口实现 ====================

std::future<std::vector<size_t>> FileHandle::async_read_batch(
    const std::vector<std::tuple<void*, size_t, uint64_t>>& requests) {
    return std::async(std::launch::async, [this, requests]() {
        std::vector<size_t> results;
        results.reserve(requests.size());
        
        for (const auto& [buffer, size, offset] : requests) {
            results.push_back(pread(buffer, size, offset));
        }
        
        return results;
    });
}

std::future<std::vector<size_t>> FileHandle::async_write_batch(
    const std::vector<std::tuple<const void*, size_t, uint64_t>>& requests) {
    return std::async(std::launch::async, [this, requests]() {
        std::vector<size_t> results;
        results.reserve(requests.size());
        
        for (const auto& [buffer, size, offset] : requests) {
            results.push_back(pwrite(buffer, size, offset));
        }
        
        return results;
    });
}

bool FileHandle::verify_checksum(uint64_t offset, size_t length, const std::string& expected_checksum) {
    std::string calculated = calculate_checksum(offset, length);
    return calculated == expected_checksum;
}

std::string FileHandle::calculate_checksum(uint64_t offset, size_t length) {
    // 简单的 CRC32 实现（实际应该使用更高效的库）
    std::vector<char> buffer(length);
    size_t bytes_read = pread(buffer.data(), length, offset);
    
    if (bytes_read != length) {
        LOG_ERROR("FileManager", "Failed to read data for checksum calculation");
        return "";
    }
    
    // 简单的哈希计算（实际应该使用 CRC32 或 MD5）
    uint32_t hash = 0;
    for (size_t i = 0; i < bytes_read; ++i) {
        hash = hash * 31 + static_cast<unsigned char>(buffer[i]);
    }
    
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(8) << hash;
    return oss.str();
}

// ==================== FileCacheManager 实现 ====================

FileCacheManager::FileCacheManager(size_t max_size, size_t max_open_files)
    : max_size_(max_size)
    , max_open_files_(max_open_files)
    , current_size_(0)
    , hit_count_(0)
    , miss_count_(0) {
}

std::shared_ptr<FileHandle> FileCacheManager::get_file(
    const std::string& path,
    FileHandle::OpenMode mode,
    const FileManagerConfig& config) {
    
    std::lock_guard<std::mutex> lock(cache_mutex_);
    
    // 查找缓存
    auto it = cache_map_.find(path);
    if (it != cache_map_.end()) {
        // 更新访问信息
        auto& entry = it->second;
        entry.last_access = std::chrono::steady_clock::now();
        entry.access_count++;
        hit_count_++;
        
        LOG_DEBUG("FileManager", "Cache hit: " + path);
        return entry.file_handle;
    }
    
    // 缓存未命中
    miss_count_++;
    
    // 检查是否超过最大文件数
    if (cache_map_.size() >= max_open_files_) {
        cleanup();
    }
    
    // 创建新文件句柄
    try {
        auto file_handle = std::make_shared<FileHandle>(path, mode, &config);
        if (!file_handle->is_open()) {
            LOG_ERROR("FileManager", "Failed to open file: " + path);
            return nullptr;
        }
        
        // 创建缓存条目
        size_t file_size = file_handle->size();
        CacheEntry entry(path, file_handle, file_size);
        
        // 检查缓存大小
        while (current_size_ + file_size > max_size_ && !cache_map_.empty()) {
            cleanup();
        }
        
        // 插入缓存
        cache_map_[path] = entry;
        current_size_ += file_size;
        
        LOG_DEBUG("FileManager", "Cached file: " + path + ", Size: " + std::to_string(file_size));
        
        return file_handle;
    } catch (const std::exception& e) {
        LOG_ERROR("FileManager", "Exception opening file: " + path + ", Error: " + e.what());
        return nullptr;
    }
}

void FileCacheManager::release_file(const std::string& path) {
    // 引用计数由 shared_ptr 管理，这里可以记录访问
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = cache_map_.find(path);
    if (it != cache_map_.end()) {
        it->second.last_access = std::chrono::steady_clock::now();
    }
}

bool FileCacheManager::evict_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    
    auto it = cache_map_.find(path);
    if (it != cache_map_.end()) {
        const auto& entry = it->second;
        if (entry.pinned) {
            LOG_WARN("FileManager", "Cannot evict pinned file: " + path);
            return false;
        }
        
        current_size_ -= entry.file_size;
        cache_map_.erase(it);
        
        LOG_DEBUG("FileManager", "Evicted file: " + path);
        return true;
    }
    
    return false;
}

void FileCacheManager::cleanup() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    
    if (cache_map_.empty()) {
        return;
    }
    
    // 使用 LRU-K 策略：找到得分最低的条目
    std::string victim_path;
    double min_score = std::numeric_limits<double>::max();
    
    for (const auto& [path, entry] : cache_map_) {
        if (entry.pinned) {
            continue; // 跳过 pinned 文件
        }
        
        double score = static_cast<double>(entry.access_count) / 
                      (1.0 + std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - entry.last_access).count());
        
        if (score < min_score) {
            min_score = score;
            victim_path = path;
        }
    }
    
    if (!victim_path.empty()) {
        evict_file(victim_path);
    }
}

FileCacheManager::CacheStats FileCacheManager::get_stats() const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    
    CacheStats stats;
    stats.total_entries = cache_map_.size();
    stats.total_size_bytes = current_size_;
    
    size_t pinned_count = 0;
    for (const auto& [path, entry] : cache_map_) {
        if (entry.pinned) {
            pinned_count++;
        }
    }
    stats.pinned_entries = pinned_count;
    
    stats.hit_count = hit_count_;
    stats.miss_count = miss_count_;
    
    size_t total_requests = hit_count_ + miss_count_;
    stats.hit_rate = total_requests > 0 ? 
                     static_cast<double>(hit_count_) / total_requests : 0.0;
    
    return stats;
}

void FileCacheManager::pin_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = cache_map_.find(path);
    if (it != cache_map_.end()) {
        it->second.pinned = true;
        LOG_DEBUG("FileManager", "Pinned file: " + path);
    }
}

void FileCacheManager::unpin_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = cache_map_.find(path);
    if (it != cache_map_.end()) {
        it->second.pinned = false;
        LOG_DEBUG("FileManager", "Unpinned file: " + path);
    }
}

void FileCacheManager::prefetch_file(const std::string& path, const FileManagerConfig& config) {
    // 预取：提前打开文件并加载到缓存
    get_file(path, FileHandle::OpenMode::ReadOnly, config);
}

std::vector<std::shared_ptr<FileHandle>> FileCacheManager::get_files_batch(
    const std::vector<std::pair<std::string, FileHandle::OpenMode>>& requests,
    const FileManagerConfig& config) {
    
    std::vector<std::shared_ptr<FileHandle>> results;
    results.reserve(requests.size());
    
    for (const auto& [path, mode] : requests) {
        auto handle = get_file(path, mode, config);
        results.push_back(handle);
    }
    
    return results;
}

void FileCacheManager::clear_cache() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cache_map_.clear();
    current_size_ = 0;
    LOG_DEBUG("FileManager", "Cache cleared");
}

size_t FileCacheManager::get_cache_size() const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    return current_size_;
}

} // namespace core
} // namespace mementodb
