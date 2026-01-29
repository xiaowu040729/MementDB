#include "IndexManager.hpp"
#include "HashIndex.hpp"
#include "SkipListIndex.hpp"
#include "BloomFilter.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace mementodb {
namespace index {

// 静态单例实例
static std::shared_ptr<IndexManager> g_instance = nullptr;
static std::mutex g_instance_mutex;

IndexManager::IndexManager(const IndexManagerConfig& config) 
    : config_(config), running_(false) {
    
    if (!config_.validate()) {
        throw std::invalid_argument("Invalid IndexManagerConfig");
    }
    
    // 创建存储目录
    std::filesystem::create_directories(config_.storage_path);
    
    // 初始化工厂
    init_factories();
    
    // 初始化指标
    metrics_ = IndexManagerMetrics{};
}

IndexManager::~IndexManager() {
    stop_background_tasks();
    wait_for_tasks();
}

std::shared_ptr<IndexManager> IndexManager::get_instance(
    const IndexManagerConfig& config) {
    std::lock_guard lock(g_instance_mutex);
    
    if (!g_instance) {
        g_instance = std::shared_ptr<IndexManager>(new IndexManager(config));
    }
    
    return g_instance;
}

void IndexManager::init_factories() {
    std::unique_lock lock(factories_mutex_);
    
    // 注册哈希索引工厂
    factories_[IndexType::HASH] = [](const std::shared_ptr<IndexConfig>& base_config) -> IndexPtr {
        auto config = std::dynamic_pointer_cast<HashIndexConfig>(base_config);
        if (!config) {
            throw std::invalid_argument("Invalid HashIndexConfig");
        }
        return std::make_shared<HashIndex>(*config);
    };
    
    // 注册跳表索引工厂
    factories_[IndexType::SKIP_LIST] = [](const std::shared_ptr<IndexConfig>& base_config) -> IndexPtr {
        auto config = std::dynamic_pointer_cast<SkipListIndexConfig>(base_config);
        if (!config) {
            throw std::invalid_argument("Invalid SkipListIndexConfig");
        }
        return std::make_shared<SkipListIndex>(*config);
    };
    
    // 注册布隆过滤器工厂
    factories_[IndexType::BLOOM_FILTER] = [](const std::shared_ptr<IndexConfig>& base_config) -> IndexPtr {
        auto config = std::dynamic_pointer_cast<BloomFilterConfig>(base_config);
        if (!config) {
            throw std::invalid_argument("Invalid BloomFilterConfig");
        }
        return std::make_shared<BloomFilter>(*config);
    };
}

IndexPtr IndexManager::create_index(IndexType type, const std::string& name,
                                    const std::shared_ptr<IndexConfig>& config) {
    std::unique_lock index_lock(indexes_mutex_);
    
    // 检查是否已存在
    if (indexes_.find(name) != indexes_.end()) {
        throw std::runtime_error("Index already exists: " + name);
    }
    
    // 检查数量限制
    if (indexes_.size() >= config_.max_index_count) {
        throw std::runtime_error("Maximum index count reached");
    }
    
    // 从工厂创建索引
    std::shared_lock factory_lock(factories_mutex_);
    auto it = factories_.find(type);
    if (it == factories_.end()) {
        throw std::runtime_error("No factory registered for index type");
    }
    
    IndexPtr index;
    try {
        config->name = name;
        index = it->second(config);
    } catch (const std::exception& e) {
        handle_error("Failed to create index: " + name, e);
        throw;
    }
    factory_lock.unlock();
    
    // 添加到管理器
    auto now = std::chrono::system_clock::now();
    indexes_[name] = IndexEntry{
        index,
        now,
        now,
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
    notify_event(IndexEvent{IndexEventType::CREATED, name, type});
    
    // 检查是否需要驱逐
    evict_if_needed();
    
    return index;
}

IndexPtr IndexManager::create_index_from_json(const std::string& name, 
                                               const std::string& json_config) {
    // 简化实现：这里应该解析JSON并创建配置
    // 实际实现需要使用JSON库（如nlohmann/json）
    throw std::runtime_error("JSON config not implemented yet");
}

std::vector<IndexPtr> IndexManager::create_indexes(
    const std::vector<std::pair<std::string, std::shared_ptr<IndexConfig>>>& specs) {
    std::vector<IndexPtr> results;
    results.reserve(specs.size());
    
    for (const auto& spec : specs) {
        try {
            auto index = create_index(spec.second->type, spec.first, spec.second);
            results.push_back(index);
        } catch (const std::exception& e) {
            handle_error("Failed to create index: " + spec.first, e);
        }
    }
    
    return results;
}

IndexPtr IndexManager::get_index(const std::string& name) {
    IndexPtr result;
    {
        std::shared_lock<std::shared_mutex> lock(indexes_mutex_);
        auto it = indexes_.find(name);
        if (it == indexes_.end()) {
            return nullptr;
        }
        result = it->second.index;
    }
    // 升级为独占更新访问时间，避免持有读锁再写锁造成死锁
    update_access_time(name);
    return result;
}

IndexPtr IndexManager::get_or_create_index(const std::string& name,
                                          const std::function<IndexPtr()>& creator) {
    std::unique_lock lock(indexes_mutex_);
    
    auto it = indexes_.find(name);
    if (it != indexes_.end()) {
        update_access_time(name);
        return it->second.index;
    }
    
    lock.unlock();
    auto index = creator();
    lock.lock();
    
    // 再次检查（可能其他线程已创建）
    it = indexes_.find(name);
    if (it != indexes_.end()) {
        return it->second.index;
    }
    
    // 添加到管理器
    auto now = std::chrono::system_clock::now();
    indexes_[name] = IndexEntry{
        index,
        now,
        now,
        1,
        false,
        false,
        false
    };
    
    access_order_.push_front(name);
    access_map_[name] = access_order_.begin();
    
    {
        std::lock_guard metrics_lock(metrics_mutex_);
        metrics_.total_indexes++;
        metrics_.active_indexes++;
        metrics_.create_count++;
    }
    
    notify_event(IndexEvent{IndexEventType::CREATED, name, index->type()});
    
    return index;
}

bool IndexManager::remove_index(const std::string& name, bool persist_before_delete) {
    std::unique_lock lock(indexes_mutex_);
    
    auto it = indexes_.find(name);
    if (it == indexes_.end()) {
        return false;
    }
    
    // 持久化（如果需要）
    if (persist_before_delete && it->second.is_modified) {
        lock.unlock();
        persist_index_async(name).wait();
        lock.lock();
    }
    
    // 从缓存中移除
    auto access_it = access_map_.find(name);
    if (access_it != access_map_.end()) {
        access_order_.erase(access_it->second);
        access_map_.erase(access_it);
    }
    
    // 移除索引
    IndexType type = it->second.index->type();
    indexes_.erase(it);
    
    // 更新指标
    {
        std::lock_guard metrics_lock(metrics_mutex_);
        metrics_.total_indexes--;
        metrics_.active_indexes--;
        metrics_.delete_count++;
    }
    
    // 通知事件
    notify_event(IndexEvent{IndexEventType::DELETED, name, type});
    
    return true;
}

bool IndexManager::rename_index(const std::string& old_name, const std::string& new_name) {
    std::unique_lock lock(indexes_mutex_);
    
    if (indexes_.find(new_name) != indexes_.end()) {
        return false; // 新名称已存在
    }
    
    auto it = indexes_.find(old_name);
    if (it == indexes_.end()) {
        return false; // 旧名称不存在
    }
    
    // 移动索引条目
    auto entry = std::move(it->second);
    indexes_.erase(it);
    indexes_[new_name] = std::move(entry);
    
    // 更新缓存
    auto access_it = access_map_.find(old_name);
    if (access_it != access_map_.end()) {
        *access_it->second = new_name;
        access_map_[new_name] = access_it->second;
        access_map_.erase(access_it);
    }
    
    // 更新索引名称
    entry.index->get_config()->name = new_name;
    
    notify_event(IndexEvent{IndexEventType::MODIFIED, new_name, entry.index->type()});
    
    return true;
}

IndexPtr IndexManager::clone_index(const std::string& src_name, const std::string& dst_name) {
    std::shared_lock<std::shared_mutex> read_lock(indexes_mutex_);
    
    auto src_it = indexes_.find(src_name);
    if (src_it == indexes_.end()) {
        return nullptr;
    }
    
    auto src_index = src_it->second.index;
    IndexPtr cloned;
    
    read_lock.unlock();
    
    // 根据类型创建克隆
    switch (src_index->type()) {
        case IndexType::HASH: {
            auto hash_index = std::dynamic_pointer_cast<HashIndex>(src_index);
            if (hash_index) {
                cloned = hash_index->snapshot();
            }
            break;
        }
        case IndexType::SKIP_LIST: {
            // SkipListIndex 快照：通过配置重新创建
            auto config = src_index->get_config();
            auto skip_config = std::dynamic_pointer_cast<SkipListIndexConfig>(
                std::shared_ptr<IndexConfig>(config->clone().release()));
            if (skip_config) {
                skip_config->name = dst_name;
                cloned = std::make_shared<SkipListIndex>(*skip_config);
                // 复制数据（简化：通过遍历插入）
                // 实际应该实现更高效的复制方法
            }
            break;
        }
        case IndexType::BLOOM_FILTER: {
            // 布隆过滤器快照：通过配置重新创建并合并
            auto config = src_index->get_config();
            auto bloom_config = std::dynamic_pointer_cast<BloomFilterConfig>(
                std::shared_ptr<IndexConfig>(config->clone().release()));
            if (bloom_config) {
                bloom_config->name = dst_name;
                cloned = std::make_shared<BloomFilter>(*bloom_config);
                // 合并数据
                auto bloom_src = std::dynamic_pointer_cast<BloomFilter>(src_index);
                auto bloom_dst = std::dynamic_pointer_cast<BloomFilter>(cloned);
                if (bloom_src && bloom_dst) {
                    bloom_dst->merge(*bloom_src);
                }
            }
            break;
        }
        default:
            // 其他类型暂不支持快照
            return nullptr;
    }
    
    if (!cloned) {
        return nullptr;
    }
    
    // 添加到管理器
    std::unique_lock<std::shared_mutex> write_lock(indexes_mutex_);
    auto now = std::chrono::system_clock::now();
    indexes_[dst_name] = IndexEntry{
        cloned,
        now,
        now,
        1,
        false,
        false,
        false
    };
    
    access_order_.push_front(dst_name);
    access_map_[dst_name] = access_order_.begin();
    
    {
        std::lock_guard metrics_lock(metrics_mutex_);
        metrics_.total_indexes++;
        metrics_.active_indexes++;
    }
    
    notify_event(IndexEvent{IndexEventType::CREATED, dst_name, cloned->type()});
    
    return cloned;
}

bool IndexManager::has_index(const std::string& name) const {
    std::shared_lock lock(indexes_mutex_);
    return indexes_.find(name) != indexes_.end();
}

std::vector<std::string> IndexManager::list_indexes(IndexType filter_type) const {
    std::shared_lock lock(indexes_mutex_);
    
    std::vector<std::string> result;
    for (const auto& [name, entry] : indexes_) {
        if (filter_type == IndexType::BPLUS_TREE || entry.index->type() == filter_type) {
            result.push_back(name);
        }
    }
    
    return result;
}

std::unordered_map<std::string, IndexStatistics> IndexManager::get_all_statistics() const {
    std::shared_lock lock(indexes_mutex_);
    
    std::unordered_map<std::string, IndexStatistics> result;
    for (const auto& [name, entry] : indexes_) {
        result[name] = entry.index->get_statistics();
    }
    
    return result;
}

void IndexManager::reset_all_statistics() {
    std::shared_lock lock(indexes_mutex_);
    
    for (const auto& [name, entry] : indexes_) {
        entry.index->reset_statistics();
    }
}

std::future<bool> IndexManager::persist_index_async(const std::string& name) {
    auto promise = std::make_shared<std::promise<bool>>();
    auto future = promise->get_future();
    
    add_task([this, name, promise]() {
        try {
            std::shared_lock lock(indexes_mutex_);
            auto it = indexes_.find(name);
            if (it == indexes_.end()) {
                promise->set_value(false);
                return;
            }
            
            auto index = it->second.index;
            std::string path = get_index_filename(name);
            lock.unlock();
            
            bool success = index->persist(path);
            
            lock.lock();
            if (success) {
                it->second.is_persisted = true;
                it->second.is_modified = false;
            }
            
            {
                std::lock_guard metrics_lock(metrics_mutex_);
                if (success) {
                    metrics_.persist_count++;
                    metrics_.last_persist_time = std::chrono::system_clock::now();
                }
            }
            
            notify_event(IndexEvent{IndexEventType::PERSISTED, name, index->type()});
            promise->set_value(success);
        } catch (const std::exception& e) {
            handle_error("Failed to persist index: " + name, e);
            promise->set_value(false);
        }
    });
    
    return future;
}

std::future<size_t> IndexManager::persist_all_async() {
    auto promise = std::make_shared<std::promise<size_t>>();
    auto future = promise->get_future();
    
    add_task([this, promise]() {
        std::vector<std::string> names;
        {
            std::shared_lock lock(indexes_mutex_);
            for (const auto& [name, entry] : indexes_) {
                if (entry.is_modified) {
                    names.push_back(name);
                }
            }
        }
        
        size_t success_count = 0;
        for (const auto& name : names) {
            auto fut = persist_index_async(name);
            if (fut.get()) {
                success_count++;
            }
        }
        
        promise->set_value(success_count);
    });
    
    return future;
}

std::future<bool> IndexManager::load_index_async(const std::string& name) {
    auto promise = std::make_shared<std::promise<bool>>();
    auto future = promise->get_future();
    
    add_task([this, name, promise]() {
        try {
            std::string path = get_index_filename(name);
            std::ifstream ifs(path);
            if (!ifs) {
                promise->set_value(false);
                return;
            }
            
            // 这里需要根据元数据确定索引类型并加载
            // 简化实现
            promise->set_value(false);
        } catch (const std::exception& e) {
            handle_error("Failed to load index: " + name, e);
            promise->set_value(false);
        }
    });
    
    return future;
}

std::future<size_t> IndexManager::load_all_async() {
    auto promise = std::make_shared<std::promise<size_t>>();
    auto future = promise->get_future();
    
    add_task([this, promise]() {
        // 扫描存储目录并加载所有索引
        size_t count = 0;
        // 简化实现
        promise->set_value(count);
    });
    
    return future;
}

size_t IndexManager::cleanup_temp_indexes() {
    std::unique_lock lock(indexes_mutex_);
    
    size_t count = 0;
    auto it = indexes_.begin();
    while (it != indexes_.end()) {
        if (it->second.is_temp) {
            auto access_it = access_map_.find(it->first);
            if (access_it != access_map_.end()) {
                access_order_.erase(access_it->second);
                access_map_.erase(access_it);
            }
            it = indexes_.erase(it);
            count++;
        } else {
            ++it;
        }
    }
    
    return count;
}

size_t IndexManager::compact_storage() {
    // 删除未使用的持久化文件
    size_t count = 0;
    std::filesystem::path storage_path(config_.storage_path);
    
    if (std::filesystem::exists(storage_path)) {
        for (const auto& entry : std::filesystem::directory_iterator(storage_path)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                std::string index_name = filename.substr(0, filename.find_last_of('.'));
                
                if (!has_index(index_name)) {
                    std::filesystem::remove(entry.path());
                    count++;
                }
            }
        }
    }
    
    return count;
}

IndexManagerMetrics IndexManager::get_metrics() const {
    std::lock_guard lock(metrics_mutex_);
    return metrics_;
}

void IndexManager::add_event_listener(EventListener listener) {
    std::lock_guard lock(listeners_mutex_);
    event_listeners_.push_back(listener);
}

void IndexManager::set_error_handler(ErrorHandler handler) {
    std::lock_guard lock(error_handler_mutex_);
    error_handler_ = handler;
}

void IndexManager::start_background_tasks() {
    if (running_.load()) {
        return;
    }
    
    running_ = true;
    
    // 启动工作线程
    size_t num_threads = std::max<size_t>(1, std::thread::hardware_concurrency() / 2);
    for (size_t i = 0; i < num_threads; ++i) {
        worker_threads_.emplace_back(&IndexManager::worker_thread_func, this);
    }
}

void IndexManager::stop_background_tasks() {
    if (!running_.load()) {
        return;
    }
    
    running_ = false;
    task_cv_.notify_all();
    
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    
    worker_threads_.clear();
}

void IndexManager::wait_for_tasks() {
    std::unique_lock lock(task_mutex_);
    while (!task_queue_.empty()) {
        task_cv_.wait(lock);
    }
}

IndexManagerConfig IndexManager::get_config() const {
    return config_;
}

bool IndexManager::update_config(const IndexManagerConfig& new_config) {
    if (!new_config.validate()) {
        return false;
    }
    
    config_ = new_config;
    return true;
}

// 私有方法实现

void IndexManager::notify_event(const IndexEvent& event) const {
    std::lock_guard<std::mutex> lock(listeners_mutex_);
    for (const auto& listener : event_listeners_) {
        try {
            listener(event);
        } catch (...) {
            // 忽略监听器异常
        }
    }
}

void IndexManager::handle_error(const std::string& context, const std::exception& e) const {
    std::lock_guard<std::mutex> lock(error_handler_mutex_);
    if (error_handler_) {
        try {
            error_handler_(context, e);
        } catch (...) {
            // 忽略错误处理器异常
        }
    }
    
    {
        std::lock_guard metrics_lock(metrics_mutex_);
        metrics_.error_count++;
        metrics_.last_error_time = std::chrono::system_clock::now();
    }
    
    notify_event(IndexEvent{IndexEventType::ERROR, "", IndexType::BPLUS_TREE, context});
}

void IndexManager::update_access_time(const std::string& name) {
    std::unique_lock<std::shared_mutex> lock(indexes_mutex_);
    auto it = indexes_.find(name);
    if (it != indexes_.end()) {
        it->second.last_access_time = std::chrono::system_clock::now();
        it->second.access_count++;
        
        // 更新LRU缓存
        auto access_it = access_map_.find(name);
        if (access_it != access_map_.end()) {
            access_order_.erase(access_it->second);
            access_order_.push_front(name);
            access_map_[name] = access_order_.begin();
        }
    }
}

void IndexManager::evict_if_needed() {
    if (!config_.enable_cache) {
        return;
    }
    
    while (access_order_.size() > config_.cache_size) {
        std::string lru_name = access_order_.back();
        access_order_.pop_back();
        access_map_.erase(lru_name);
        
        // 可以选择持久化或直接删除
        auto it = indexes_.find(lru_name);
        if (it != indexes_.end() && it->second.is_modified) {
            // 异步持久化
            persist_index_async(lru_name);
        }
    }
}

void IndexManager::worker_thread_func() {
    while (running_.load()) {
        std::unique_lock lock(task_mutex_);
        
        task_cv_.wait(lock, [this] {
            return !task_queue_.empty() || !running_.load();
        });
        
        if (!running_.load() && task_queue_.empty()) {
            break;
        }
        
        if (!task_queue_.empty()) {
            auto task = std::move(task_queue_.front());
            task_queue_.pop();
            lock.unlock();
            
            try {
                task();
            } catch (const std::exception& e) {
                handle_error("Background task error", e);
            }
            
            task_cv_.notify_one();
        }
    }
}

void IndexManager::add_task(std::function<void()> task) {
    std::lock_guard lock(task_mutex_);
    task_queue_.push(std::move(task));
    task_cv_.notify_one();
}

std::string IndexManager::get_index_filename(const std::string& name) const {
    return (std::filesystem::path(config_.storage_path) / (name + ".idx")).string();
}

std::string IndexManager::get_index_metadata_filename(const std::string& name) const {
    return (std::filesystem::path(config_.storage_path) / (name + ".meta")).string();
}

bool IndexManager::save_index_metadata(const std::string& name, const IndexEntry& entry) const {
    std::ofstream ofs(get_index_metadata_filename(name));
    if (!ofs) {
        return false;
    }
    
    // 简化实现：保存基本元数据
    ofs << "name=" << name << "\n";
    ofs << "type=" << static_cast<int>(entry.index->type()) << "\n";
    // 更多元数据...
    
    return ofs.good();
}

bool IndexManager::load_index_metadata(const std::string& name, IndexEntry& entry) const {
    std::ifstream ifs(get_index_metadata_filename(name));
    if (!ifs) {
        return false;
    }
    
    // 简化实现：加载基本元数据
    // 实际实现需要解析元数据文件
    
    return ifs.good();
}

bool IndexManager::load_config() {
    std::filesystem::path config_path = std::filesystem::path(config_.storage_path) / "manager.cfg";
    if (!std::filesystem::exists(config_path)) {
        return false;
    }
    
    // 简化实现：实际需要序列化/反序列化配置
    return true;
}

bool IndexManager::save_config() const {
    std::filesystem::path config_path = std::filesystem::path(config_.storage_path) / "manager.cfg";
    
    // 简化实现：实际需要序列化配置
    return true;
}

} // namespace index
} // namespace mementodb

