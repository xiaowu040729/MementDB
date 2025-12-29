#ifndef FILE_MANAGER_HPP
#define FILE_MANAGER_HPP

#include <iostream>
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <map>
#include <set>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <chrono>
#include <functional>
#include <queue>
#include <future>
#include <optional>
// #include <span>  // C++20 feature, use alternative if not available
#include <filesystem>
#include <fstream>
#include <cstring>
#include <cstdint>
#include <system_error>
#include <algorithm>
#include <random>
#include <iomanip>
#include <sstream>

// 平台特定头文件
#ifdef _WIN32
    #include <windows.h>
    #include <fileapi.h>
    #include <winioctl.h>
    #include <io.h>
    #include <fcntl.h>
#else
    #include <unistd.h>
    #include <sys/types.h>
    #include <sys/stat.h>
    #include <sys/mman.h>
    #include <sys/ioctl.h>
    #include <fcntl.h>
    #include <dirent.h>
    #include <linux/fs.h>
    #ifdef __linux__
        #include <linux/falloc.h>
    #endif
#endif

namespace fs = std::filesystem;

namespace mementodb {
namespace core {

// ==================== 错误处理 ====================

class FileSystemError : public std::system_error {
public:
    explicit FileSystemError(const std::string& what, int err = 0)
        : std::system_error(err ? err : errno, std::system_category(), what) {}
};

// ==================== 配置结构 ====================

struct FileManagerConfig {
    // 文件打开选项
    bool use_direct_io = false;        // 使用直接IO（O_DIRECT）
    bool use_async_io = false;         // 使用异步IO
    bool use_memory_mapping = true;    // 使用内存映射
    bool use_write_through = false;    // 直写模式
    bool use_no_cache = false;         // 绕过系统缓存
    
    // 性能调优
    size_t max_open_files = 1024;      // 最大打开文件数
    size_t io_thread_count = 4;        // IO线程数
    size_t max_mapped_memory = 256 * 1024 * 1024; // 最大映射内存：256MB
    
    // 文件扩展策略
    size_t default_extent_size = 64 * 1024 * 1024; // 默认扩展大小：64MB
    size_t max_file_size = 4ULL * 1024 * 1024 * 1024 * 1024; // 最大4TB
    
    // 缓存策略
    bool enable_file_cache = true;     // 启用文件缓存
    size_t file_cache_size = 100;      // 文件缓存大小
    
    // 监控配置
    bool enable_metrics = true;        // 启用指标收集
    size_t metrics_interval_sec = 60;  // 指标收集间隔
    
    // 恢复配置
    bool enable_atomic_write = true;   // 启用原子写入
    bool enable_data_integrity = true; // 启用数据完整性检查
    std::string checksum_algorithm = "crc32"; // 校验算法
    
    // 压缩配置
    bool enable_compression = false;   // 启用压缩
    std::string compression_type = "lz4"; // 压缩类型
    
    // 加密配置
    bool enable_encryption = false;    // 启用加密
    std::string encryption_key;        // 加密密钥
};

// ==================== 文件句柄类 ====================

class FileHandle {
public:
    enum class OpenMode {
        ReadOnly,      // 只读
        WriteOnly,     // 只写
        ReadWrite,     // 读写
        Create,        // 创建新文件
        Truncate,      // 截断
        Append         // 追加
    };
    
    enum class SeekOrigin {
        Begin,        // 文件开头
        Current,      // 当前位置
        End           // 文件末尾
    };
    
    struct FileInfo {
        std::string path;
        uint64_t size;
        std::chrono::system_clock::time_point create_time;
        std::chrono::system_clock::time_point modify_time;
        std::chrono::system_clock::time_point access_time;
        uint32_t permissions;
        bool is_directory;
        bool is_regular_file;
        bool is_symlink;
    };
    
    struct IOStats {
        size_t read_count = 0;
        size_t write_count = 0;
        size_t read_bytes = 0;
        size_t write_bytes = 0;
        double avg_read_latency_us = 0;
        double avg_write_latency_us = 0;
        size_t cache_hits = 0;
        size_t cache_misses = 0;
    };
    
    explicit FileHandle(const std::string& path, 
                       OpenMode mode = OpenMode::ReadWrite,
                       const FileManagerConfig* config = nullptr);
    
    ~FileHandle();
    
    // 禁止拷贝
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    
    // 允许移动
    FileHandle(FileHandle&& other) noexcept;
    FileHandle& operator=(FileHandle&& other) noexcept;
    
    // 基础操作
    size_t read(void* buffer, size_t size);
    size_t write(const void* buffer, size_t size);
    size_t pread(void* buffer, size_t size, uint64_t offset);
    size_t pwrite(const void* buffer, size_t size, uint64_t offset);
    
    // 向量IO（scatter/gather）
    size_t readv(const std::vector<iovec>& iov);
    size_t writev(const std::vector<iovec>& iov);
    
    // 文件指针操作
    uint64_t seek(int64_t offset, SeekOrigin origin = SeekOrigin::Current);
    uint64_t tell();
    void rewind();
    
    // 文件控制
    void flush();
    void sync();
    void truncate(uint64_t size);
    void allocate(uint64_t offset, uint64_t length);
    
    // 内存映射
    void* map(uint64_t offset = 0, size_t length = 0);
    void unmap(void* addr, size_t length);
    void sync_mapped(void* addr, size_t length, bool async = false);
    
    // 文件信息
    FileInfo get_info() const;
    uint64_t size() const;
    bool is_open() const;
    const std::string& path() const { return file_path_; }
    
    // 锁操作
    bool lock_shared(uint64_t offset = 0, uint64_t length = 0, bool wait = true);
    bool lock_exclusive(uint64_t offset = 0, uint64_t length = 0, bool wait = true);
    bool unlock(uint64_t offset = 0, uint64_t length = 0);
    
    // 统计信息
    IOStats get_stats() const;
    void reset_stats();
    
    // 异步操作
    std::future<size_t> async_read(void* buffer, size_t size, uint64_t offset);
    std::future<size_t> async_write(const void* buffer, size_t size, uint64_t offset);
    
    // 零拷贝传输（sendfile）
    size_t send_to(FileHandle& dest, uint64_t src_offset, uint64_t dest_offset, size_t size);
    
    // 扩展接口：批量操作
    std::future<std::vector<size_t>> async_read_batch(
        const std::vector<std::tuple<void*, size_t, uint64_t>>& requests);
    std::future<std::vector<size_t>> async_write_batch(
        const std::vector<std::tuple<const void*, size_t, uint64_t>>& requests);
    
    // 扩展接口：文件完整性检查
    bool verify_checksum(uint64_t offset, size_t length, const std::string& expected_checksum);
    std::string calculate_checksum(uint64_t offset, size_t length);
    
private:
    std::string file_path_;
    OpenMode open_mode_;
    
#ifdef _WIN32
    HANDLE file_handle_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_handle_ = nullptr;
#else
    int file_descriptor_ = -1;
#endif
    
    // 内存映射相关
    struct MappedRegion {
        void* address;
        size_t size;
        uint64_t offset;
    };
    std::vector<MappedRegion> mapped_regions_;
    mutable std::mutex map_mutex_;
    
    // 统计信息
    mutable IOStats stats_;
    mutable std::mutex stats_mutex_;
    
    // 配置
    const FileManagerConfig* config_;
    static FileManagerConfig default_config_;
    
    // 内部方法
    void open_file();
    void close_file();
    int get_native_flags() const;
    
#ifdef _WIN32
    DWORD get_desired_access() const;
    DWORD get_creation_disposition() const;
    DWORD get_flags_and_attributes() const;
#endif
    
    void update_read_stats(size_t bytes, std::chrono::microseconds latency);
    void update_write_stats(size_t bytes, std::chrono::microseconds latency);
};

// ==================== 文件缓存管理器 ====================

class FileCacheManager {
private:
    struct CacheEntry {
        std::string file_path;
        std::shared_ptr<FileHandle> file_handle;
        std::chrono::steady_clock::time_point last_access;
        size_t access_count;
        size_t file_size;
        bool pinned;
        
        CacheEntry() = default;
        
        CacheEntry(const std::string& path, 
                   std::shared_ptr<FileHandle> handle, 
                   size_t size)
            : file_path(path)
            , file_handle(std::move(handle))
            , last_access(std::chrono::steady_clock::now())
            , access_count(1)
            , file_size(size)
            , pinned(false) {}
        
        bool operator<(const CacheEntry& other) const {
            if (pinned != other.pinned) {
                return pinned > other.pinned; // pinned entries first
            }
            // LRU-K算法：考虑访问频率和时间
            double score1 = static_cast<double>(access_count) / 
                           (1.0 + std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - last_access).count());
            double score2 = static_cast<double>(other.access_count) / 
                           (1.0 + std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - other.last_access).count());
            return score1 < score2;
        }
    };
    
public:
    explicit FileCacheManager(size_t max_size, size_t max_open_files);
    
    ~FileCacheManager() = default;
    
    // 获取文件句柄（如果缓存中不存在则创建）
    std::shared_ptr<FileHandle> get_file(const std::string& path, 
                                         FileHandle::OpenMode mode,
                                         const FileManagerConfig& config);
    
    // 释放文件句柄（引用计数减一）
    void release_file(const std::string& path);
    
    // 从缓存中移除文件
    bool evict_file(const std::string& path);
    
    // 清理缓存（根据策略）
    void cleanup();
    
    // 获取缓存统计信息
    struct CacheStats {
        size_t total_entries;
        size_t pinned_entries;
        size_t total_size_bytes;
        size_t hit_count;
        size_t miss_count;
        double hit_rate;
    };
    
    CacheStats get_stats() const;
    
    // 设置文件为pinned（防止被换出）
    void pin_file(const std::string& path);
    void unpin_file(const std::string& path);
    
    // 预取文件
    void prefetch_file(const std::string& path, 
                      const FileManagerConfig& config);
    
    // 扩展接口：批量操作
    std::vector<std::shared_ptr<FileHandle>> get_files_batch(
        const std::vector<std::pair<std::string, FileHandle::OpenMode>>& requests,
        const FileManagerConfig& config);
    
    // 扩展接口：清理所有缓存
    void clear_cache();
    
    // 扩展接口：获取缓存大小
    size_t get_cache_size() const;
    
private:
    size_t max_size_;
    size_t max_open_files_;
    size_t current_size_;
    std::unordered_map<std::string, CacheEntry> cache_map_;
    mutable std::mutex cache_mutex_;
    mutable std::atomic<size_t> hit_count_{0};
    mutable std::atomic<size_t> miss_count_{0};
};

} // namespace core
} // namespace mementodb

#endif // FILE_MANAGER_HPP