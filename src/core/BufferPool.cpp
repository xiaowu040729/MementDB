// File: src/core/BufferPool.cpp

#include "BufferPool.h"
#include "FileManager.hpp"
#include <algorithm>
#include <thread>
#include <chrono>
#include <cstring>

namespace mementodb {
namespace core {

// ==================== LRUReplacer 实现 ====================

LRUReplacer::LRUReplacer(size_t num_pages)
    : num_pages_(num_pages), head_(nullptr), tail_(nullptr) {
    pinned_.resize(num_pages, false);
}

void LRUReplacer::record_access(size_t frame_id) {
    if (frame_id >= num_pages_) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = node_map_.find(frame_id);
    if (it != node_map_.end()) {
        // 移动到头部
        move_to_head(it->second.get());
    } else if (!pinned_[frame_id]) {
        // 创建新节点并添加到头部
        auto node = std::make_unique<Node>(frame_id);
        Node* node_ptr = node.get();
        node_map_[frame_id] = std::move(node);
        add_to_head(node_ptr);
    }
}

void LRUReplacer::pin(size_t frame_id) {
    if (frame_id >= num_pages_) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    pinned_[frame_id] = true;
    
    // 从链表中移除
    auto it = node_map_.find(frame_id);
    if (it != node_map_.end()) {
        remove_node(it->second.get());
        node_map_.erase(it);
    }
}

bool LRUReplacer::unpin(size_t frame_id) {
    if (frame_id >= num_pages_) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    pinned_[frame_id] = false;
    
    // 添加到链表头部
    auto it = node_map_.find(frame_id);
    if (it == node_map_.end()) {
        auto node = std::make_unique<Node>(frame_id);
        Node* node_ptr = node.get();
        node_map_[frame_id] = std::move(node);
        add_to_head(node_ptr);
    }
    
    return true;
}

std::optional<size_t> LRUReplacer::select_victim() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 从尾部选择（最少使用的）
    if (tail_ == nullptr) {
        return std::nullopt;
    }
    
    size_t victim_id = tail_->frame_id;
    remove_node(tail_);
    node_map_.erase(victim_id);
    
    return victim_id;
}

void LRUReplacer::remove(size_t frame_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = node_map_.find(frame_id);
    if (it != node_map_.end()) {
        remove_node(it->second.get());
        node_map_.erase(it);
    }
}

size_t LRUReplacer::get_replacable_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return node_map_.size();
}

size_t LRUReplacer::get_total_count() const {
    return num_pages_;
}

void LRUReplacer::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    head_ = nullptr;
    tail_ = nullptr;
    node_map_.clear();
    std::fill(pinned_.begin(), pinned_.end(), false);
}

void LRUReplacer::add_to_head(Node* node) {
    node->prev = nullptr;
    node->next = head_;
    
    if (head_ != nullptr) {
        head_->prev = node;
    } else {
        tail_ = node;
    }
    
    head_ = node;
}

void LRUReplacer::remove_node(Node* node) {
    if (node->prev != nullptr) {
        node->prev->next = node->next;
    } else {
        head_ = node->next;
    }
    
    if (node->next != nullptr) {
        node->next->prev = node->prev;
    } else {
        tail_ = node->prev;
    }
}

void LRUReplacer::move_to_head(Node* node) {
    if (node == head_) {
        return;
    }
    
    remove_node(node);
    add_to_head(node);
}

// ==================== ClockReplacer 实现 ====================

ClockReplacer::ClockReplacer(size_t num_pages)
    : num_pages_(num_pages),
      clock_hand_(0),
      ref_bits_(num_pages),
      pinned_(num_pages),
      in_replacer_(num_pages) {
    for (size_t i = 0; i < num_pages; ++i) {
        ref_bits_[i].store(false);
        pinned_[i].store(false);
        in_replacer_[i].store(false);
    }
}

void ClockReplacer::record_access(size_t frame_id) {
    if (frame_id >= num_pages_) {
        return;
    }
    
    ref_bits_[frame_id].store(true);
}

void ClockReplacer::pin(size_t frame_id) {
    if (frame_id >= num_pages_) {
        return;
    }
    
    pinned_[frame_id].store(true);
    in_replacer_[frame_id].store(false);
}

bool ClockReplacer::unpin(size_t frame_id) {
    if (frame_id >= num_pages_) {
        return false;
    }
    
    pinned_[frame_id].store(false);
    in_replacer_[frame_id].store(true);
    return true;
}

std::optional<size_t> ClockReplacer::select_victim() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 扫描两轮（最多）
    for (size_t round = 0; round < 2; ++round) {
        for (size_t i = 0; i < num_pages_; ++i) {
            size_t frame_id = clock_hand_;
            clock_hand_ = (clock_hand_ + 1) % num_pages_;
            
            if (!in_replacer_[frame_id].load()) {
                continue;
            }
            
            if (pinned_[frame_id].load()) {
                continue;
            }
            
            if (ref_bits_[frame_id].load()) {
                // 清除引用位，给第二次机会
                ref_bits_[frame_id].store(false);
            } else {
                // 找到牺牲页
                in_replacer_[frame_id].store(false);
                return frame_id;
            }
        }
    }
    
    return std::nullopt;
}

void ClockReplacer::remove(size_t frame_id) {
    if (frame_id >= num_pages_) {
        return;
    }
    
    in_replacer_[frame_id].store(false);
    ref_bits_[frame_id].store(false);
}

size_t ClockReplacer::get_replacable_count() const {
    size_t count = 0;
    for (size_t i = 0; i < num_pages_; ++i) {
        if (in_replacer_[i].load() && !pinned_[i].load()) {
            count++;
        }
    }
    return count;
}

size_t ClockReplacer::get_total_count() const {
    return num_pages_;
}

void ClockReplacer::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    clock_hand_ = 0;
    
    for (size_t i = 0; i < num_pages_; ++i) {
        ref_bits_[i].store(false);
        pinned_[i].store(false);
        in_replacer_[i].store(false);
    }
}

// ==================== BufferPoolConfig 实现 ====================

bool BufferPoolConfig::validate() const {
    if (pool_size == 0) {
        return false;
    }
    if (page_size == 0) {
        return false;
    }
    if (flush_interval.count() <= 0) {
        return false;
    }
    return true;
}

BufferPoolConfig BufferPoolConfig::default_config() {
    BufferPoolConfig config;
    config.pool_size = 1024;
    config.page_size = kPageSize;
    config.replacer_type = ReplacerType::LRU;
    config.flush_strategy = FlushStrategy::WRITE_BACK;
    config.flush_interval = std::chrono::milliseconds(1000);
    config.dirty_page_threshold = 100;
    config.enable_prefetch = true;
    config.prefetch_size = 4;
    config.enable_statistics = true;
    config.stats_interval = std::chrono::seconds(60);
    return config;
}

// ==================== BufferFrame 实现 ====================

BufferFrame::BufferFrame(size_t page_size)
    : page_() {
    (void)page_size; // Page是固定4KB，构造函数不需要参数
}

void BufferFrame::pin() {
    pin_count_.fetch_add(1, std::memory_order_relaxed);
}

void BufferFrame::unpin() {
    uint32_t old_count = pin_count_.fetch_sub(1, std::memory_order_relaxed);
    if (old_count == 0) {
        // 已经是0，不应该再减少
        pin_count_.store(0);
    }
}

void BufferFrame::mark_accessed() {
    access_time_.store(bufferpool_now_ns(), std::memory_order_relaxed);
    access_count_.fetch_add(1, std::memory_order_relaxed);
}

bool BufferFrame::try_lock() {
    bool expected = false;
    return locked_.compare_exchange_strong(expected, true, std::memory_order_acquire);
}

void BufferFrame::unlock() {
    locked_.store(false, std::memory_order_release);
}

// ==================== BufferPool 实现 ====================

std::unique_ptr<BufferPool> BufferPool::create(FileManager* file_manager,
                                                const BufferPoolConfig& config) {
    if (!file_manager) {
        return nullptr;
    }
    
    if (!config.validate()) {
        return nullptr;
    }
    
    auto pool = std::unique_ptr<BufferPool>(new BufferPool(file_manager, config));
    
    if (!pool->init()) {
        return nullptr;
    }
    
    return pool;
}

BufferPool::BufferPool(FileManager* file_manager, const BufferPoolConfig& config)
    : config_(config), file_manager_(file_manager) {
}

BufferPool::~BufferPool() {
    cleanup();
}

bool BufferPool::init() {
    // 创建替换器
    switch (config_.replacer_type) {
        case BufferPoolConfig::ReplacerType::LRU:
            replacer_ = std::make_unique<LRUReplacer>(config_.pool_size);
            break;
        case BufferPoolConfig::ReplacerType::CLOCK:
            replacer_ = std::make_unique<ClockReplacer>(config_.pool_size);
            break;
        case BufferPoolConfig::ReplacerType::ADAPTIVE:
            // 默认使用LRU
            replacer_ = std::make_unique<LRUReplacer>(config_.pool_size);
            break;
    }
    
    // 分配帧
    frames_.reserve(config_.pool_size);
    for (size_t i = 0; i < config_.pool_size; ++i) {
        frames_.push_back(std::make_unique<BufferFrame>(config_.page_size));
        free_list_.push_back(i);
    }
    
    // 初始化统计信息
    stats_.total_frames = config_.pool_size;
    stats_.free_frames = config_.pool_size;
    stats_.replacer_name = replacer_->get_name();
    
    // 初始化数据文件（单文件模式）
    // 简单策略：使用固定文件名，由上层保证 data 目录存在
    if (file_manager_) {
        // 这里不引入完整的 FileManager 高层接口，而是直接通过 FileCacheManager 风格：
        // 为了保持依赖最小，BufferPool 只拿一个 FileHandle。
        try {
            std::string path = "data/mementodb.dat";
            data_file_path_ = path;
            FileManagerConfig fm_cfg; // 使用默认配置
            FileCacheManager cache(1024 * 1024 * 1024, 1024); // 简单参数：1GB 缓存，最多 1024 文件
            data_file_ = cache.get_file(path, FileHandle::OpenMode::ReadWrite, fm_cfg);
        } catch (...) {
            data_file_.reset();
        }
    }

    // 启动后台线程
    if (config_.flush_strategy == BufferPoolConfig::FlushStrategy::PERIODIC ||
        config_.flush_strategy == BufferPoolConfig::FlushStrategy::WRITE_BACK) {
        start_background_threads();
    }
    
    return true;
}

void BufferPool::cleanup() {
    stop_background_threads();
    
    // 刷所有脏页
    flush_all();
    
    // 清理资源
    std::unique_lock page_table_lock(page_table_mutex_);
    page_table_.clear();
}

BufferFrame* BufferPool::fetch_page(uint64_t page_id, bool exclusive) {
    if (page_id == 0) {
        return nullptr;
    }
    
    // 查找页面是否已在缓冲池中
    {
        std::shared_lock lock(page_table_mutex_);
        auto it = page_table_.find(page_id);
        if (it != page_table_.end()) {
            size_t frame_id = it->second;
            BufferFrame* frame = get_frame(frame_id);
            if (frame) {
                frame->mark_accessed();
                frame->pin();
                replacer_->record_access(frame_id);
                hit_count_.fetch_add(1, std::memory_order_relaxed);
                return frame;
            }
        }
    }
    
    // 缓存未命中
    miss_count_.fetch_add(1, std::memory_order_relaxed);
    
    // 加载页面
    return load_page(page_id, exclusive);
}

BufferFrame* BufferPool::new_page() {
    // 简单的 FileManager-based PageAllocator：
    // 通过数据文件当前大小推算下一个 page_id，并在需要时扩展文件。
    if (!data_file_) {
        return nullptr;
    }

    uint64_t file_size = data_file_->size();
    uint64_t page_id = file_size / config_.page_size;

    // 确保文件至少扩展一个 page 的空间
    try {
        uint64_t new_size = (page_id + 1) * config_.page_size;
        if (new_size > file_size) {
            data_file_->truncate(new_size);
        }
    } catch (...) {
        return nullptr;
    }

    return load_page(page_id, false);
}

void BufferPool::unpin_page(uint64_t page_id, bool mark_dirty) {
    std::shared_lock lock(page_table_mutex_);
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return;
    }
    
    size_t frame_id = it->second;
    BufferFrame* frame = get_frame(frame_id);
    if (!frame) {
        return;
    }
    
    if (mark_dirty) {
        frame->mark_dirty();
    }
    
    frame->unpin();
    replacer_->unpin(frame_id);
}

bool BufferPool::delete_page(uint64_t page_id) {
    std::unique_lock lock(page_table_mutex_);
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }
    
    size_t frame_id = it->second;
    BufferFrame* frame = get_frame(frame_id);
    if (frame && frame->is_pinned()) {
        return false; // 页面被固定，不能删除
    }
    
    // 从页表中移除
    page_table_.erase(it);
    
    // 从替换器中移除
    replacer_->remove(frame_id);
    
    // 释放帧
    deallocate_frame(frame_id);
    
    return true;
}

void BufferPool::prefetch_pages(const std::vector<uint64_t>& page_ids) {
    if (!config_.enable_prefetch) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(prefetch_queue_mutex_);
    for (uint64_t page_id : page_ids) {
        prefetch_queue_.push(page_id);
    }
    prefetch_count_.fetch_add(page_ids.size(), std::memory_order_relaxed);
}

void BufferPool::flush_all() {
    std::shared_lock lock(page_table_mutex_);
    
    for (const auto& pair : page_table_) {
        flush_page(pair.first);
    }
}

bool BufferPool::flush_page(uint64_t page_id) {
    std::shared_lock lock(page_table_mutex_);
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }
    
    size_t frame_id = it->second;
    return flush_frame(frame_id);
}

BufferPool::Statistics BufferPool::get_statistics() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    Statistics result = stats_;
    
    // 更新实时统计
    uint64_t hits = hit_count_.load();
    uint64_t misses = miss_count_.load();
    uint64_t total = hits + misses;
    
    result.hit_count = hits;
    result.miss_count = misses;
    result.hit_ratio = total > 0 ? static_cast<double>(hits) / total : 0.0;
    result.evict_count = evict_count_.load();
    result.flush_count = flush_count_.load();
    result.prefetch_count = prefetch_count_.load();
    
    // 统计帧状态
    result.used_frames = page_table_.size();
    result.free_frames = stats_.total_frames - result.used_frames;
    
    size_t dirty_count = 0;
    size_t pinned_count = 0;
    for (size_t i = 0; i < frames_.size(); ++i) {
        BufferFrame* frame = get_frame(i);
        if (frame) {
            if (frame->is_dirty()) {
                dirty_count++;
            }
            if (frame->is_pinned()) {
                pinned_count++;
            }
        }
    }
    
    result.dirty_frames = dirty_count;
    result.pinned_frames = pinned_count;
    
    return result;
}

void BufferPool::reset_statistics() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    hit_count_.store(0);
    miss_count_.store(0);
    evict_count_.store(0);
    flush_count_.store(0);
    prefetch_count_.store(0);
    stats_ = Statistics();
    stats_.total_frames = config_.pool_size;
    stats_.replacer_name = replacer_->get_name();
}

BufferFrame* BufferPool::get_frame(size_t frame_id) {
    if (frame_id >= frames_.size()) {
        return nullptr;
    }
    return frames_[frame_id].get();
}

BufferFrame* BufferPool::get_frame(size_t frame_id) const {
    if (frame_id >= frames_.size()) {
        return nullptr;
    }
    return frames_[frame_id].get();
}

size_t BufferPool::allocate_frame() {
    std::lock_guard<std::mutex> lock(free_list_mutex_);
    
    if (!free_list_.empty()) {
        size_t frame_id = free_list_.back();
        free_list_.pop_back();
        return frame_id;
    }
    
    // 没有空闲帧，需要驱逐一个
    auto victim = replacer_->select_victim();
    if (victim.has_value()) {
        size_t frame_id = victim.value();
        evict_frame(frame_id);
        return frame_id;
    }
    
    return SIZE_MAX; // 没有可用的帧
}

void BufferPool::deallocate_frame(size_t frame_id) {
    BufferFrame* frame = get_frame(frame_id);
    if (!frame) {
        return;
    }
    
    // 重置帧状态
    frame->set_page_id(0);
    frame->clear_dirty();
    frame->unpin();
    
    // 添加到空闲列表
    std::lock_guard<std::mutex> lock(free_list_mutex_);
    free_list_.push_back(frame_id);
}

BufferFrame* BufferPool::load_page(uint64_t page_id, bool exclusive) {
    // 分配帧
    size_t frame_id = allocate_frame();
    if (frame_id == SIZE_MAX) {
        return nullptr; // 无法分配帧
    }
    
    BufferFrame* frame = get_frame(frame_id);
    if (!frame) {
        return nullptr;
    }
    
    // 如果帧中有旧页面，先驱逐
    if (frame->is_valid()) {
        evict_frame(frame_id);
    }
    
    // 从文件加载页面（FileManager 单文件模式）
    if (data_file_) {
        uint64_t offset = page_id * config_.page_size;
        try {
            size_t bytes_read = data_file_->pread(frame->page().GetData(),
                                                  config_.page_size,
                                                  offset);
            // 对于新分配但尚未写入的页，可能读取到零或不足 page_size 的数据；
            // 这里简单视为成功，让上层初始化内容。
            (void)bytes_read;
        } catch (...) {
            return nullptr;
        }
    }
    
    // 设置页面ID
    frame->set_page_id(page_id);
    frame->pin();
    frame->mark_accessed();
    
    // 更新页表
    {
        std::unique_lock lock(page_table_mutex_);
        page_table_[page_id] = frame_id;
    }
    
    // 通知替换器
    replacer_->pin(frame_id);
    
    return frame;
}

bool BufferPool::evict_frame(size_t frame_id) {
    BufferFrame* frame = get_frame(frame_id);
    if (!frame || !frame->is_valid()) {
        return false;
    }
    
    uint64_t page_id = frame->page_id();
    
    // 如果是脏页，先刷盘
    if (frame->is_dirty()) {
        if (!flush_frame(frame_id)) {
            return false; // 刷盘失败，不能驱逐
        }
    }
    
    // 从页表中移除
    {
        std::unique_lock lock(page_table_mutex_);
        page_table_.erase(page_id);
    }
    
    // 从替换器中移除
    replacer_->remove(frame_id);
    
    // 重置帧
    frame->set_page_id(0);
    frame->clear_dirty();
    
    evict_count_.fetch_add(1, std::memory_order_relaxed);
    
    return true;
}

bool BufferPool::flush_frame(size_t frame_id) {
    BufferFrame* frame = get_frame(frame_id);
    if (!frame || !frame->is_valid() || !frame->is_dirty()) {
        return true; // 不是脏页，不需要刷
    }
    
    uint64_t page_id = frame->page_id();
    
    if (!data_file_) {
        return false;
    }

    // 计算页面在文件中的偏移量
    uint64_t offset = page_id * config_.page_size;

    // 写回策略：WRITE_THROUGH 立即写盘并 sync；WRITE_BACK 只在这里写一次
    size_t bytes_written =
        data_file_->pwrite(frame->page().GetData(), config_.page_size, offset);
    if (bytes_written != config_.page_size) {
        return false;
    }

    if (config_.flush_strategy == BufferPoolConfig::FlushStrategy::WRITE_THROUGH) {
        data_file_->sync();
    }

    frame->clear_dirty();
    flush_count_.fetch_add(1, std::memory_order_relaxed);

    return true;
}

void BufferPool::start_background_threads() {
    if (running_.exchange(true)) {
        return; // 已经在运行
    }
    
    if (config_.flush_strategy == BufferPoolConfig::FlushStrategy::PERIODIC ||
        config_.flush_strategy == BufferPoolConfig::FlushStrategy::WRITE_BACK) {
        flush_thread_ = std::thread(&BufferPool::flush_thread_func, this);
    }
    
    if (config_.enable_prefetch) {
        prefetch_thread_ = std::thread(&BufferPool::prefetch_thread_func, this);
    }
    
    if (config_.enable_statistics) {
        stats_thread_ = std::thread(&BufferPool::stats_thread_func, this);
    }
}

void BufferPool::stop_background_threads() {
    if (!running_.exchange(false)) {
        return; // 没有运行
    }
    
    if (flush_thread_.joinable()) {
        flush_thread_.join();
    }
    
    if (prefetch_thread_.joinable()) {
        prefetch_thread_.join();
    }
    
    if (stats_thread_.joinable()) {
        stats_thread_.join();
    }
}

void BufferPool::flush_thread_func() {
    while (running_.load()) {
        std::this_thread::sleep_for(config_.flush_interval);
        
        if (!running_.load()) {
            break;
        }
        
        // 刷脏页
        std::shared_lock lock(page_table_mutex_);
        size_t dirty_count = 0;
        
        for (const auto& pair : page_table_) {
            size_t frame_id = pair.second;
            BufferFrame* frame = get_frame(frame_id);
            if (frame && frame->is_dirty()) {
                dirty_count++;
                if (dirty_count >= config_.dirty_page_threshold) {
                    flush_frame(frame_id);
                }
            }
        }
    }
}

void BufferPool::prefetch_thread_func() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        if (!running_.load()) {
            break;
        }
        
        std::vector<uint64_t> page_ids;
        {
            std::lock_guard<std::mutex> lock(prefetch_queue_mutex_);
            size_t count = std::min(config_.prefetch_size, prefetch_queue_.size());
            for (size_t i = 0; i < count; ++i) {
                page_ids.push_back(prefetch_queue_.front());
                prefetch_queue_.pop();
            }
        }
        
        for (uint64_t page_id : page_ids) {
            fetch_page(page_id, false);
        }
    }
}

void BufferPool::stats_thread_func() {
    while (running_.load()) {
        std::this_thread::sleep_for(config_.stats_interval);
        
        if (!running_.load()) {
            break;
        }
        
        update_statistics();
    }
}

void BufferPool::update_statistics() {
    // 更新统计信息（可以在这里记录日志或发送到Metrics系统）
    auto stats = get_statistics();
    (void)stats; // 避免未使用警告
}

uint64_t BufferPool::get_current_time_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace core
} // namespace mementodb

