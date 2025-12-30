#ifndef DISK_ENGINE_V2_HPP
#define DISK_ENGINE_V2_HPP

#include <atomic>
#include <memory>
#include <vector>
#include <string>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <queue>
#include <future>
#include <unordered_map>
// #include <span>  // C++20 feature, use alternative if not available
#include <bit>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include "../memory/WALBase.hpp"
#include <sys/stat.h>
#include <aio.h>
// #include <xxhash.h>  // Optional: for fast hashing, can be disabled if not available

#include "Page.hpp"  // 使用已有的Page实现
#include "FileManager.hpp"  // 使用FileManager处理文件IO
#include "../utils/LoggingSystem/LogMacros.hpp"  // 日志系统
#include "BPlusTree.hpp"  // B+树索引
#include "Record.hpp"  // 键值对编码

namespace mementodb {
namespace core {

// ==================== 配置系统 ====================

struct EngineConfig {
    // 存储配置
    size_t page_size = 4096;               // 页大小（4KB对齐）
    size_t extent_size = 64 * 1024 * 1024; // 区大小（64MB）
    size_t max_file_size = 4ULL * 1024 * 1024 * 1024 * 1024; // 最大4TB
    
    // 缓冲池配置
    size_t buffer_pool_size = 1024 * 1024;  // 1M页（4GB）
    size_t hot_pool_size = 1024 * 16;       // 热数据池（64MB）
    
    // 性能配置
    bool use_direct_io = true;             // 直接IO
    bool use_mmap = true;                  // 内存映射
    bool use_io_uring = true;              // io_uring异步IO
    bool use_zero_copy = true;             // 零拷贝传输
    
    // 刷盘策略
    enum class FlushStrategy {
        Lazy,           // 惰性刷盘
        Periodic,       // 定期刷盘
        WriteThrough,   // 直写
        WriteBack       // 回写
    } flush_strategy = FlushStrategy::WriteBack;
    
    size_t flush_interval_ms = 1000;       // 刷盘间隔
    size_t dirty_page_threshold = 10000;   // 脏页阈值
    
    // 恢复配置
    bool enable_wal = true;
    size_t wal_buffer_size = 64 * 1024 * 1024; // 64MB
    size_t wal_segment_size = 256 * 1024 * 1024; // 256MB
    
    // 压缩配置-
    bool enable_compression = false;
    enum class CompressionType {
        None,
        LZ4,
        ZSTD,
        Snappy
    } compression_type = CompressionType::LZ4;
    
    // 加密配置
    bool enable_encryption = false;
    std::string encryption_key;
    
    // 监控配置
    bool enable_metrics = true;
    size_t metrics_interval_sec = 60;
};

// ==================== 自定义分配器 ====================

// 对齐内存分配器，用于分配对齐的内存（如页对齐）。
template<typename T>
class AlignedAllocator {
public:
    using value_type = T;
    using size_type = size_t;
    using difference_type = ptrdiff_t;
    
    template<typename U>
    struct rebind {
        using other = AlignedAllocator<U>;
    };
    
    AlignedAllocator() = default;
    
    template<typename U>
    AlignedAllocator(const AlignedAllocator<U>&) noexcept {}
    
    T* allocate(size_t n) {
        if (n > std::numeric_limits<size_t>::max() / sizeof(T)) {
            throw std::bad_array_new_length();
        }
        
        void* ptr = nullptr;
#ifdef _WIN32
        ptr = _aligned_malloc(n * sizeof(T), 4096);
        if (!ptr) throw std::bad_alloc();
#else
        if (posix_memalign(&ptr, 4096, n * sizeof(T)) != 0) {
            throw std::bad_alloc();
        }
#endif
        return static_cast<T*>(ptr);
    }
    
    void deallocate(T* ptr, size_t) noexcept {
#ifdef _WIN32
        _aligned_free(ptr);
#else
        free(ptr);
#endif
    }
};

// ==================== 页结构适配 ====================

using DiskPage = Page;  // 类型别名，方便后续如果需要修改


namespace disk_page_adapter {
    /**
     * to_page_id - 将 page_id 转换为 Page 构造函数需要的类型
     * 
     * @param id 页ID（uint64_t）
     * @return 页ID（uint64_t）
     * - 保留此函数是为了接口一致性，便于将来扩展
     */
    inline uint64_t to_page_id(uint64_t id) {
        return id;  // 直接返回，无需转换
    }
    
    /**
     * get_data - 获取页数据区的指针（非const版本）
     * 
     * @param page 页对象引用
     * @return 指向数据区的指针（可修改）
     */
    inline char* get_data(Page& page) {
        return page.GetData();
    }
    
    /**
     * get_data - 获取页数据区的指针（const版本）
     * 
     * @param page 页对象常量引用
     * @return 指向数据区的常量指针（只读）
     */
    inline const char* get_data(const Page& page) {
        return page.GetData();
    }
    
    /**
     * get_page_id - 获取页ID
     * 
     * @param page 页对象常量引用
     * @return 页ID（uint64_t）
     */
    inline uint64_t get_page_id(const Page& page) {
        return page.GetPageId();  // 直接返回，无需转换
    }
}

// ==================== 扩展文件管理器（使用FileManager）====================
class ExtentManager {
public:
    struct Extent {
        uint64_t start_page;
        uint64_t page_count;
        bool allocated;
        uint32_t file_id;
        std::atomic<uint64_t> ref_count{0};
        
        Extent(uint64_t start, uint64_t count, uint32_t fid)
            : start_page(start), page_count(count), file_id(fid), allocated(false) {}
        
        // 禁止移动和拷贝（因为包含 atomic）
        Extent(const Extent&) = delete;
        Extent& operator=(const Extent&) = delete;
        Extent(Extent&&) = delete;
        Extent& operator=(Extent&&) = delete;
        
        bool contains(uint64_t page_id) const {
            return page_id >= start_page && 
                   page_id < start_page + page_count;
        }
    };
    
    ExtentManager(const std::string& base_path, size_t extent_size, size_t page_size,
                  const FileManagerConfig& file_config);
    
    ~ExtentManager();
    
    /**
     * allocate_extent - 分配新扩展
     * 
     * @param page_count 页数
     * @return Extent* 扩展指针
     */
    Extent* allocate_extent(size_t page_count);

    
    /**
     * free_extent - 释放扩展
     * 
     * @param file_id 文件ID
     * @return bool 是否成功
     */
    bool free_extent(uint32_t file_id);
    
    /**
     * find_extent - 查找包含指定页的扩展
     * 
     * @param page_id 页ID
     * @return Extent* 扩展指针
     */
    Extent* find_extent(uint64_t page_id);
    
    /**
     * get_file_handle - 获取文件句柄（使用FileManager）
     * 
     * @param file_id 文件ID
     * @return std::shared_ptr<FileHandle> 文件句柄
     */
    std::shared_ptr<FileHandle> get_file_handle(uint32_t file_id);
    
    /**
     * get_stats - 获取扩展统计信息
     * 
     * @return ExtentStats 扩展统计信息
     */
    struct ExtentStats {
        size_t total_extents;
        size_t allocated_extents;
        size_t total_pages;
        size_t allocated_pages;
        size_t fragmentation; // 百分比
    };
    
    ExtentStats get_stats() const;
    
private:
    std::string base_path_;
    size_t extent_size_;
    size_t page_size_;
    FileManagerConfig file_config_;
    
    std::vector<std::unique_ptr<Extent>> extents_;
    uint64_t total_pages_{0};
    uint32_t next_extent_id_;
    mutable std::mutex extents_mutex_;
    
    // 使用FileHandle缓存
    std::unordered_map<uint32_t, std::shared_ptr<FileHandle>> file_cache_;
    mutable std::mutex file_cache_mutex_;
    
    static void ensure_directory(const std::string& path);
    std::string get_extent_path(uint32_t file_id) const;
    void load_extent_table();
    void save_extent_table();
};

// ==================== 智能缓冲池 ====================

class SmartBufferPool {
public:
    /**
     * BufferFrame - 缓冲池帧结构
     */
    struct BufferFrame {
        DiskPage page;                              // 页数据（4KB，固定大小）
        std::atomic<uint64_t> access_time{0};       // 最后访问时间（纳秒时间戳）
        std::atomic<uint32_t> access_count{0};      // 访问次数（用于CLOCK算法）
        std::atomic<uint32_t> pin_count{0};         // 引用计数（Pin计数）
        std::atomic<bool> dirty{false};             // 脏页标记（是否已修改）
        std::atomic<bool> locked{false};            // 独占锁标记（并发控制）
        
        // 禁止移动和拷贝（因为包含 atomic 和禁止拷贝的 Page）
        BufferFrame(const BufferFrame&) = delete;
        BufferFrame& operator=(const BufferFrame&) = delete;
        BufferFrame(BufferFrame&&) = delete;
        BufferFrame& operator=(BufferFrame&&) = delete;
        
        /**
         * 构造函数
         * @param page_size 页大小（当前版本固定4KB，此参数保留用于兼容）
         * 
         * 说明：
         * - Page 是固定 4KB 大小，page_size 参数实际不使用
         * - 使用 (void)page_size 避免编译器警告
         */
        BufferFrame(size_t page_size) : page() {
            // 已有Page是固定4KB，不需要page_size参数
            (void)page_size;  // 避免未使用参数警告
        }
        
        /**
         * pin - 固定页（增加引用计数）
         */
        void pin();
        
        /**
         * unpin - 释放固定（减少引用计数）
         */
        void unpin();
        
        /**
         * mark_accessed - 标记页被访问
         */
        void mark_accessed();
    };
    
    SmartBufferPool(size_t pool_size, size_t page_size, 
                   EngineConfig::FlushStrategy strategy);
    ~SmartBufferPool();
    
    // 获取页
    BufferFrame* fetch_page(uint64_t page_id, bool exclusive = false);

    
    // 创建新页
    BufferFrame* new_page();
    
    // 释放页
    void release_page(uint64_t page_id, bool mark_dirty = false);
    
    // 预取页
    void prefetch_pages(const std::vector<uint64_t>& page_ids);
    
    // 获取统计信息
    struct PoolStats {
        size_t total_frames;
        size_t used_frames;
        size_t free_frames;
        size_t dirty_frames;
        size_t pinned_frames;
        double hit_ratio;
        size_t prefetch_count;
    };
    
    PoolStats get_stats() const;
    
private:
    size_t pool_size_;
    size_t page_size_;
    EngineConfig::FlushStrategy flush_strategy_;
    
    std::vector<std::unique_ptr<BufferFrame>> frames_;
    std::vector<size_t> free_list_;
    
    // 页表：page_id -> frame_id
    std::unordered_map<uint64_t, size_t> page_table_;
    mutable std::shared_mutex page_table_mutex_;
    
    // 置换算法相关
    std::vector<size_t> lru_list_;
    std::vector<size_t> clock_hand_;
    
    // 预取队列（使用标准库）
    std::queue<uint64_t> prefetch_queue_;
    mutable std::mutex prefetch_queue_mutex_;
    
    // 后台线程
    std::atomic<bool> running_{false};
    std::vector<std::thread> background_threads_;
    
    // 统计信息
    std::atomic<size_t> hit_count_{0};
    std::atomic<size_t> miss_count_{0};
    std::atomic<size_t> prefetch_count_{0};
    
    // 获取空闲帧
    size_t get_free_frame();
    
    // 选择牺牲页（自适应策略）
    size_t select_victim();
    
    // CLOCK算法
    size_t select_by_clock();
    
    // LRU算法
    size_t select_by_lru();
    
    // 驱逐帧
    void evict_frame(size_t frame_id);
    
    // 加载页
    BufferFrame* load_page(uint64_t page_id, bool exclusive);
    
    // 刷帧到磁盘
    void flush_frame(size_t frame_id);
    
public:
    // 刷所有脏页（供外部调用）
    void flush_all();
    
private:
    
    // 后台线程
    void start_background_threads();
    void stop_background_threads();
    void flush_thread();
    void prefetch_thread();
    void stats_thread();
    void flush_dirty_pages();
    
    static uint64_t get_current_time();
    void log_stats(const PoolStats& stats);
    
    // 配置参数
    size_t flush_interval_ = 1000; // 1秒
    uint64_t min_dirty_age_ = 5 * 1000 * 1000 * 1000ULL; // 5秒
};

// ==================== 分布式WAL ====================

// 定义 LogRecord 结构（在类外部，以便作为模板参数）
struct DistributedWALLogRecord {
    uint64_t lsn;
    uint64_t timestamp;
    uint32_t node_id;
    uint32_t term; // Raft任期
    std::vector<char> data;
    std::function<void(bool)> callback;
    
    DistributedWALLogRecord(uint64_t l, uint32_t nid, uint32_t t, std::vector<char> d)
        : lsn(l), node_id(nid), term(t), data(std::move(d)) {
        // 使用标准库获取时间戳
        timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
};

class DistributedWAL : public memory::WALBase<DistributedWALLogRecord> {
public:
    // 使用外部定义的 LogRecord 类型
    using LogRecord = DistributedWALLogRecord;
    
    DistributedWAL(const std::string& data_dir, size_t node_id,
                   const std::vector<std::string>& peers);
    ~DistributedWAL();
    
    // 追加日志记录
    uint64_t append(const std::vector<char>& data);
    
    // 读取日志记录
    std::optional<LogRecord> read(uint64_t lsn);
    
    // 获取提交位置
    uint64_t get_commit_index() const;
    
    // 等待日志提交
    bool wait_for_commit(uint64_t lsn, std::chrono::milliseconds timeout);
    
    // 获取集群状态
    struct ClusterStatus {
        uint32_t leader_id;
        uint32_t current_term;
        uint64_t commit_index;
        size_t node_count;
        std::vector<bool> node_online;
    };
    
    ClusterStatus get_cluster_status() const;
    
private:
    size_t node_id_;
    std::vector<std::string> peers_;
    
    std::atomic<uint32_t> current_term_{0};
    std::atomic<uint32_t> leader_id_{0};
    std::atomic<uint64_t> commit_index_{0};
    std::atomic<uint64_t> last_applied_{0};
    
    std::vector<LogRecord> log_entries_;
    std::unordered_map<uint64_t, std::promise<bool>> commit_promises_;
    
    mutable std::mutex wal_mutex_;
    
    std::vector<bool> peers_connected_;
    mutable std::mutex peers_mutex_;
    
    // Raft状态
    enum class NodeState {
        Follower,
        Candidate,
        Leader
    };
    
    std::atomic<NodeState> node_state_{NodeState::Follower};
    std::atomic<uint64_t> last_heartbeat_{0};
    
    // 后台线程
    std::atomic<bool> running_{false};
    std::thread consensus_thread_;
    std::thread replication_thread_;
    
    // Raft共识线程
    void start_consensus_thread();
    void stop_consensus_thread();
    void consensus_loop();

    // Raft复制线程
    void replication_loop();

    // Raft选举
    void start_election();
        
    // Raft心跳
    void request_votes();
    void send_heartbeats();

    // Raft复制
    void replicate_pending_logs();

    // Raft持久化
    void persist_record(const LogRecord& record);

    // Raft读取
    std::optional<LogRecord> read_record(uint64_t lsn);

    // Raft复制
    void replicate_record(const LogRecord& record);
    void wait_for_quorum(uint64_t lsn);

    // Raft加载（重写基类方法以支持额外的 term 状态）
    void load_wal_state();

    // Raft持久化（重写基类方法以支持额外的 term 状态）
    void persist_wal_state();
    
    // Raft配置
    uint64_t election_timeout_ = 150; // 150ms
    uint64_t heartbeat_interval_ = 50; // 50ms
};

// ==================== 主引擎类 ====================

class DiskEngineV2 {
public:
    DiskEngineV2(const std::string& data_dir, const EngineConfig& config = EngineConfig{});
    ~DiskEngineV2();
    
    /**
     * read_page_async - 异步读取页
     * 
     * @param page_id 要读取的页ID（uint64_t）
     * @return std::future<DiskPage> 异步返回的页对象
     */
    std::future<DiskPage> read_page_async(uint64_t page_id);
        
    /**
     * write_page_async - 异步写入页
     * 
     * @param page 要写入的页对象（const DiskPage&）
     * @return std::future<bool> 异步返回写入是否成功
     */
    std::future<bool> write_page_async(const DiskPage& page);
    
    // ==================== 键值对操作（使用B+树索引）====================
    
    /**
     * put - 插入或更新键值对
     * 
     * @param key 键（Slice）
     * @param value 值（Slice）
     * @return std::future<bool> 异步返回是否成功
     */
    std::future<bool> put(const Slice& key, const Slice& value);
    
    /**
     * get - 查找键值对
     * 
     * @param key 键（Slice）
     * @return std::future<std::optional<Slice>> 异步返回值（如果存在）
     */
    std::future<std::optional<Slice>> get(const Slice& key);
    
    /**
     * remove - 删除键值对
     * 
     * @param key 键（Slice）
     * @return std::future<bool> 异步返回是否成功
     */
    std::future<bool> remove(const Slice& key);
    
    /**
     * range_query - 范围查询
     * 
     * @param start_key 起始键（包含）
     * @param end_key 结束键（包含）
     * @param callback 回调函数，对每个键值对调用
     * @return std::future<void> 异步完成
     */
    std::future<void> range_query(const Slice& start_key, const Slice& end_key,
                                  std::function<void(const Slice&, const Slice&)> callback);
    
    // 批量操作上下文（前向声明）
    class BatchContext;
        
    /**
     * batch_operation - 批量操作
     * 
     * @param operation 批量操作函数
     * @return std::future<bool> 异步返回是否成功
     */
    std::future<bool> batch_operation(
        const std::function<void(BatchContext&)>& operation);
    
    /**
     * compact_storage - 压缩存储
     * 
     * @return std::future<bool> 异步返回是否成功
     */
    std::future<bool> compact_storage();
    
    /**
     * backup - 备份
     * 
     * @param backup_dir 备份目录
     * @return std::future<bool> 异步返回是否成功
     */
    std::future<bool> backup(const std::string& backup_dir);
    
    /**
     * restore - 恢复
     * 
     * @param backup_dir 备份目录
     * @return std::future<bool> 异步返回是否成功
     */
    std::future<bool> restore(const std::string& backup_dir);
    
    /**
     * get_status - 获取引擎状态
     * 
     * @return EngineStatus 引擎状态
     */
    struct EngineStatus {
        SmartBufferPool::PoolStats buffer_pool_stats; // 缓冲池统计信息
        ExtentManager::ExtentStats extent_stats; // 扩展统计信息
        FileHandle::IOStats io_stats;  // 使用FileHandle的IOStats
        DistributedWAL::ClusterStatus cluster_status; // 集群状态
        size_t total_pages; // 总页数
        size_t used_pages; // 已使用页数
        double compression_ratio; // 压缩比
    };
    
    EngineStatus get_status() const;
    
    /**
     * set_metrics_callback - 设置监控回调
     * 
     * @param callback 监控回调函数
     * @param interval 监控间隔
     */
    using MetricsCallback = std::function<void(const EngineStatus&)>;
    void set_metrics_callback(MetricsCallback callback, 
                             std::chrono::seconds interval);
    
    // 批量操作上下文（需要在 public 部分定义，以便外部使用）
    class BatchContext {
    public:
        explicit BatchContext(DiskEngineV2& engine);
        void write_page(const DiskPage& page);
        void read_page(uint64_t page_id, 
                      std::function<void(const DiskPage&)> callback);
        bool commit();
        
    private:
        DiskEngineV2& engine_;
        std::vector<std::unique_ptr<DiskPage>> pending_writes_;
        std::vector<std::pair<uint64_t, std::function<void(const DiskPage&)>>> pending_reads_;
    };
    
private:
    EngineConfig config_;
    std::string data_dir_;
    FileManagerConfig file_config_;
    
    std::unique_ptr<ExtentManager> extent_manager_;
    std::unique_ptr<SmartBufferPool> buffer_pool_;
    std::unique_ptr<DistributedWAL> wal_;
    
    // B+树索引（键值对存储）
    // Key: Slice (作为字符串存储)
    // Value: uint64_t (页ID，指向实际数据页)
    std::unique_ptr<BPlusTree::BPlusTree<std::string, uint64_t>> index_tree_;
    mutable std::shared_mutex index_mutex_;  // 索引读写锁
    
    // 数据页管理（存储实际键值对数据）
    uint64_t allocate_data_page();  // 分配新的数据页
    bool store_record_in_page(uint64_t page_id, const Slice& key, const Slice& value);
    std::optional<std::pair<Slice, Slice>> load_record_from_page(uint64_t page_id, const Slice& key);
    
    // 监控
    MetricsCallback metrics_callback_;
    std::chrono::seconds metrics_interval_{60};
    std::thread metrics_thread_;
    std::atomic<bool> metrics_running_{false};
    
    /*
     * initialize_engine - 初始化引擎
     * 
     * @return void
     */
    void initialize_engine();
    /*
     * shutdown - 关闭引擎
     * 
     * @return void
     */
    void shutdown();
    /*
     * load_metadata - 加载引擎元数据
     * 
     * @return void
     */
    void load_metadata();
    /*
     * recover_from_wal - 从WAL恢复
     * 
     * @return void
     */
    void recover_from_wal();
    /*
     * perform_compaction - 执行压缩
     * 
     * @return bool
     */
    bool perform_compaction();
    /*
     * perform_backup - 执行备份
     * 
     * @param backup_dir 备份目录
     * @return bool
     */
    bool perform_backup(const std::string& backup_dir);
    /*
     * perform_restore - 执行恢复
     * 
     * @param backup_dir 备份目录
     * @return bool
     */
    bool perform_restore(const std::string& backup_dir);
    /*
     * start_background_tasks - 启动后台任务
     * 
     * @return void
     */
    void start_background_tasks();
    /*
     * stop_background_tasks - 停止后台任务
     * 
     * @return void
     */
    void stop_background_tasks();
    /*
     * start_metrics_thread - 启动监控线程
     * 
     * @return void
     */
    void start_metrics_thread();
    /*
     * ensure_directory - 确保目录存在
     * 
     * @param path 目录路径
     * @return void
     */
    static void ensure_directory(const std::string& path);
};

} // namespace core
} // namespace mementodb

#endif // DISK_ENGINE_HPP