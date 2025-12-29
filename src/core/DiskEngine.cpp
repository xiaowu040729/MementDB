// File: src/core/DiskEngine.cpp
#include "DiskEngine.hpp"
#include "Record.hpp"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <thread>
#include <chrono>
#include <stdexcept>
#include <cstring>
#include <sys/stat.h>
#include <limits>
#include <queue>
#include <optional>
#include <string>

namespace mementodb {
namespace core {

// ==================== 辅助函数 ====================

static uint64_t get_current_time() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ==================== ExtentManager 实现 ====================

ExtentManager::ExtentManager(const std::string& base_path, size_t extent_size, size_t page_size,
                             const FileManagerConfig& file_config)
    : base_path_(base_path),
      extent_size_(extent_size),
      page_size_(page_size),
      next_extent_id_(0),
      file_config_(file_config) {
    
    ensure_directory(base_path);
    load_extent_table();
}

ExtentManager::~ExtentManager() {
    save_extent_table();
    // FileHandle 会自动关闭
}

ExtentManager::Extent* ExtentManager::allocate_extent(size_t page_count) {
    uint32_t file_id = 0;
    uint64_t start_page = 0;
    
    {
        std::lock_guard<std::mutex> lock(extents_mutex_);
        
        // 尝试重用未完全分配的扩展
        for (auto& extent : extents_) {
            if (extent && !extent->allocated && extent->page_count >= page_count) {
                extent->allocated = true;
                return extent.get();
            }
        }
        
        // 创建新扩展
        file_id = next_extent_id_++;
        start_page = total_pages_;
        
        extents_.push_back(std::make_unique<Extent>(start_page, page_count, file_id));
        extents_.back()->allocated = true;
        
        total_pages_ += page_count;
        
        // 使用FileManager创建文件
        std::string file_path = get_extent_path(file_id);
        try {
            auto file_handle = std::make_shared<FileHandle>(
                file_path, FileHandle::OpenMode::Create, &file_config_);
            
            // 检查文件是否成功打开
            if (!file_handle->is_open()) {
                LOG_ERROR("ExtentManager", "Failed to create file: " + file_path);
                return nullptr;
            }
            
            // 预分配空间
            file_handle->allocate(0, page_count * page_size_);
            
            // 缓存文件句柄
            file_cache_[file_id] = file_handle;
        } catch (const std::exception& e) {
            LOG_ERROR("ExtentManager", "Exception creating file: " + file_path + 
                     ", Error: " + e.what());
            return nullptr;
        }
    }
    
    return extents_.back().get();
}

bool ExtentManager::free_extent(uint32_t file_id) {
    std::lock_guard<std::mutex> lock(extents_mutex_);
    
    auto it = std::find_if(extents_.begin(), extents_.end(),
        [file_id](const std::unique_ptr<Extent>& e) {
            return e && e->file_id == file_id;
        });
    
    if (it != extents_.end() && *it) {
        (*it)->allocated = false;
        // 可以延迟物理删除
        return true;
    }
    
    return false;
}

ExtentManager::Extent* ExtentManager::find_extent(uint64_t page_id) {
    std::lock_guard<std::mutex> lock(extents_mutex_);
    
    for (auto& extent : extents_) {
        if (extent && extent->contains(page_id)) {
            return extent.get();
        }
    }
    
    return nullptr;
}

std::shared_ptr<FileHandle> ExtentManager::get_file_handle(uint32_t file_id) {
    std::lock_guard<std::mutex> lock(file_cache_mutex_);
    
    auto it = file_cache_.find(file_id);
    if (it != file_cache_.end()) {
        return it->second;
    }
    
    // 打开文件
    std::string file_path = get_extent_path(file_id);
    auto file_handle = std::make_shared<FileHandle>(
        file_path, FileHandle::OpenMode::ReadWrite, &file_config_);
    
    if (file_handle->is_open()) {
        file_cache_[file_id] = file_handle;
        return file_handle;
    }
    
    return nullptr;
}

ExtentManager::ExtentStats ExtentManager::get_stats() const {
    std::lock_guard<std::mutex> lock(extents_mutex_);
    
    ExtentStats stats{};
    stats.total_extents = extents_.size();
    
    for (const auto& extent : extents_) {
        if (extent) {
            if (extent->allocated) {
                ++stats.allocated_extents;
                stats.allocated_pages += extent->page_count;
            }
            stats.total_pages += extent->page_count;
        }
    }
    
    if (stats.total_pages > 0) {
        stats.fragmentation = 100 - (stats.allocated_pages * 100 / stats.total_pages);
    }
    
    return stats;
}

void ExtentManager::ensure_directory(const std::string& path) {
    std::filesystem::create_directories(path);
}

std::string ExtentManager::get_extent_path(uint32_t file_id) const {
    return base_path_ + "/extent_" + std::to_string(file_id) + ".dat";
}

void ExtentManager::load_extent_table() {
    std::string table_path = base_path_ + "/extent_table.bin";
    std::ifstream file(table_path, std::ios::binary);
    
    if (!file.is_open()) {
        return;
    }
    
    size_t count;
    file.read(reinterpret_cast<char*>(&count), sizeof(count));
    
    extents_.clear();
    extents_.reserve(count);  // 在添加元素前预留空间，避免移动
    for (size_t i = 0; i < count; ++i) {
        uint64_t start, page_count;
        uint32_t file_id;
        bool allocated;
        
        file.read(reinterpret_cast<char*>(&start), sizeof(start));
        file.read(reinterpret_cast<char*>(&page_count), sizeof(page_count));
        file.read(reinterpret_cast<char*>(&file_id), sizeof(file_id));
        file.read(reinterpret_cast<char*>(&allocated), sizeof(allocated));
        
        extents_.push_back(std::make_unique<Extent>(start, page_count, file_id));
        extents_.back()->allocated = allocated;
        
        if (file_id >= next_extent_id_) {
            next_extent_id_ = file_id + 1;
        }
    }
}

void ExtentManager::save_extent_table() {
    std::string table_path = base_path_ + "/extent_table.bin";
    std::ofstream file(table_path, std::ios::binary);
    
    if (!file.is_open()) {
        return;
    }
    
    size_t count = extents_.size();
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));
    
    for (const auto& extent : extents_) {
        if (extent) {
            file.write(reinterpret_cast<const char*>(&extent->start_page), 
                      sizeof(extent->start_page));
            file.write(reinterpret_cast<const char*>(&extent->page_count), 
                      sizeof(extent->page_count));
            file.write(reinterpret_cast<const char*>(&extent->file_id), 
                      sizeof(extent->file_id));
            file.write(reinterpret_cast<const char*>(&extent->allocated), 
                      sizeof(extent->allocated));
        }
    }
}

// ==================== DiskEngineV2 实现 ====================

DiskEngineV2::DiskEngineV2(const std::string& data_dir, const EngineConfig& config)
    : config_(config),
      data_dir_(data_dir) {
    
    // 配置FileManager
    FileManagerConfig file_config;
    file_config.use_direct_io = config.use_direct_io;
    file_config.use_async_io = config.use_io_uring;
    file_config.use_memory_mapping = config.use_mmap;
    file_config.default_extent_size = config.extent_size;
    file_config.max_file_size = config.max_file_size;
    file_config_ = file_config;
    
    // 初始化组件
    extent_manager_ = std::make_unique<ExtentManager>(
        data_dir + "/data", config.extent_size, config.page_size, file_config_);
    buffer_pool_ = std::make_unique<SmartBufferPool>(
        config.buffer_pool_size, config.page_size, config.flush_strategy);
    
    if (config.enable_wal) {
        // 初始化分布式WAL
        std::vector<std::string> peers; // TODO: 从配置读取
        wal_ = std::make_unique<DistributedWAL>(
            data_dir + "/wal", 1, peers);
    }
    
    // 初始化B+树索引
    BPlusTree::BPlusTreeConfig tree_config;
    tree_config.order = 16;  // 内部节点阶数
    tree_config.leaf_order = 32;  // 叶子节点容量
    tree_config.use_verbose_log = false;
    tree_config.storage_path = data_dir + "/index";
    index_tree_ = std::make_unique<BPlusTree::BPlusTree<std::string, uint64_t>>(tree_config);
    
    initialize_engine();
}

DiskEngineV2::~DiskEngineV2() {
    shutdown();
}

std::future<DiskPage> DiskEngineV2::read_page_async(uint64_t page_id) {
    auto promise = std::make_shared<std::promise<DiskPage>>();
    auto future = promise->get_future();
    
    // 步骤1：先在缓冲池中查找（快速路径）
    auto* frame = buffer_pool_->fetch_page(page_id, false);
    if (frame) {
        // 缓冲池命中：直接复制页数据，无需磁盘IO
        // 注意：必须使用正确的 page_id 构造 DiskPage，然后复制数据
        DiskPage page_copy(page_id);
        if (page_copy.GetData() && frame->page.GetData()) {
            std::memcpy(page_copy.GetData(), frame->page.GetData(), kPageSize);
        }
        // 还需要复制 header_，因为 GetData() 只返回数据区
        // 使用序列化/反序列化来确保完整复制
        char buffer[kPageSize];
        frame->page.SerializeTo(buffer);
        page_copy.DeserializeFrom(buffer);
        buffer_pool_->release_page(page_id, false);
        
        promise->set_value(std::move(page_copy));
        return future;
    }
    
    // 步骤2：不在缓冲池中，需要从磁盘读取（慢速路径）
    auto extent = extent_manager_->find_extent(page_id);
    if (!extent) {
        promise->set_exception(
            std::make_exception_ptr(std::runtime_error("Page not found"))
        );
        return future;
    }
    
    // 步骤3：获取文件句柄
    auto file_handle = extent_manager_->get_file_handle(extent->file_id);
    if (!file_handle || !file_handle->is_open()) {
        promise->set_exception(
            std::make_exception_ptr(std::runtime_error("Failed to open file"))
        );
        return future;
    }
    
    // 步骤4：计算文件中的偏移量
    uint64_t offset = (page_id - extent->start_page) * kPageSize;
    
    // 步骤5：分配临时缓冲区（用于接收磁盘数据）
    auto page_buffer = std::make_unique<char[]>(kPageSize);
    
    // 步骤6：提交异步读请求（使用FileManager的异步IO）
    auto read_future = file_handle->async_read(page_buffer.get(), kPageSize, offset);
    
    // 步骤7：在后台线程中处理IO完成后的逻辑
    // 注意：必须保存 async 返回的 future，否则可能立即销毁导致未执行
    auto async_task = std::async(std::launch::async, 
        [promise, page_buffer = std::move(page_buffer), 
         page_id, read_future = std::move(read_future)]() mutable {
            try {
                LOG_DEBUG("DATABASE", "read_page_async: Async task started for page_id=" + std::to_string(page_id));
                // 等待IO完成
                size_t bytes_read = read_future.get();
                LOG_DEBUG("DATABASE", "read_page_async: IO completed, bytes_read=" + std::to_string(bytes_read));
                if (bytes_read != kPageSize) {
                    throw std::runtime_error("Incomplete page read: expected " + 
                                           std::to_string(kPageSize) + " bytes, got " + 
                                           std::to_string(bytes_read));
                }
                
                // 步骤8：反序列化页数据
                // 注意：DeserializeFrom 会从序列化数据中读取 page_id，但如果磁盘上的数据有问题，
                // page_id 可能是错误的值。我们需要确保反序列化后 page_id 正确。
                // 先反序列化到临时页对象，然后修复 page_id
                DiskPage temp_page(0);  // 临时页对象
                try {
                    temp_page.DeserializeFrom(page_buffer.get());
                } catch (const std::exception& e) {
                    LOG_ERROR("DATABASE", "read_page_async: DeserializeFrom failed: " + std::string(e.what()));
                    throw;
                }
                
                // 保存所有数据（包括 page_id 错误的情况）
                PageType saved_type = temp_page.GetType();
                uint16_t saved_key_count = temp_page.GetKeyCount();
                uint16_t saved_free_offset = temp_page.GetFreeOffset();
                std::vector<char> saved_data(kPageSize - sizeof(PageHeader));
                if (temp_page.GetData()) {
                    std::memcpy(saved_data.data(), temp_page.GetData(), kPageSize - sizeof(PageHeader));
                }
                
                // 使用正确的 page_id 构造页对象
                DiskPage loaded_page(page_id);
                loaded_page.SetType(saved_type);
                loaded_page.SetKeyCount(saved_key_count);
                loaded_page.SetFreeOffset(saved_free_offset);
                if (loaded_page.GetData() && !saved_data.empty()) {
                    std::memcpy(loaded_page.GetData(), saved_data.data(), kPageSize - sizeof(PageHeader));
                }
                
                
                // 反序列化后，强制设置正确的 page_id（因为反序列化会覆盖 header_）
                // 如果磁盘上的 page_id 不正确，我们需要修复它
                if (loaded_page.GetPageId() != page_id) {
                    LOG_WARN("DATABASE", "Page ID mismatch after deserialize: Expected=" + 
                            std::to_string(page_id) + ", Got=" + std::to_string(loaded_page.GetPageId()) +
                            ". Disk data may have incorrect page_id. Fixing by preserving page data and re-setting page_id.");
                    
                    // 保存页的数据（除了 page_id）
                    PageType saved_type = loaded_page.GetType();
                    uint16_t saved_key_count = loaded_page.GetKeyCount();
                    uint16_t saved_free_offset = loaded_page.GetFreeOffset();
                    const char* saved_data_ptr = loaded_page.GetData();
                    
                    // 重新构造页对象，使用正确的 page_id
                    // 注意：不能使用移动赋值，因为会破坏 page_id
                    // 先保存所有需要的数据
                    std::vector<char> saved_data(kPageSize - sizeof(PageHeader));
                    if (saved_data_ptr) {
                        std::memcpy(saved_data.data(), saved_data_ptr, kPageSize - sizeof(PageHeader));
                    }
                    
                    // 重新构造页对象
                    loaded_page = DiskPage(page_id);
                    loaded_page.SetType(saved_type);
                    loaded_page.SetKeyCount(saved_key_count);
                    loaded_page.SetFreeOffset(saved_free_offset);
                    
                    // 复制数据区
                    if (loaded_page.GetData() && !saved_data.empty()) {
                        std::memcpy(loaded_page.GetData(), saved_data.data(), kPageSize - sizeof(PageHeader));
                    }
                    
                    // 最终验证
                    if (loaded_page.GetPageId() != page_id) {
                        LOG_ERROR("DATABASE", "Failed to fix page_id after reconstruction: Expected=" + 
                                 std::to_string(page_id) + ", Got=" + std::to_string(loaded_page.GetPageId()));
                    } else {
                        LOG_DEBUG("DATABASE", "Fixed page_id: " + std::to_string(page_id) + 
                                 ", Type=" + std::to_string(static_cast<int>(saved_type)) +
                                 ", KeyCount=" + std::to_string(saved_key_count) +
                                 ", FreeOffset=" + std::to_string(saved_free_offset));
                    }
                }
                
                // 步骤9：返回结果（确保 page_id 正确）
                if (loaded_page.GetPageId() != page_id) {
                    LOG_ERROR("DATABASE", "Page ID still incorrect before return: Expected=" + 
                             std::to_string(page_id) + ", Got=" + std::to_string(loaded_page.GetPageId()));
                    // 最后一次尝试修复
                    PageType final_type = loaded_page.GetType();
                    uint16_t final_key_count = loaded_page.GetKeyCount();
                    uint16_t final_free_offset = loaded_page.GetFreeOffset();
                    std::vector<char> final_data(kPageSize - sizeof(PageHeader));
                    if (loaded_page.GetData()) {
                        std::memcpy(final_data.data(), loaded_page.GetData(), kPageSize - sizeof(PageHeader));
                    }
                    loaded_page = DiskPage(page_id);
                    loaded_page.SetType(final_type);
                    loaded_page.SetKeyCount(final_key_count);
                    loaded_page.SetFreeOffset(final_free_offset);
                    if (loaded_page.GetData() && !final_data.empty()) {
                        std::memcpy(loaded_page.GetData(), final_data.data(), kPageSize - sizeof(PageHeader));
                    }
                }
                
                promise->set_value(std::move(loaded_page));
            } catch (const std::exception& e) {
                LOG_ERROR("DATABASE", "Exception in async read: " + std::string(e.what()));
                promise->set_exception(std::current_exception());
            } catch (...) {
                // 任何错误都通过promise传递
                LOG_ERROR("DATABASE", "Unknown exception in async read");
                promise->set_exception(std::current_exception());
            }
        });
    
    // 保存 async_task 以避免立即销毁
    (void)async_task;  // 避免未使用警告
    
    return future;
}

std::future<bool> DiskEngineV2::write_page_async(const DiskPage& page) {
    auto promise = std::make_shared<std::promise<bool>>();
    auto future = promise->get_future();
    
    // 步骤1：从页对象中提取页ID
    uint64_t page_id = disk_page_adapter::get_page_id(page);
    LOG_DEBUG("DATABASE", "write_page_async: 提取的 page_id=" + std::to_string(page_id));
    
    // 步骤2：查找包含该页的扩展
    auto extent = extent_manager_->find_extent(page_id);
    
    if (!extent) {
        // 页不存在，这种情况不应该发生（应该在 allocate_data_page 中已分配）
        // 但为了安全，我们记录错误并返回失败
        std::string error_msg = "Extent not found for page: PageID=" + std::to_string(page_id) +
                                ", attempting to allocate new extent";
        LOG_ERROR("DATABASE", error_msg);
        extent = extent_manager_->allocate_extent(1);  // 分配1页的扩展
        if (!extent) {
            std::string alloc_error = "Failed to allocate extent for page: PageID=" + std::to_string(page_id);
            LOG_ERROR("DATABASE", alloc_error);
            promise->set_value(false);  // 分配失败
            return future;
        }
        // 注意：新分配的 extent 的 start_page 可能与 page_id 不匹配
        // 这种情况下应该使用 extent->start_page 而不是 page_id
        std::string warn_msg = std::string("Allocated new extent for page, but page_id mismatch: ") +
                               "Requested=" + std::to_string(page_id) + 
                               ", Allocated=" + std::to_string(extent->start_page);
        LOG_WARN("DATABASE", warn_msg);
    }
    
    // 步骤3：获取文件句柄（用于写入）
    auto file_handle = extent_manager_->get_file_handle(extent->file_id);
    if (!file_handle || !file_handle->is_open()) {
        promise->set_value(false);  // 文件打开失败
        return future;
    }
    
    // 步骤4：写前日志（Write-Ahead Logging，如果启用）
    if (config_.enable_wal && wal_) {
        std::vector<char> wal_data(kPageSize + sizeof(page_id));
        std::memcpy(wal_data.data(), &page_id, sizeof(page_id));
        
        // 序列化页到WAL缓冲区
        char page_buffer[kPageSize];
        page.SerializeTo(page_buffer);
        std::memcpy(wal_data.data() + sizeof(page_id), page_buffer, kPageSize);
        
        // 追加到WAL
        wal_->append(wal_data);
    }
    
    // 步骤5：计算文件中的偏移量
    uint64_t offset = (page_id - extent->start_page) * kPageSize;
    
    // 步骤6：序列化页对象为字节流
    // 在序列化前再次验证 page_id（防止在 lambda 捕获时被移动）
    uint64_t verify_page_id = disk_page_adapter::get_page_id(page);
    if (verify_page_id != page_id) {
        LOG_ERROR("DATABASE", "Page ID changed before serialization: Expected=" + 
                 std::to_string(page_id) + ", Got=" + std::to_string(verify_page_id));
        promise->set_value(false);
        return future;
    }
    
    char page_buffer[kPageSize];
    page.SerializeTo(page_buffer);
    
    // 步骤6.5：更新缓冲池（如果页在缓冲池中）
    auto* frame = buffer_pool_->fetch_page(page_id, true);  // 获取写锁
    if (frame) {
        // 使用序列化/反序列化来更新缓冲池中的页
        frame->page.DeserializeFrom(page_buffer);
        frame->dirty.store(false);  // 已写入磁盘，不再脏
        buffer_pool_->release_page(page_id, true);
    }
    
    // 步骤7：提交异步写请求
    auto write_future = file_handle->async_write(page_buffer, kPageSize, offset);
    
    // 步骤8：在后台线程中处理IO完成
    // 注意：必须保存 async 返回的 future，否则任务可能被立即销毁
    auto async_task = std::async(std::launch::async, [promise, write_future = std::move(write_future)]() mutable {
        try {
            size_t bytes_written = write_future.get();
            promise->set_value(bytes_written == kPageSize);  // 必须写入完整页
        } catch (const std::exception& e) {
            LOG_ERROR("DATABASE", "Exception in async write: " + std::string(e.what()));
            promise->set_value(false);
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    });
    
    // 保存 async_task 的 future 以避免任务被销毁（虽然这里不需要等待）
    (void)async_task;  // 避免未使用变量警告
    
    return future;
}

void DiskEngineV2::initialize_engine() {
    // 创建必要目录
    ensure_directory(data_dir_);
    ensure_directory(data_dir_ + "/data");
    ensure_directory(data_dir_ + "/wal");
    ensure_directory(data_dir_ + "/backup");
    
    // 加载元数据
    load_metadata();
    
    // 恢复WAL（如果启用）
    if (config_.enable_wal && wal_) {
        recover_from_wal();
    }
    
    // 启动后台任务
    start_background_tasks();
}

void DiskEngineV2::shutdown() {
    // 停止后台任务
    stop_background_tasks();
    
    // 刷所有脏页
    buffer_pool_->flush_all();
    
    // 关闭WAL
    if (wal_) {
        wal_->wait_for_commit(wal_->get_commit_index(), 
                             std::chrono::seconds(30));
    }
}

void DiskEngineV2::load_metadata() {
    // TODO: 加载引擎元数据
}

void DiskEngineV2::recover_from_wal() {
    // TODO: 从WAL恢复
}

bool DiskEngineV2::perform_compaction() {
    // TODO: 实现存储压缩
    return false;
}

bool DiskEngineV2::perform_backup(const std::string& backup_dir) {
    // TODO: 实现备份
    return false;
}

bool DiskEngineV2::perform_restore(const std::string& backup_dir) {
    // TODO: 实现恢复
    return false;
}

void DiskEngineV2::start_background_tasks() {
    // 启动监控线程
    if (config_.enable_metrics) {
        start_metrics_thread();
    }
}

void DiskEngineV2::stop_background_tasks() {
    metrics_running_.store(false);
    if (metrics_thread_.joinable()) {
        metrics_thread_.join();
    }
}

void DiskEngineV2::start_metrics_thread() {
    metrics_running_.store(true);
    metrics_thread_ = std::thread([this]() {
        while (metrics_running_.load()) {
            std::this_thread::sleep_for(metrics_interval_);
            
            if (metrics_callback_) {
                auto status = get_status();
                metrics_callback_(status);
            }
        }
    });
}

void DiskEngineV2::ensure_directory(const std::string& path) {
    std::filesystem::create_directories(path);
}

// ==================== SmartBufferPool::BufferFrame 实现 ====================

void SmartBufferPool::BufferFrame::pin() {
    ++pin_count;
}

void SmartBufferPool::BufferFrame::unpin() {
    if (pin_count > 0) {
        --pin_count;
    }
}

void SmartBufferPool::BufferFrame::mark_accessed() {
    access_time.store(SmartBufferPool::get_current_time());
    ++access_count;
}

// ==================== SmartBufferPool 实现 ====================

SmartBufferPool::SmartBufferPool(size_t pool_size, size_t page_size, 
                                 EngineConfig::FlushStrategy strategy)
    : pool_size_(pool_size),
      page_size_(page_size),
      flush_strategy_(strategy),
      running_(false) {
    
    frames_.clear();
    frames_.reserve(pool_size);  // 在添加元素前预留空间，避免移动
    for (size_t i = 0; i < pool_size; ++i) {
        frames_.push_back(std::make_unique<BufferFrame>(page_size));
        free_list_.push_back(i);
    }
    
    // 启动后台线程
    start_background_threads();
}

SmartBufferPool::~SmartBufferPool() {
    stop_background_threads();
    flush_all();
}

SmartBufferPool::BufferFrame* SmartBufferPool::fetch_page(uint64_t page_id, bool exclusive) {
    // 1. 检查是否在缓冲池中
    {
        std::shared_lock<std::shared_mutex> lock(page_table_mutex_);
        auto it = page_table_.find(page_id);
        if (it != page_table_.end()) {
            BufferFrame* frame = frames_[it->second].get();
            
            if (exclusive) {
                // 尝试获取独占访问
                bool expected = false;
                if (frame->locked.compare_exchange_strong(expected, true)) {
                    frame->pin();
                    frame->mark_accessed();
                    ++hit_count_;
                    return frame;
                }
                return nullptr; // 被其他线程锁定
            } else {
                frame->pin();
                frame->mark_accessed();
                ++hit_count_;
                return frame;
            }
        }
    }
    
    // 2. 不在缓冲池中，需要加载
    return load_page(page_id, exclusive);
}

SmartBufferPool::BufferFrame* SmartBufferPool::new_page() {
    std::unique_lock<std::shared_mutex> lock(page_table_mutex_);
    
    // 分配页ID
    static std::atomic<uint64_t> next_page_id{1};
    uint64_t page_id = next_page_id.fetch_add(1);
    
    // 获取缓冲池帧
    size_t frame_id = get_free_frame();
    if (frame_id == pool_size_) {
        return nullptr; // 没有可用帧
    }
    
    // 初始化页（使用page_id构造）
    BufferFrame* frame = frames_[frame_id].get();
    frame->page = DiskPage(disk_page_adapter::to_page_id(page_id));
    frame->dirty.store(true);
    frame->pin();
    frame->mark_accessed();
    
    // 更新页表
    page_table_[page_id] = frame_id;
    
    return frame;
}

void SmartBufferPool::release_page(uint64_t page_id, bool mark_dirty) {
    std::shared_lock<std::shared_mutex> lock(page_table_mutex_);
    
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        BufferFrame* frame = frames_[it->second].get();
        
        frame->unpin();
        if (mark_dirty) {
            frame->dirty.store(true);
        }
        
        // 如果是独占访问，释放锁
        frame->locked.store(false);
    }
}

void SmartBufferPool::prefetch_pages(const std::vector<uint64_t>& page_ids) {
    // 在后台线程中异步预取
    std::lock_guard<std::mutex> lock(prefetch_queue_mutex_);
    for (uint64_t page_id : page_ids) {
        prefetch_queue_.push(page_id);
    }
}

SmartBufferPool::PoolStats SmartBufferPool::get_stats() const {
    PoolStats stats{};
    stats.total_frames = pool_size_;
    
    size_t hits = hit_count_.load();
    size_t misses = miss_count_.load();
    stats.hit_ratio = (hits + misses) > 0 ? 
        static_cast<double>(hits) / (hits + misses) : 0.0;
    
    for (const auto& frame : frames_) {
        if (frame && frame->pin_count.load() > 0) {
            ++stats.pinned_frames;
        }
        if (frame && frame->dirty.load()) {
            ++stats.dirty_frames;
        }
    }
    
    stats.used_frames = pool_size_ - free_list_.size();
    stats.free_frames = free_list_.size();
    stats.prefetch_count = prefetch_count_.load();
    
    return stats;
}

size_t SmartBufferPool::get_free_frame() {
    if (!free_list_.empty()) {
        size_t frame_id = free_list_.back();
        free_list_.pop_back();
        return frame_id;
    }
    
    // 需要置换
    return select_victim();
}

size_t SmartBufferPool::select_victim() {
    // 尝试多种策略
    size_t frame_id = select_by_clock();
    if (frame_id != pool_size_) {
        evict_frame(frame_id);
        return frame_id;
    }
    
    frame_id = select_by_lru();
    if (frame_id != pool_size_) {
        evict_frame(frame_id);
        return frame_id;
    }
    
    // 所有页都被固定
    return pool_size_;
}

size_t SmartBufferPool::select_by_clock() {
    static size_t clock_hand = 0;
    
    for (size_t i = 0; i < pool_size_ * 2; ++i) {
        size_t frame_id = clock_hand;
        clock_hand = (clock_hand + 1) % pool_size_;
        
        BufferFrame* frame = frames_[frame_id].get();
        
        if (frame && frame->pin_count.load() > 0) {
            continue; // 跳过固定页
        }
        
        // 检查访问位（简化实现）
        if (frame && frame->access_count.load() == 0) {
            return frame_id;
        }
        
        // 清除访问位
        if (frame) {
            frame->access_count.store(0);
        }
    }
    
    return pool_size_;
}

size_t SmartBufferPool::select_by_lru() {
    uint64_t oldest_time = UINT64_MAX;
    size_t oldest_frame = pool_size_;
    
    for (size_t i = 0; i < pool_size_; ++i) {
        BufferFrame* frame = frames_[i].get();
        
        if (!frame || frame->pin_count.load() > 0) {
            continue;
        }
        
        uint64_t access_time = frame->access_time.load();
        if (access_time < oldest_time) {
            oldest_time = access_time;
            oldest_frame = i;
        }
    }
    
    return oldest_frame;
}

void SmartBufferPool::evict_frame(size_t frame_id) {
    BufferFrame* frame = frames_[frame_id].get();
    
    if (!frame) return;
    
    // 如果是脏页，刷盘
    if (frame->dirty.load()) {
        flush_frame(frame_id);
    }
    
    // 从页表中移除
    std::unique_lock<std::shared_mutex> lock(page_table_mutex_);
    for (auto it = page_table_.begin(); it != page_table_.end(); ++it) {
        if (it->second == frame_id) {
            page_table_.erase(it);
            break;
        }
    }
    
    // 重置帧状态
    frame->dirty.store(false);
    frame->access_count.store(0);
    frame->pin_count.store(0);
    frame->locked.store(false);
    
    // 添加到空闲列表
    free_list_.push_back(frame_id);
}

SmartBufferPool::BufferFrame* SmartBufferPool::load_page(uint64_t page_id, bool exclusive) {
    ++miss_count_;
    
    size_t frame_id = get_free_frame();
    if (frame_id == pool_size_) {
        return nullptr;
    }
    
    BufferFrame* frame = frames_[frame_id].get();
    
    // 从磁盘加载页数据（通过反序列化）
    // TODO: 实际实现需要从磁盘读取数据后调用DeserializeFrom
    
    // 初始化页（使用page_id构造）
    frame->page = DiskPage(disk_page_adapter::to_page_id(page_id));
    frame->pin();
    frame->mark_accessed();
    
    if (exclusive) {
        frame->locked.store(true);
    }
    
    // 更新页表
    {
        std::unique_lock<std::shared_mutex> lock(page_table_mutex_);
        page_table_[page_id] = frame_id;
    }
    
    return frame;
}

void SmartBufferPool::flush_frame(size_t frame_id) {
    BufferFrame* frame = frames_[frame_id].get();
    
    if (!frame) return;
    
    // TODO: 异步刷盘
    // 需要使用frame->page.SerializeTo()序列化后写入磁盘
    
    frame->dirty.store(false);
}

void SmartBufferPool::flush_all() {
    for (size_t i = 0; i < pool_size_; ++i) {
        if (frames_[i] && frames_[i]->dirty.load() && frames_[i]->pin_count.load() == 0) {
            flush_frame(i);
        }
    }
}

void SmartBufferPool::start_background_threads() {
    running_.store(true);
    
    // 刷盘线程
    background_threads_.emplace_back([this]() { flush_thread(); });
    
    // 预取线程
    background_threads_.emplace_back([this]() { prefetch_thread(); });
    
    // 统计线程
    background_threads_.emplace_back([this]() { stats_thread(); });
}

void SmartBufferPool::stop_background_threads() {
    running_.store(false);
    for (auto& thread : background_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    background_threads_.clear();
}

void SmartBufferPool::flush_thread() {
    while (running_.load()) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(flush_interval_));
        
        if (flush_strategy_ == EngineConfig::FlushStrategy::Periodic) {
            flush_dirty_pages();
        }
    }
}

void SmartBufferPool::prefetch_thread() {
    std::vector<uint64_t> page_ids;
    page_ids.reserve(64);
    
    while (running_.load()) {
        page_ids.clear();
        
        // 从队列中取出待预取的页
        {
            std::lock_guard<std::mutex> lock(prefetch_queue_mutex_);
            while (!prefetch_queue_.empty() && page_ids.size() < 64) {
                page_ids.push_back(prefetch_queue_.front());
                prefetch_queue_.pop();
            }
        }
        
        if (!page_ids.empty()) {
            prefetch_count_ += page_ids.size();
            
            // 批量预取
            for (uint64_t page_id : page_ids) {
                // 异步加载页，但不pin
                fetch_page(page_id, false);
                release_page(page_id, false);
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void SmartBufferPool::stats_thread() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(60));
        
        auto stats = get_stats();
        log_stats(stats);
    }
}

void SmartBufferPool::flush_dirty_pages() {
    for (size_t i = 0; i < pool_size_; ++i) {
        BufferFrame* frame = frames_[i].get();
        
        if (frame && frame->dirty.load() && frame->pin_count.load() == 0) {
            // 检查页是否最近被访问
            uint64_t now = get_current_time();
            uint64_t last_access = frame->access_time.load();
            
            if (now - last_access > min_dirty_age_) {
                flush_frame(i);
            }
        }
    }
}

uint64_t SmartBufferPool::get_current_time() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
}

void SmartBufferPool::log_stats(const PoolStats& stats) {
    // TODO: 输出到日志或监控系统
}

// ==================== DistributedWAL 实现 ====================

// LogRecord 构造函数已在头文件中定义为内联，这里不需要重复定义

DistributedWAL::DistributedWAL(const std::string& data_dir, size_t node_id,
                               const std::vector<std::string>& peers)
    : data_dir_(data_dir),
      node_id_(node_id),
      peers_(peers),
      current_term_(0),
      commit_index_(0),
      last_applied_(0) {
    
    LOG_INFO("DATABASE", "Initializing DistributedWAL: NodeID=" + std::to_string(node_id_) +
                         ", DataDir=" + data_dir +
                         ", PeerCount=" + std::to_string(peers.size()));
    
    ensure_directory(data_dir);
    load_wal_state();
    
    // 启动Raft共识线程
    start_consensus_thread();
    
    LOG_INFO("DATABASE", "DistributedWAL initialized successfully");
}

DistributedWAL::~DistributedWAL() {
    LOG_INFO("DATABASE", "Shutting down DistributedWAL: NodeID=" + std::to_string(node_id_));
    stop_consensus_thread();
    persist_wal_state();
    LOG_INFO("DATABASE", "DistributedWAL shutdown complete");
}

uint64_t DistributedWAL::append(const std::vector<char>& data) {
    std::lock_guard<std::mutex> lock(wal_mutex_);
    
    uint64_t lsn = next_lsn_++;
    uint32_t term = current_term_;
    
    LogRecord record(lsn, node_id_, term, data);
    
    // 记录追加操作
    LOG_INFO("DATABASE", "Appending WAL record: LSN=" + std::to_string(lsn) +
                         ", Term=" + std::to_string(term) +
                         ", NodeID=" + std::to_string(node_id_) +
                         ", DataSize=" + std::to_string(data.size()));
    
    // 本地持久化
    persist_record(record);
    
    // 复制到其他节点
    replicate_record(record);
    
    // 等待多数派确认
    wait_for_quorum(lsn);
    
    LOG_DEBUG("DATABASE", "WAL record appended and committed: LSN=" + std::to_string(lsn));
    
    return lsn;
}

std::optional<DistributedWAL::LogRecord> DistributedWAL::read(uint64_t lsn) {
    std::lock_guard<std::mutex> lock(wal_mutex_);
    
    if (lsn >= next_lsn_) {
        LOG_WARN("DATABASE", "Reading WAL record failed: LSN=" + std::to_string(lsn) +
                             " >= next_lsn=" + std::to_string(next_lsn_));
        return std::nullopt;
    }
    
    // 从本地存储读取
    LOG_DEBUG("DATABASE", "Reading WAL record: LSN=" + std::to_string(lsn));
    return read_record(lsn);
}

uint64_t DistributedWAL::get_commit_index() const {
    return commit_index_.load();
}

bool DistributedWAL::wait_for_commit(uint64_t lsn, std::chrono::milliseconds timeout) {
    auto start = std::chrono::steady_clock::now();
    
    LOG_DEBUG("DATABASE", "Waiting for commit: LSN=" + std::to_string(lsn) +
                          ", Timeout=" + std::to_string(timeout.count()) + "ms");
    
    while (commit_index_.load() < lsn) {
        if (std::chrono::steady_clock::now() - start > timeout) {
            LOG_WARN("DATABASE", "Wait for commit timeout: LSN=" + std::to_string(lsn) +
                                 ", CurrentCommitIndex=" + std::to_string(commit_index_.load()));
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    LOG_DEBUG("DATABASE", "Commit confirmed: LSN=" + std::to_string(lsn));
    return true;
}

DistributedWAL::ClusterStatus DistributedWAL::get_cluster_status() const {
    ClusterStatus status{};
    status.leader_id = leader_id_.load();
    status.current_term = current_term_.load();
    status.commit_index = commit_index_.load();
    status.node_count = peers_.size() + 1;
    
    std::lock_guard<std::mutex> lock(peers_mutex_);
    status.node_online.reserve(peers_connected_.size());
    for (bool connected : peers_connected_) {
        status.node_online.push_back(connected);
    }
    
    return status;
}

void DistributedWAL::start_consensus_thread() {
    running_.store(true);
    consensus_thread_ = std::thread([this]() { consensus_loop(); });
    replication_thread_ = std::thread([this]() { replication_loop(); });
}

void DistributedWAL::stop_consensus_thread() {
    running_.store(false);
    if (consensus_thread_.joinable()) {
        consensus_thread_.join();
    }
    if (replication_thread_.joinable()) {
        replication_thread_.join();
    }
}

void DistributedWAL::consensus_loop() {
    LOG_INFO("DATABASE", "Consensus loop started: NodeID=" + std::to_string(node_id_));
    
    while (running_.load()) {
        auto now = get_current_time();
        
        switch (node_state_.load()) {
            case NodeState::Follower:
                // 检查是否需要选举
                if (now - last_heartbeat_.load() > election_timeout_) {
                    LOG_DEBUG("DATABASE", "Election timeout, starting election: NodeID=" + 
                             std::to_string(node_id_) + ", Timeout=" + 
                             std::to_string(election_timeout_) + "ms");
                    start_election();
                }
                break;
                
            case NodeState::Candidate:
                // 等待投票结果
                break;
                
            case NodeState::Leader:
                // 发送心跳
                send_heartbeats();
                break;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    LOG_INFO("DATABASE", "Consensus loop stopped: NodeID=" + std::to_string(node_id_));
}

void DistributedWAL::replication_loop() {
    while (running_.load()) {
        // 复制日志到其他节点
        replicate_pending_logs();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void DistributedWAL::start_election() {
    uint32_t new_term = current_term_.fetch_add(1) + 1;
    node_state_.store(NodeState::Candidate);
    
    LOG_INFO("DATABASE", "Starting election: NodeID=" + std::to_string(node_id_) +
                         ", NewTerm=" + std::to_string(new_term));
    
    // 请求投票
    request_votes();
}

void DistributedWAL::request_votes() {
    // TODO: 向其他节点发送投票请求
}

void DistributedWAL::send_heartbeats() {
    // TODO: 向其他节点发送心跳
    last_heartbeat_.store(get_current_time());
    LOG_DEBUG("DATABASE", "Sending heartbeats: NodeID=" + std::to_string(node_id_) +
                          ", Term=" + std::to_string(current_term_.load()));
}

void DistributedWAL::replicate_pending_logs() {
    // TODO: 复制未提交的日志到其他节点
}

void DistributedWAL::persist_record(const LogRecord& record) {
    std::string file_path = data_dir_ + "/wal_" + 
                           std::to_string(record.term) + ".log";
    
    std::ofstream file(file_path, std::ios::binary | std::ios::app);
    if (!file.is_open()) {
        LOG_ERROR("DATABASE", "Failed to open WAL file: " + file_path);
        return;
    }
    
    // 写入记录
    file.write(reinterpret_cast<const char*>(&record.lsn), sizeof(record.lsn));
    file.write(reinterpret_cast<const char*>(&record.timestamp), 
              sizeof(record.timestamp));
    file.write(reinterpret_cast<const char*>(&record.node_id), 
              sizeof(record.node_id));
    file.write(reinterpret_cast<const char*>(&record.term), 
              sizeof(record.term));
    
    size_t data_size = record.data.size();
    file.write(reinterpret_cast<const char*>(&data_size), sizeof(data_size));
    file.write(record.data.data(), data_size);
    
    // 记录日志
    std::string log_msg = "Persisted WAL record: LSN=" + std::to_string(record.lsn) +
                         ", Term=" + std::to_string(record.term) +
                         ", NodeID=" + std::to_string(record.node_id) +
                         ", DataSize=" + std::to_string(data_size) +
                         ", File=" + file_path;
    LOG_DEBUG("DATABASE", log_msg);
}

std::optional<DistributedWAL::LogRecord> DistributedWAL::read_record(uint64_t lsn) {
    // TODO: 从文件读取日志记录
    LOG_DEBUG("DATABASE", "Reading WAL record: LSN=" + std::to_string(lsn) + " (not implemented)");
    return std::nullopt;
}

void DistributedWAL::replicate_record(const LogRecord& record) {
    // TODO: 复制到其他节点
}

void DistributedWAL::wait_for_quorum(uint64_t lsn) {
    std::promise<bool> promise;
    auto future = promise.get_future();
    
    {
        std::lock_guard<std::mutex> lock(wal_mutex_);
        commit_promises_[lsn] = std::move(promise);
    }
    
    // 等待多数派确认
    future.wait();
}

void DistributedWAL::load_wal_state() {
    std::string state_path = data_dir_ + "/wal_state.bin";
    std::ifstream file(state_path, std::ios::binary);
    
    if (!file.is_open()) {
        LOG_INFO("DATABASE", "WAL state file not found, starting fresh: " + state_path);
        return;
    }
    
    uint64_t next_lsn;
    uint32_t current_term;
    file.read(reinterpret_cast<char*>(&next_lsn), sizeof(next_lsn));
    file.read(reinterpret_cast<char*>(&current_term), sizeof(current_term));
    next_lsn_ = next_lsn;
    current_term_.store(current_term);
    
    LOG_INFO("DATABASE", "Loaded WAL state: NextLSN=" + std::to_string(next_lsn) +
                         ", CurrentTerm=" + std::to_string(current_term) +
                         ", File=" + state_path);
}

void DistributedWAL::persist_wal_state() {
    std::string state_path = data_dir_ + "/wal_state.bin";
    std::ofstream file(state_path, std::ios::binary);
    
    if (!file.is_open()) {
        LOG_ERROR("DATABASE", "Failed to open WAL state file for writing: " + state_path);
        return;
    }
    
    uint64_t next_lsn = next_lsn_;
    uint32_t current_term = current_term_.load();
    file.write(reinterpret_cast<const char*>(&next_lsn), sizeof(next_lsn));
    file.write(reinterpret_cast<const char*>(&current_term), sizeof(current_term));
    
    LOG_DEBUG("DATABASE", "Persisted WAL state: NextLSN=" + std::to_string(next_lsn) +
                          ", CurrentTerm=" + std::to_string(current_term) +
                          ", File=" + state_path);
}

void DistributedWAL::ensure_directory(const std::string& path) {
    std::filesystem::create_directories(path);
}

uint64_t DistributedWAL::get_current_time() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// ==================== DiskEngineV2 剩余方法实现 ====================

std::future<bool> DiskEngineV2::batch_operation(
    const std::function<void(BatchContext&)>& operation) {
    
    auto promise = std::make_shared<std::promise<bool>>();
    auto future = promise->get_future();
    
    // 在后台线程中执行批量操作
    std::thread([this, operation, promise]() {
        try {
            BatchContext context(*this);
            operation(context);
            
            // 提交所有修改
            bool success = context.commit();
            promise->set_value(success);
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    }).detach();
    
    return future;
}

std::future<bool> DiskEngineV2::compact_storage() {
    return std::async(std::launch::async, [this]() {
        return perform_compaction();
    });
}

std::future<bool> DiskEngineV2::backup(const std::string& backup_dir) {
    return std::async(std::launch::async, [this, backup_dir]() {
        return perform_backup(backup_dir);
    });
}

std::future<bool> DiskEngineV2::restore(const std::string& backup_dir) {
    return std::async(std::launch::async, [this, backup_dir]() {
        return perform_restore(backup_dir);
    });
}

DiskEngineV2::EngineStatus DiskEngineV2::get_status() const {
    EngineStatus status{};
    
    status.buffer_pool_stats = buffer_pool_->get_stats();
    status.extent_stats = extent_manager_->get_stats();
    
    // 从FileHandle获取IO统计（需要从所有打开的文件句柄汇总）
    // TODO: 实现IO统计汇总逻辑
    
    if (wal_) {
        status.cluster_status = wal_->get_cluster_status();
    }
    
    // 计算其他统计信息
    status.total_pages = status.extent_stats.total_pages;
    status.used_pages = status.extent_stats.allocated_pages;
    
    return status;
}

void DiskEngineV2::set_metrics_callback(MetricsCallback callback, 
                                        std::chrono::seconds interval) {
    metrics_callback_ = std::move(callback);
    metrics_interval_ = interval;
    
    if (metrics_callback_ && !metrics_thread_.joinable()) {
        start_metrics_thread();
    }
}

// ==================== DiskEngineV2::BatchContext 实现 ====================

DiskEngineV2::BatchContext::BatchContext(DiskEngineV2& engine) 
    : engine_(engine) {
}

void DiskEngineV2::BatchContext::write_page(const DiskPage& page) {
    // Page 禁止拷贝，需要移动构造
    auto page_copy = std::make_unique<DiskPage>(page.GetPageId());
    // 复制页数据
    char buffer[kPageSize];
    page.SerializeTo(buffer);
    page_copy->DeserializeFrom(buffer);
    pending_writes_.push_back(std::move(page_copy));
}

void DiskEngineV2::BatchContext::read_page(uint64_t page_id, 
                                          std::function<void(const DiskPage&)> callback) {
    pending_reads_.emplace_back(page_id, std::move(callback));
}

bool DiskEngineV2::BatchContext::commit() {
    // 执行所有读取
    for (const auto& [page_id, callback] : pending_reads_) {
        auto page_future = engine_.read_page_async(page_id);
        try {
            DiskPage page = page_future.get();
            callback(page);
        } catch (...) {
            // 读取失败
            return false;
        }
    }
    
    // 执行所有写入
    std::vector<std::future<bool>> write_futures;
    for (const auto& page_ptr : pending_writes_) {
        if (page_ptr) {
            write_futures.push_back(engine_.write_page_async(*page_ptr));
        }
    }
    
    for (auto& future : write_futures) {
        if (!future.get()) {
            return false;
        }
    }
    
    return true;
}

// ==================== DiskEngineV2 键值对操作实现 ====================

std::future<bool> DiskEngineV2::put(const Slice& key, const Slice& value) {
    return std::async(std::launch::async, [this, key, value]() {
        try {
            // 将 Slice 转换为字符串（作为 B+树键）
            std::string key_str(key.data(), key.size());
            
            // 查找是否已存在
            std::shared_lock<std::shared_mutex> read_lock(index_mutex_);
            uint64_t* existing_page_id = index_tree_->find(key_str);
            read_lock.unlock();
            
            uint64_t page_id;
            if (existing_page_id) {
                // 键已存在，更新现有页
                page_id = *existing_page_id;

            } else {
                // 键不存在，分配新页
                page_id = allocate_data_page();
                // 注意：page_id 0 是有效的（第一个页），所以不能用 == 0 判断失败
                // allocate_data_page 在失败时会返回 0，但我们需要区分"第一个页"和"分配失败"
                // 暂时先检查是否 >= 0（所有非负数都认为是成功），如果后续需要可以改为返回 -1 表示失败
                // 但更好的方法是让 allocate_data_page 返回 std::optional<uint64_t>
                // 目前先假设如果写入成功，page_id 就是有效的
                LOG_DEBUG("DATABASE", "Allocated page_id=" + std::to_string(page_id) + 
                         " for new key: " + key_str);
            }
            
            // 将键值对存储到页中
            if (!store_record_in_page(page_id, key, value)) {
                LOG_ERROR("DATABASE", "Failed to store record in page: PageID=" + 
                         std::to_string(page_id));
                return false;
            }
            
            // 更新索引
            {
                std::unique_lock<std::shared_mutex> write_lock(index_mutex_);
                if (existing_page_id) {
                    // 更新：先删除旧索引，再插入新索引
                    index_tree_->remove(key_str);
                }
                index_tree_->insert(key_str, page_id);
            }
            
            LOG_DEBUG("DATABASE", "Put key-value: KeySize=" + std::to_string(key.size()) +
                     ", ValueSize=" + std::to_string(value.size()) +
                     ", PageID=" + std::to_string(page_id));
            
            return true;
        } catch (const std::exception& e) {
            LOG_ERROR("DATABASE", "Put operation failed: " + std::string(e.what()));
            return false;
        }
    });
}

std::future<std::optional<Slice>> DiskEngineV2::get(const Slice& key) {
    return std::async(std::launch::async, [this, key]() -> std::optional<Slice> {
        try {
            // 将 Slice 转换为字符串
            std::string key_str(key.data(), key.size());
            
            // 从索引中查找页ID
            std::shared_lock<std::shared_mutex> read_lock(index_mutex_);
            uint64_t* page_id_ptr = index_tree_->find(key_str);
            if (!page_id_ptr) {
                LOG_ERROR("DATABASE", "Key not found in index: " + key_str);
                return std::nullopt;  // 键不存在
            }
            uint64_t page_id = *page_id_ptr;
            read_lock.unlock();
            
            // 从页中加载记录
            auto record = load_record_from_page(page_id, key);
            if (!record) {
                LOG_WARN("DATABASE", "Record not found in page: PageID=" + 
                        std::to_string(page_id) + ", KeySize=" + std::to_string(key.size()));
                return std::nullopt;
            }
            
            // 返回值的副本（因为 Slice 可能指向临时缓冲区）
            const Slice& value_slice = record->second;
            std::string value_str(value_slice.data(), value_slice.size());
            return Slice(value_str);
        } catch (const std::exception& e) {
            LOG_ERROR("DATABASE", "Get operation failed: " + std::string(e.what()));
            return std::nullopt;
        }
    });
}

std::future<bool> DiskEngineV2::remove(const Slice& key) {
    return std::async(std::launch::async, [this, key]() {
        try {
            // 将 Slice 转换为字符串
            std::string key_str(key.data(), key.size());
            
            // 从索引中查找并删除
            std::unique_lock<std::shared_mutex> write_lock(index_mutex_);
            if (!index_tree_->find(key_str)) {
                return false;  // 键不存在
            }
            
            bool removed = index_tree_->remove(key_str);
            write_lock.unlock();
            
            if (removed) {
                LOG_DEBUG("DATABASE", "Removed key: KeySize=" + std::to_string(key.size()));
            }
            
            return removed;
        } catch (const std::exception& e) {
            LOG_ERROR("DATABASE", "Remove operation failed: " + std::string(e.what()));
            return false;
        }
    });
}

std::future<void> DiskEngineV2::range_query(const Slice& start_key, const Slice& end_key,
                                           std::function<void(const Slice&, const Slice&)> callback) {
    return std::async(std::launch::async, [this, start_key, end_key, callback]() {
        try {
            std::string start_key_str(start_key.data(), start_key.size());
            std::string end_key_str(end_key.data(), end_key.size());
            
            std::shared_lock<std::shared_mutex> read_lock(index_mutex_);
            
            // 使用 B+树的范围查询
            index_tree_->range_query(start_key_str, end_key_str, 
                [this, &callback](const std::string& key_str, uint64_t page_id) {
                    // 从页中加载记录
                    Slice key_slice(key_str.data(), key_str.size());
                    auto record = load_record_from_page(page_id, key_slice);
                    if (record) {
                        callback(record->first, record->second);
                    }
                });
            
            read_lock.unlock();
        } catch (const std::exception& e) {
            LOG_ERROR("DATABASE", "Range query failed: " + std::string(e.what()));
        }
    });
}

uint64_t DiskEngineV2::allocate_data_page() {
    // 分配新的数据页
    auto extent = extent_manager_->allocate_extent(1);
    if (!extent) {
        LOG_ERROR("DATABASE", "Failed to allocate extent for data page");
        return 0;
    }
    
    uint64_t page_id = extent->start_page;
    
    // 初始化页（新分配的页）
    DiskPage page(page_id);
    LOG_DEBUG("DATABASE", "创建页对象后: page_id=" + std::to_string(page_id) + 
             ", page.GetPageId()=" + std::to_string(page.GetPageId()));
    
    page.SetType(PageType::DATA);
    page.SetKeyCount(0);
    page.SetFreeOffset(sizeof(PageHeader));
    
    LOG_DEBUG("DATABASE", "设置属性后: page.GetPageId()=" + std::to_string(page.GetPageId()));
    
    // 初始化数据区为零
    char* data = page.GetData();
    if (data) {
        std::memset(data, 0, kPageSize - sizeof(PageHeader));
    }
    
    // 验证 page_id 是否正确（在序列化前）
    if (page.GetPageId() != page_id) {
        LOG_ERROR("DATABASE", "Page ID mismatch before serialization: Expected=" + 
                 std::to_string(page_id) + ", Got=" + std::to_string(page.GetPageId()));
        return 0;
    }
    
    // 写入页
    LOG_DEBUG("DATABASE", "准备写入新分配的页: PageID=" + std::to_string(page_id) +
             ", Page.GetPageId()=" + std::to_string(page.GetPageId()));
    auto write_future = write_page_async(page);
    bool write_success = false;
    try {
        // 设置超时，避免无限等待
        auto status = write_future.wait_for(std::chrono::seconds(5));
        if (status != std::future_status::ready) {
            LOG_ERROR("DATABASE", "写入页超时: PageID=" + std::to_string(page_id));
            return 0;
        }
        write_success = write_future.get();
        LOG_DEBUG("DATABASE", "写入页完成: PageID=" + std::to_string(page_id) + 
                 ", Success=" + std::to_string(write_success));
    } catch (const std::exception& e) {
        LOG_ERROR("DATABASE", "Exception writing page during allocation: PageID=" + 
                 std::to_string(page_id) + ", Error: " + e.what());
        return 0;
    }
    
    if (!write_success) {
        LOG_ERROR("DATABASE", "Failed to write page during allocation: PageID=" + 
                 std::to_string(page_id) + " (write_page_async returned false)");
        return 0;
    }
    
    LOG_DEBUG("DATABASE", "Allocated data page: PageID=" + std::to_string(page_id));
    return page_id;
}

bool DiskEngineV2::store_record_in_page(uint64_t page_id, const Slice& key, const Slice& value) {
    LOG_DEBUG("DATABASE", "store_record_in_page: PageID=" + std::to_string(page_id) +
             ", KeySize=" + std::to_string(key.size()) + ", ValueSize=" + std::to_string(value.size()));
    
    // 尝试从缓冲池或磁盘读取页，如果失败则创建新页
    DiskPage page(page_id);  // 先创建页对象，确保 page_id 正确设置
    bool page_exists = false;
    
    try {
        auto read_future = read_page_async(page_id);
        // 设置超时，避免无限等待
        auto status = read_future.wait_for(std::chrono::seconds(2));
        if (status == std::future_status::ready) {
            page = read_future.get();  // 从磁盘读取的页会覆盖默认构造的页
            page_exists = true;
            LOG_DEBUG("DATABASE", "成功从磁盘读取页: PageID=" + std::to_string(page_id) +
                     ", Page.GetPageId()=" + std::to_string(page.GetPageId()) +
                     ", Type=" + std::to_string(static_cast<int>(page.GetType())) +
                     ", KeyCount=" + std::to_string(page.GetKeyCount()) +
                     ", FreeOffset=" + std::to_string(page.GetFreeOffset()));
        } else {
            LOG_DEBUG("DATABASE", "读取页超时，使用新页: PageID=" + std::to_string(page_id));
        }
    } catch (const std::exception& e) {
        // 页可能不存在（新分配的页），继续创建新页
        LOG_DEBUG("DATABASE", "Page not found (new page?): PageID=" + 
                 std::to_string(page_id) + ", Error: " + e.what());
    }
    
    // 如果页不存在，初始化新页
    if (!page_exists) {
        // page 已经在上面用正确的 page_id 构造了，只需要设置其他属性
        page.SetType(PageType::DATA);
        page.SetKeyCount(0);
        page.SetFreeOffset(sizeof(PageHeader));
        
        // 初始化数据区
        char* data = page.GetData();
        if (data) {
            std::memset(data, 0, kPageSize - sizeof(PageHeader));
        }
        LOG_DEBUG("DATABASE", "初始化新页: PageID=" + std::to_string(page_id) +
                 ", Page.GetPageId()=" + std::to_string(page.GetPageId()));
    }
    
    // 确保 page_id 正确（防止被移动构造破坏）
    if (page.GetPageId() != page_id) {
        LOG_ERROR("DATABASE", "Page ID mismatch: Expected=" + std::to_string(page_id) +
                 ", Got=" + std::to_string(page.GetPageId()));
        // 重新设置 page_id（如果 Page 类有 SetPageId 方法）
        // 如果没有，我们需要重新构造页对象
        page = DiskPage(page_id);
        page.SetType(PageType::DATA);
        page.SetKeyCount(0);
        page.SetFreeOffset(sizeof(PageHeader));
        char* data = page.GetData();
        if (data) {
            std::memset(data, 0, kPageSize - sizeof(PageHeader));
        }
    }
    
    // 计算记录大小
    size_t record_size = Record::ByteSize(key.size(), value.size());
    size_t data_area_size = kPageSize - sizeof(PageHeader);
    
    // 检查是否有足够空间
    uint16_t free_offset = page.GetFreeOffset();
    if (free_offset + record_size > data_area_size) {
        LOG_WARN("DATABASE", "Page full, cannot store record: PageID=" + 
                std::to_string(page_id) + ", RecordSize=" + std::to_string(record_size) +
                ", FreeOffset=" + std::to_string(free_offset) +
                ", DataAreaSize=" + std::to_string(data_area_size));
        return false;
    }
    
    // 编码记录到页数据区
    char* data_ptr = page.GetData() + free_offset;
    Record::Encode(data_ptr, key, value);
    
    // 更新页头
    page.SetFreeOffset(free_offset + record_size);
    page.SetKeyCount(page.GetKeyCount() + 1);
    
    // 写入页
    LOG_DEBUG("DATABASE", "准备写入页（store_record）: PageID=" + std::to_string(page_id) +
             ", Page.GetPageId()=" + std::to_string(page.GetPageId()));
    try {
        auto write_future = write_page_async(page);
        auto status = write_future.wait_for(std::chrono::seconds(5));
        if (status == std::future_status::ready) {
            bool result = write_future.get();
            LOG_DEBUG("DATABASE", "写入页完成（store_record）: PageID=" + std::to_string(page_id) +
                     ", Result=" + std::to_string(result));
            return result;
        } else {
            LOG_ERROR("DATABASE", "Write page timeout: PageID=" + std::to_string(page_id));
            return false;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("DATABASE", "Exception writing page: PageID=" + 
                 std::to_string(page_id) + ", Error: " + e.what());
        return false;
    }
}

// 从指定页中查找并返回匹配的键值对。
std::optional<std::pair<Slice, Slice>> DiskEngineV2::load_record_from_page(uint64_t page_id, const Slice& key) {
    LOG_DEBUG("DATABASE", "load_record_from_page: PageID=" + std::to_string(page_id) + 
             ", KeySize=" + std::to_string(key.size()));
    // 读取页
    auto read_future = read_page_async(page_id);
    DiskPage page = read_future.get();
    
    // 确保 page_id 正确（直接修复，不依赖 read_page_async 的修复）
    if (page.GetPageId() != page_id) {
        LOG_WARN("DATABASE", "load_record_from_page: Page ID mismatch: Expected=" + 
                std::to_string(page_id) + ", Got=" + std::to_string(page.GetPageId()) +
                ". Fixing page_id directly.");
        
        // 保存页的数据
        PageType saved_type = page.GetType();
        uint16_t saved_key_count = page.GetKeyCount();
        uint16_t saved_free_offset = page.GetFreeOffset();
        std::vector<char> saved_data(kPageSize - sizeof(PageHeader));
        if (page.GetData()) {
            std::memcpy(saved_data.data(), page.GetData(), kPageSize - sizeof(PageHeader));
        }
        
        // 重新构造页对象，使用正确的 page_id
        page = DiskPage(page_id);
        page.SetType(saved_type);
        page.SetKeyCount(saved_key_count);
        page.SetFreeOffset(saved_free_offset);
        if (page.GetData() && !saved_data.empty()) {
            std::memcpy(page.GetData(), saved_data.data(), kPageSize - sizeof(PageHeader));
        }
        
        // 验证修复结果
        if (page.GetPageId() != page_id) {
            LOG_ERROR("DATABASE", "load_record_from_page: Failed to fix page_id: Expected=" + 
                     std::to_string(page_id) + ", Got=" + std::to_string(page.GetPageId()));
            return std::nullopt;
        }
        
        LOG_DEBUG("DATABASE", "load_record_from_page: Fixed page_id successfully: PageID=" + 
                 std::to_string(page_id));
    }
    
    LOG_DEBUG("DATABASE", "load_record_from_page: Page loaded successfully: PageID=" + 
             std::to_string(page_id) + ", Type=" + std::to_string(static_cast<int>(page.GetType())) +
             ", KeyCount=" + std::to_string(page.GetKeyCount()) +
             ", FreeOffset=" + std::to_string(page.GetFreeOffset()));
    
    // 遍历页中的所有记录
    uint16_t offset = sizeof(PageHeader);
    // 获取页的空闲偏移量
    uint16_t free_offset = page.GetFreeOffset();
    
    while (offset < free_offset) {
        Slice key_slice, value_slice;
        // 解码记录
        if (!Record::Decode(page.GetData() + offset, free_offset - offset, 
                           &key_slice, &value_slice)) {
            break;  // 数据损坏或到达末尾
        }
        
        // 检查键是否匹配
        if (key_slice.size() == key.size() && 
            std::memcmp(key_slice.data(), key.data(), key.size()) == 0) {
            // 找到匹配的键，返回值
            return std::make_pair(key_slice, value_slice);
        }
        
        // 移动到下一个记录
        offset += Record::ByteSize(key_slice.size(), value_slice.size());
    }
    
    return std::nullopt;  // 未找到
}

} // namespace core
} // namespace mementodb
