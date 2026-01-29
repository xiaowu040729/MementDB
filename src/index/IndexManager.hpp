#ifndef INDEX_MANAGER_HPP
#define INDEX_MANAGER_HPP

#include "IIndex.hpp"
#include <unordered_map>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <queue>
#include <future>
#include <set>
#include <list>
#include <thread>
#include <sstream>
#include <stdexcept>

namespace mementodb {
namespace index {

// 前向声明
class IIndex;
/**
 * 索引事件类型
 */
enum class IndexEventType {
    CREATED,
    DELETED,
    MODIFIED,
    PERSISTED,
    LOADED,
    ERROR
};

/**
 * 索引事件
 */
struct IndexEvent {
    IndexEventType type;
    std::string index_name;
    IndexType index_type;
    std::string message{};
    std::chrono::system_clock::time_point timestamp;
    
    IndexEvent(IndexEventType t, const std::string& name, IndexType it = IndexType::BPLUS_TREE,
               const std::string& msg = "")
        : type(t), index_name(name), index_type(it), message(msg),
          timestamp(std::chrono::system_clock::now()) {}
};

/**
 * 索引管理器配置
 */
struct IndexManagerConfig {
    std::string storage_path = "./indexes";           // 索引存储路径
    bool auto_persist = false;                        // 是否自动持久化
    std::chrono::seconds persist_interval{300};       // 持久化间隔
    std::chrono::seconds persist_timeout{30};         // 持久化超时时间
    bool enable_statistics = true;                    // 是否启用统计信息
    bool enable_monitoring = false;                   // 是否启用监控
    size_t max_index_count = 1000;                    // 最大索引数量
    size_t max_memory_mb = 1024;                      // 最大内存使用量（MB）
    bool lazy_loading = false;                        // 是否延迟加载
    bool enable_cache = true;                         // 是否启用缓存
    size_t cache_size = 100;                          // 缓存索引数量
    
    // 序列化和反序列化
    bool validate() const {
        return !storage_path.empty() && 
               max_index_count > 0 && 
               max_memory_mb > 0;
    }
};

/**
 * 索引管理器的监控指标
 */
struct IndexManagerMetrics {
    size_t total_indexes = 0;
    size_t active_indexes = 0;
    size_t persisted_indexes = 0;
    size_t memory_usage_mb = 0;
    size_t disk_usage_mb = 0;
    uint64_t create_count = 0;
    uint64_t delete_count = 0;
    uint64_t persist_count = 0;
    uint64_t load_count = 0;
    uint64_t error_count = 0;
    std::chrono::system_clock::time_point last_persist_time;
    std::chrono::system_clock::time_point last_error_time;
    
    std::string to_string() const {
        std::ostringstream oss;
        oss << "IndexManagerMetrics{"
            << "total_indexes=" << total_indexes
            << ", active_indexes=" << active_indexes
            << ", persisted_indexes=" << persisted_indexes
            << ", memory_usage_mb=" << memory_usage_mb
            << ", disk_usage_mb=" << disk_usage_mb
            << ", create_count=" << create_count
            << ", delete_count=" << delete_count
            << ", persist_count=" << persist_count
            << ", load_count=" << load_count
            << ", error_count=" << error_count
            << "}";
        return oss.str();
    }
};

/**
 * 索引管理器的持久化任务
 */
struct PersistTask {
    std::string index_name;
    std::promise<bool> promise;
    std::chrono::system_clock::time_point deadline;
    
    PersistTask(const std::string& name) : index_name(name) {}
};

/**
 * 索引管理器
 * 
 * 功能：
 * - 统一管理所有索引的生命周期
 * - 支持索引工厂模式注册和创建
 * - 提供索引的事件通知机制
 * - 支持异步持久化和加载
 * - 支持索引的依赖关系管理
 * - 提供监控和统计功能
 */
class IndexManager {
public:
    explicit IndexManager(const IndexManagerConfig& config = IndexManagerConfig{});
    ~IndexManager();
    
    // 禁止拷贝和移动（管理器应为单例或受控实例）
    IndexManager(const IndexManager&) = delete;
    IndexManager& operator=(const IndexManager&) = delete;
    IndexManager(IndexManager&&) = delete;
    IndexManager& operator=(IndexManager&&) = delete;
    
    /**
     * 获取管理器实例（可选单例模式）
     */
    static std::shared_ptr<IndexManager> get_instance(
        const IndexManagerConfig& config = IndexManagerConfig{});
    
    /**
     * 注册索引工厂函数
     */
    template<typename ConfigType>
    bool register_factory(IndexType type, 
                         std::function<IndexPtr(const ConfigType&)> factory);
    
    /**
     * 创建索引（泛型版本）
     */
    template<typename ConfigType>
    IndexPtr create_index(const std::string& name, const ConfigType& config);
    
    /**
     * 创建索引（类型+配置）
     */
    IndexPtr create_index(IndexType type, const std::string& name, 
                         const std::shared_ptr<IndexConfig>& config);
    
    /**
     * 通过JSON配置创建索引
     */
    IndexPtr create_index_from_json(const std::string& name, const std::string& json_config);
    
    /**
     * 批量创建索引
     */
    std::vector<IndexPtr> create_indexes(
        const std::vector<std::pair<std::string, std::shared_ptr<IndexConfig>>>& specs);
    
    /**
     * 获取索引（带缓存）
     */
    IndexPtr get_index(const std::string& name);
    
    /**
     * 获取或创建索引（线程安全）
     */
    IndexPtr get_or_create_index(const std::string& name, 
                                 const std::function<IndexPtr()>& creator);
    
    /**
     * 删除索引（可选是否持久化）
     */
    bool remove_index(const std::string& name, bool persist_before_delete = false);
    
    /**
     * 重命名索引
     */
    bool rename_index(const std::string& old_name, const std::string& new_name);
    
    /**
     * 复制索引
     */
    IndexPtr clone_index(const std::string& src_name, const std::string& dst_name);
    
    /**
     * 检查索引是否存在
     */
    bool has_index(const std::string& name) const;
    
    /**
     * 获取所有索引名称（可过滤）
     */
    std::vector<std::string> list_indexes(IndexType filter_type = IndexType::BPLUS_TREE) const;
    
    /**
     * 获取索引统计信息
     */
    std::unordered_map<std::string, IndexStatistics> get_all_statistics() const;
    
    /**
     * 重置所有统计信息
     */
    void reset_all_statistics();
    
    /**
     * 持久化单个索引
     */
    std::future<bool> persist_index_async(const std::string& name);
    
    /**
     * 持久化所有索引
     */
    std::future<size_t> persist_all_async();
    
    /**
     * 加载单个索引
     */
    std::future<bool> load_index_async(const std::string& name);
    
    /**
     * 加载所有索引
     */
    std::future<size_t> load_all_async();
    
    /**
     * 清理过期的临时索引
     */
    size_t cleanup_temp_indexes();
    
    /**
     * 压缩存储（删除未使用的持久化文件）
     */
    size_t compact_storage();
    
    /**
     * 获取管理器指标
     */
    IndexManagerMetrics get_metrics() const;
    
    /**
     * 注册事件监听器
     */
    using EventListener = std::function<void(const IndexEvent&)>;
    void add_event_listener(EventListener listener);
    
    /**
     * 设置错误处理器
     */
    using ErrorHandler = std::function<void(const std::string&, const std::exception&)>;
    void set_error_handler(ErrorHandler handler);
    
    /**
     * 开始后台任务
     */
    void start_background_tasks();
    
    /**
     * 停止后台任务
     */
    void stop_background_tasks();
    
    /**
     * 等待所有任务完成
     */
    void wait_for_tasks();
    
    /**
     * 获取配置
     */
    IndexManagerConfig get_config() const;
    
    /**
     * 更新配置（动态调整）
     */
    bool update_config(const IndexManagerConfig& new_config);
    
private:
    IndexManagerConfig config_;
    
    // 索引存储
    struct IndexEntry {
        IndexPtr index;
        std::chrono::system_clock::time_point created_time;
        std::chrono::system_clock::time_point last_access_time;
        size_t access_count = 0;
        bool is_persisted = false;
        bool is_temp = false;
        bool is_modified = false;
    };
    
    std::unordered_map<std::string, IndexEntry> indexes_;
    mutable std::shared_mutex indexes_mutex_;
    
    // 工厂注册
    std::unordered_map<IndexType, std::function<IndexPtr(const std::shared_ptr<IndexConfig>&)>> factories_;
    std::shared_mutex factories_mutex_;
    
    // 缓存管理（LRU）
    mutable std::list<std::string> access_order_;
    mutable std::unordered_map<std::string, 
        std::list<std::string>::iterator> access_map_;
    
    // 后台任务
    std::atomic<bool> running_{false};
    std::vector<std::thread> worker_threads_;
    std::condition_variable task_cv_;
    mutable std::mutex task_mutex_;
    
    // 任务队列
    std::queue<std::function<void()>> task_queue_;
    
    // 持久化任务
    std::unordered_map<std::string, std::shared_ptr<PersistTask>> persist_tasks_;
    std::mutex persist_tasks_mutex_;
    
    // 事件系统
    std::vector<EventListener> event_listeners_;
    mutable std::mutex listeners_mutex_;
    
    // 错误处理
    ErrorHandler error_handler_;
    mutable std::mutex error_handler_mutex_;
    
    // 监控数据
    mutable IndexManagerMetrics metrics_;
    mutable std::mutex metrics_mutex_;
    
    // 私有方法
    void init_factories();
    void notify_event(const IndexEvent& event) const;
    void handle_error(const std::string& context, const std::exception& e) const;
    void update_access_time(const std::string& name);
    void evict_if_needed();
    void worker_thread_func();
    void add_task(std::function<void()> task);
    
    // 持久化辅助
    std::string get_index_filename(const std::string& name) const;
    std::string get_index_metadata_filename(const std::string& name) const;
    bool save_index_metadata(const std::string& name, const IndexEntry& entry) const;
    bool load_index_metadata(const std::string& name, IndexEntry& entry) const;
    
    // 配置管理
    bool load_config();
    bool save_config() const;
};

// 模板方法实现
template<typename ConfigType>
bool IndexManager::register_factory(IndexType type, 
                                   std::function<IndexPtr(const ConfigType&)> factory) {
    std::unique_lock lock(factories_mutex_);
    
    // 包装工厂函数以接受基类配置
    auto wrapped_factory = [factory](const std::shared_ptr<IndexConfig>& base_config) -> IndexPtr {
        auto derived_config = std::dynamic_pointer_cast<ConfigType>(base_config);
        if (!derived_config) {
            throw std::invalid_argument("Invalid configuration type for index factory");
        }
        return factory(*derived_config);
    };
    
    factories_[type] = wrapped_factory;
    return true;
}

template<typename ConfigType>
IndexPtr IndexManager::create_index(const std::string& name, const ConfigType& config) {
    std::unique_lock index_lock(indexes_mutex_);
    
    // 检查是否已存在
    if (indexes_.find(name) != indexes_.end()) {
        throw std::runtime_error("Index already exists: " + name);
    }
    
    // 创建索引
    IndexPtr index;
    try {
        // 从工厂创建
        std::shared_ptr<IndexConfig> base_config = std::make_shared<ConfigType>(config);
        base_config->name = name;
        
        index_lock.unlock();
        index = create_index(config.type, name, base_config);
        index_lock.lock();
        
        // 添加到管理器
        indexes_[name] = IndexEntry{
            index,
            std::chrono::system_clock::now(),
            std::chrono::system_clock::now(),
            1,
            false,
            false,
            false
        };
        
        // 更新缓存
        access_order_.push_front(name);
        access_map_[name] = access_order_.begin();
        
        // 更新指标
        {
            std::lock_guard metrics_lock(metrics_mutex_);
            metrics_.total_indexes++;
            metrics_.active_indexes++;
            metrics_.create_count++;
        }
        
        // 通知事件
        notify_event(IndexEvent{IndexEventType::CREATED, name, config.type});
        
        // 检查是否需要驱逐
        evict_if_needed();
        
    } catch (const std::exception& e) {
        handle_error("Failed to create index: " + name, e);
        throw;
    }
    
    return index;
}

} // namespace index
} // namespace mementodb

#endif // INDEX_MANAGER_HPP
