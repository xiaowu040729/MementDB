// File: src/core/BufferPool_Enhanced.cpp
// 增强版BufferPool实现

#include "BufferPool.h"
#include "FileManager.hpp"
#include <algorithm>
#include <thread>
#include <chrono>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <numeric>
#include <cstdlib>

// ZSTD 压缩库
#ifdef HAVE_ZSTD
#include <zstd.h>
#endif

namespace mementodb {
namespace core {

// ==================== BufferPoolPerformanceCounters 实现 ====================

void BufferPoolPerformanceCounters::LatencyStats::record(uint64_t latency_ns) {
    count++;
    total_ns += latency_ns;
    if (latency_ns < min_ns) {
        min_ns = latency_ns;
    }
    if (latency_ns > max_ns) {
        max_ns = latency_ns;
    }
}

double BufferPoolPerformanceCounters::LatencyStats::average_ns() const {
    return count > 0 ? static_cast<double>(total_ns) / count : 0.0;
}

void BufferPoolPerformanceCounters::LatencyStats::reset() {
    count = 0;
    total_ns = 0;
    min_ns = UINT64_MAX;
    max_ns = 0;
}

double BufferPoolPerformanceCounters::hit_ratio() const {
    uint64_t total = fetch_page_hits + fetch_page_misses;
    return total > 0 ? static_cast<double>(fetch_page_hits) / total : 0.0;
}

double BufferPoolPerformanceCounters::avg_read_latency_ns() const {
    return io_read_time_ns > 0 && fetch_page_calls > 0 
        ? static_cast<double>(io_read_time_ns) / fetch_page_calls 
        : 0.0;
}

double BufferPoolPerformanceCounters::avg_write_latency_ns() const {
    return io_write_time_ns > 0 && flush_page_calls > 0 
        ? static_cast<double>(io_write_time_ns) / flush_page_calls 
        : 0.0;
}

void BufferPoolPerformanceCounters::reset() {
    fetch_page_calls = 0;
    fetch_page_hits = 0;
    fetch_page_misses = 0;
    new_page_calls = 0;
    unpin_page_calls = 0;
    flush_page_calls = 0;
    evict_page_calls = 0;
    prefetch_calls = 0;
    prefetch_hits = 0;
    lock_contention_count = 0;
    lock_wait_time_ns = 0;
    io_read_bytes = 0;
    io_write_bytes = 0;
    io_read_time_ns = 0;
    io_write_time_ns = 0;
    
    fetch_latency.reset();
    flush_latency.reset();
    evict_latency.reset();
}

std::string BufferPoolPerformanceCounters::to_string() const {
    std::ostringstream oss;
    oss << "BufferPool Performance Counters:\n";
    oss << "  Fetch Page: calls=" << fetch_page_calls 
        << ", hits=" << fetch_page_hits 
        << ", misses=" << fetch_page_misses
        << ", hit_ratio=" << std::fixed << std::setprecision(2) << (hit_ratio() * 100) << "%\n";
    oss << "  New Page: calls=" << new_page_calls << "\n";
    oss << "  Unpin Page: calls=" << unpin_page_calls << "\n";
    oss << "  Flush Page: calls=" << flush_page_calls << "\n";
    oss << "  Evict Page: calls=" << evict_page_calls << "\n";
    oss << "  Prefetch: calls=" << prefetch_calls << ", hits=" << prefetch_hits << "\n";
    oss << "  Lock Contention: count=" << lock_contention_count 
        << ", wait_time_ns=" << lock_wait_time_ns << "\n";
    oss << "  IO Read: bytes=" << io_read_bytes 
        << ", time_ns=" << io_read_time_ns 
        << ", avg_latency_ns=" << avg_read_latency_ns() << "\n";
    oss << "  IO Write: bytes=" << io_write_bytes 
        << ", time_ns=" << io_write_time_ns 
        << ", avg_latency_ns=" << avg_write_latency_ns() << "\n";
    oss << "  Fetch Latency: avg=" << fetch_latency.average_ns() 
        << "ns, min=" << fetch_latency.min_ns 
        << "ns, max=" << fetch_latency.max_ns << "ns\n";
    oss << "  Flush Latency: avg=" << flush_latency.average_ns() 
        << "ns, min=" << flush_latency.min_ns 
        << "ns, max=" << flush_latency.max_ns << "ns\n";
    oss << "  Evict Latency: avg=" << evict_latency.average_ns() 
        << "ns, min=" << evict_latency.min_ns 
        << "ns, max=" << evict_latency.max_ns << "ns\n";
    return oss.str();
}

std::string BufferPoolPerformanceCounters::to_json() const {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"fetch_page_calls\": " << fetch_page_calls << ",\n";
    oss << "  \"fetch_page_hits\": " << fetch_page_hits << ",\n";
    oss << "  \"fetch_page_misses\": " << fetch_page_misses << ",\n";
    oss << "  \"hit_ratio\": " << std::fixed << std::setprecision(4) << hit_ratio() << ",\n";
    oss << "  \"new_page_calls\": " << new_page_calls << ",\n";
    oss << "  \"unpin_page_calls\": " << unpin_page_calls << ",\n";
    oss << "  \"flush_page_calls\": " << flush_page_calls << ",\n";
    oss << "  \"evict_page_calls\": " << evict_page_calls << ",\n";
    oss << "  \"prefetch_calls\": " << prefetch_calls << ",\n";
    oss << "  \"prefetch_hits\": " << prefetch_hits << ",\n";
    oss << "  \"io_read_bytes\": " << io_read_bytes << ",\n";
    oss << "  \"io_write_bytes\": " << io_write_bytes << ",\n";
    oss << "  \"avg_read_latency_ns\": " << avg_read_latency_ns() << ",\n";
    oss << "  \"avg_write_latency_ns\": " << avg_write_latency_ns() << "\n";
    oss << "}";
    return oss.str();
}

// ==================== ReplacerConfig 实现 ====================

ReplacerConfig ReplacerConfig::default_config() {
    ReplacerConfig config;
    config.type = Type::LRU;
    config.lru_config.enable_scan_resistance = true;
    config.lru_config.hot_threshold = 10;
    config.clock_config.second_chance = 2;
    config.clock_config.enable_adaptive_hand = true;
    config.arc_config.adaptive_p = 0;
    config.arc_config.enable_dynamic_p = true;
    config.lirs_config.lir_size_ratio = 20;
    config.lirs_config.hir_size_ratio = 80;
    config.two_queue_config.kin_ratio = 25;
    config.two_queue_config.kout_ratio = 50;
    config.enable_access_frequency = true;
    config.enable_access_recency = true;
    config.monitor_window_size = 1000;
    config.adapt_interval = std::chrono::seconds(60);
    return config;
}

std::string ReplacerConfig::type_to_string() const {
    switch (type) {
        case Type::LRU: return "LRU";
        case Type::CLOCK: return "Clock";
        case Type::ARC: return "ARC";
        case Type::LIRS: return "LIRS";
        case Type::TWO_QUEUE: return "2Q";
        case Type::MQ: return "MQ";
        case Type::ADAPTIVE: return "Adaptive";
        case Type::HYBRID: return "Hybrid";
        default: return "Unknown";
    }
}

// ==================== ARCReplacer 实现 ====================

ARCReplacer::ARCReplacer(size_t num_pages, const ReplacerConfig::ARCConfig& config)
    : capacity_(num_pages), config_(config), pinned_(num_pages) {
    for (size_t i = 0; i < num_pages; ++i) {
        pinned_[i].store(false);
    }
    p_ = config.adaptive_p;
}

void ARCReplacer::record_access(size_t frame_id) {
    if (frame_id >= capacity_) {
        return;
    }
    
    std::unique_lock lock(mutex_);
    
    // 检查是否在T1或T2中
    if (is_in_t1(frame_id)) {
        // 从T1移动到T2
        move_to_t2(frame_id);
        update_stats(true);
    } else if (is_in_t2(frame_id)) {
        // 已经在T2中，移动到头部
        auto it = t2_map_.find(frame_id);
        if (it != t2_map_.end()) {
            t2_.erase(it->second);
            t2_.push_front(frame_id);
            t2_map_[frame_id] = t2_.begin();
        }
        update_stats(true);
    } else if (is_in_b1(frame_id)) {
        // 在B1中，说明之前被替换了，需要调整p
        if (config_.enable_dynamic_p) {
            size_t delta1 = b1_.size() >= b2_.size() ? 1 : b2_.size() / b1_.size();
            p_ = std::min(p_ + delta1, capacity_);
        }
        replace(true);
        // 从B1移除，添加到T2
        auto it = b1_map_.find(frame_id);
        if (it != b1_map_.end()) {
            b1_.erase(it->second);
            b1_map_.erase(it);
        }
        t2_.push_front(frame_id);
        t2_map_[frame_id] = t2_.begin();
        update_stats(false);
    } else if (is_in_b2(frame_id)) {
        // 在B2中
        if (config_.enable_dynamic_p) {
            size_t delta2 = b2_.size() >= b1_.size() ? 1 : b1_.size() / b2_.size();
            p_ = p_ > delta2 ? p_ - delta2 : 0;
        }
        replace(false);
        // 从B2移除，添加到T2
        auto it = b2_map_.find(frame_id);
        if (it != b2_map_.end()) {
            b2_.erase(it->second);
            b2_map_.erase(it);
        }
        t2_.push_front(frame_id);
        t2_map_[frame_id] = t2_.begin();
        update_stats(false);
    } else {
        // 新页面，添加到T1
        if (t1_.size() + (is_in_b1(frame_id) ? 0 : 1) >= capacity_) {
            if (t1_.size() < capacity_) {
                // 从B1移除一个
                if (!b1_.empty()) {
                    size_t victim = b1_.back();
                    b1_.pop_back();
                    b1_map_.erase(victim);
                }
                replace(true);
            } else {
                // 从T1移除一个到B1
                size_t victim = t1_.back();
                t1_.pop_back();
                t1_map_.erase(victim);
                b1_.push_front(victim);
                b1_map_[victim] = b1_.begin();
            }
        }
        t1_.push_front(frame_id);
        t1_map_[frame_id] = t1_.begin();
        update_stats(false);
    }
}

void ARCReplacer::record_access_batch(const std::vector<size_t>& frame_ids) {
    for (size_t frame_id : frame_ids) {
        record_access(frame_id);
    }
}

void ARCReplacer::pin(size_t frame_id) {
    if (frame_id >= capacity_) {
        return;
    }
    
    std::unique_lock lock(mutex_);
    pinned_[frame_id].store(true);
    
    // 从所有列表中移除
    if (is_in_t1(frame_id)) {
        auto it = t1_map_.find(frame_id);
        if (it != t1_map_.end()) {
            t1_.erase(it->second);
            t1_map_.erase(it);
        }
    }
    if (is_in_t2(frame_id)) {
        auto it = t2_map_.find(frame_id);
        if (it != t2_map_.end()) {
            t2_.erase(it->second);
            t2_map_.erase(it);
        }
    }
}

void ARCReplacer::pin_batch(const std::vector<size_t>& frame_ids) {
    for (size_t frame_id : frame_ids) {
        pin(frame_id);
    }
}

bool ARCReplacer::unpin(size_t frame_id) {
    if (frame_id >= capacity_) {
        return false;
    }
    
    std::unique_lock lock(mutex_);
    pinned_[frame_id].store(false);
    return true;
}

void ARCReplacer::unpin_batch(const std::vector<size_t>& frame_ids) {
    for (size_t frame_id : frame_ids) {
        unpin(frame_id);
    }
}

std::optional<size_t> ARCReplacer::select_victim() {
    std::unique_lock lock(mutex_);
    
    // 从T1或T2中选择受害者
    if (!t1_.empty() && (t1_.size() > p_ || (t2_.empty() && t1_.size() == p_))) {
        size_t victim = choose_victim_from_t1();
        if (victim != SIZE_MAX) {
            return victim;
        }
    }
    
    if (!t2_.empty()) {
        size_t victim = choose_victim_from_t2();
        if (victim != SIZE_MAX) {
            return victim;
        }
    }
    
    return std::nullopt;
}

std::vector<size_t> ARCReplacer::select_victims(size_t count) {
    std::vector<size_t> victims;
    for (size_t i = 0; i < count; ++i) {
        auto victim = select_victim();
        if (victim.has_value()) {
            victims.push_back(victim.value());
            remove(victim.value());
        } else {
            break;
        }
    }
    return victims;
}

void ARCReplacer::remove(size_t frame_id) {
    std::unique_lock lock(mutex_);
    
    if (is_in_t1(frame_id)) {
        auto it = t1_map_.find(frame_id);
        if (it != t1_map_.end()) {
            t1_.erase(it->second);
            t1_map_.erase(it);
        }
    }
    if (is_in_t2(frame_id)) {
        auto it = t2_map_.find(frame_id);
        if (it != t2_map_.end()) {
            t2_.erase(it->second);
            t2_map_.erase(it);
        }
    }
}

size_t ARCReplacer::get_replacable_count() const {
    std::shared_lock lock(mutex_);
    size_t count = 0;
    for (size_t i = 0; i < capacity_; ++i) {
        if (!pinned_[i].load() && (is_in_t1(i) || is_in_t2(i))) {
            count++;
        }
    }
    return count;
}

size_t ARCReplacer::get_total_count() const {
    return capacity_;
}

void ARCReplacer::reset() {
    std::unique_lock lock(mutex_);
    t1_.clear();
    t2_.clear();
    b1_.clear();
    b2_.clear();
    t1_map_.clear();
    t2_map_.clear();
    b1_map_.clear();
    b2_map_.clear();
    p_ = 0;
    for (size_t i = 0; i < capacity_; ++i) {
        pinned_[i].store(false);
    }
    
    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
    stats_ = ReplacerStats();
}


double ARCReplacer::get_heat_score(size_t frame_id) const {
    std::shared_lock lock(mutex_);
    
    if (is_in_t2(frame_id)) {
        // T2中的页面更热
        return 1.0;
    } else if (is_in_t1(frame_id)) {
        return 0.5;
    } else {
        return 0.0;
    }
}

double ARCReplacer::get_cold_score(size_t frame_id) const {
    return 1.0 - get_heat_score(frame_id);
}

void ARCReplacer::tune(const std::string& param, double value) {
    std::unique_lock lock(mutex_);
    
    if (param == "p") {
        p_ = static_cast<size_t>(value);
    }
}

IAdvancedReplacer::ReplacerStats ARCReplacer::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

std::string ARCReplacer::export_state() const {
    std::shared_lock lock(mutex_);
    std::ostringstream oss;
    oss << "ARC:" << p_ << ":" << t1_.size() << ":" << t2_.size() << ":" 
        << b1_.size() << ":" << b2_.size();
    return oss.str();
}

bool ARCReplacer::import_state(const std::string& state) {
    // 简化实现
    return true;
}

bool ARCReplacer::is_in_t1(size_t frame_id) const {
    return t1_map_.find(frame_id) != t1_map_.end();
}

bool ARCReplacer::is_in_t2(size_t frame_id) const {
    return t2_map_.find(frame_id) != t2_map_.end();
}

bool ARCReplacer::is_in_b1(size_t frame_id) const {
    return b1_map_.find(frame_id) != b1_map_.end();
}

bool ARCReplacer::is_in_b2(size_t frame_id) const {
    return b2_map_.find(frame_id) != b2_map_.end();
}

void ARCReplacer::move_to_t2(size_t frame_id) {
    auto it = t1_map_.find(frame_id);
    if (it != t1_map_.end()) {
        t1_.erase(it->second);
        t1_map_.erase(it);
    }
    t2_.push_front(frame_id);
    t2_map_[frame_id] = t2_.begin();
}

void ARCReplacer::replace(bool in_b2) {
    // 根据p值决定从哪个列表替换
    if (t1_.size() > 0 && (t1_.size() > p_ || (in_b2 && t1_.size() == p_))) {
        if (!t1_.empty()) {
            size_t victim = t1_.back();
            t1_.pop_back();
            t1_map_.erase(victim);
            b1_.push_front(victim);
            b1_map_[victim] = b1_.begin();
        }
    } else {
        if (!t2_.empty()) {
            size_t victim = t2_.back();
            t2_.pop_back();
            t2_map_.erase(victim);
            b2_.push_front(victim);
            b2_map_[victim] = b2_.begin();
        }
    }
}

size_t ARCReplacer::choose_victim_from_t1() const {
    // 从T1尾部选择（最近最少使用）
    for (auto it = t1_.rbegin(); it != t1_.rend(); ++it) {
        if (!pinned_[*it].load()) {
            return *it;
        }
    }
    return SIZE_MAX;
}

size_t ARCReplacer::choose_victim_from_t2() const {
    // 从T2尾部选择
    for (auto it = t2_.rbegin(); it != t2_.rend(); ++it) {
        if (!pinned_[*it].load()) {
            return *it;
        }
    }
    return SIZE_MAX;
}

void ARCReplacer::update_stats(bool hit) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.total_accesses++;
    if (hit) {
        stats_.hits++;
    } else {
        stats_.compulsory_misses++;
    }
    if (stats_.total_accesses > 0) {
        stats_.hit_ratio = static_cast<double>(stats_.hits) / stats_.total_accesses;
    }
}

// ==================== AdaptiveReplacer::AccessPatternAnalyzer 实现 ====================

AdaptiveReplacer::AccessPatternAnalyzer::AccessPatternAnalyzer(size_t window_size)
    : window_size_(window_size) {
}

void AdaptiveReplacer::AccessPatternAnalyzer::record_access(size_t frame_id, uint64_t timestamp_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    access_history_.push_back({frame_id, timestamp_ns});
    if (access_history_.size() > window_size_) {
        access_history_.pop_front();
    }
}

AdaptiveReplacer::AccessPatternAnalyzer::Pattern AdaptiveReplacer::AccessPatternAnalyzer::analyze() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (access_history_.size() < 10) {
        return Pattern::UNKNOWN;
    }
    
    double locality = calculate_locality_score();
    double sequential = calculate_sequential_score();
    double loop = calculate_loop_score();
    
    if (sequential > 0.7) {
        return Pattern::SEQUENTIAL;
    } else if (loop > 0.6) {
        return Pattern::LOOPING;
    } else if (locality < 0.3) {
        return Pattern::RANDOM;
    } else if (locality > 0.7) {
        return Pattern::HOT_COLD;
    } else {
        return Pattern::SCAN;
    }
}

AdaptiveReplacer::AccessPatternAnalyzer::AnalysisResult AdaptiveReplacer::AccessPatternAnalyzer::get_analysis() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    AnalysisResult result;
    result.pattern = analyze();
    result.confidence = 0.8; // 简化实现
    
    result.metrics["locality"] = calculate_locality_score();
    result.metrics["sequential"] = calculate_sequential_score();
    result.metrics["loop"] = calculate_loop_score();
    result.metrics["hot_cold_ratio"] = calculate_hot_cold_ratio();
    
    switch (result.pattern) {
        case Pattern::SEQUENTIAL:
            result.suggested_replacer = "LRU";
            break;
        case Pattern::LOOPING:
            result.suggested_replacer = "Clock";
            break;
        case Pattern::RANDOM:
            result.suggested_replacer = "ARC";
            break;
        case Pattern::HOT_COLD:
            result.suggested_replacer = "ARC";
            break;
        default:
            result.suggested_replacer = "LRU";
    }
    
    return result;
}

double AdaptiveReplacer::AccessPatternAnalyzer::calculate_locality_score() const {
    if (access_history_.size() < 2) {
        return 0.0;
    }
    
    // 计算重复访问的比例
    std::set<size_t> unique_pages;
    for (const auto& entry : access_history_) {
        unique_pages.insert(entry.first);
    }
    
    double ratio = static_cast<double>(unique_pages.size()) / access_history_.size();
    return 1.0 - ratio; // 重复访问越多，局部性越高
}

double AdaptiveReplacer::AccessPatternAnalyzer::calculate_sequential_score() const {
    if (access_history_.size() < 2) {
        return 0.0;
    }
    
    size_t sequential_count = 0;
    for (size_t i = 1; i < access_history_.size(); ++i) {
        int64_t diff = static_cast<int64_t>(access_history_[i].first) - 
                      static_cast<int64_t>(access_history_[i-1].first);
        if (std::abs(diff) == 1) {
            sequential_count++;
        }
    }
    
    return static_cast<double>(sequential_count) / (access_history_.size() - 1);
}

double AdaptiveReplacer::AccessPatternAnalyzer::calculate_loop_score() const {
    if (access_history_.size() < 10) {
        return 0.0;
    }
    
    // 检查是否有循环模式
    size_t pattern_length = access_history_.size() / 2;
    for (size_t len = 2; len <= pattern_length; ++len) {
        bool is_loop = true;
        for (size_t i = len; i < access_history_.size(); ++i) {
            if (access_history_[i].first != access_history_[i - len].first) {
                is_loop = false;
                break;
            }
        }
        if (is_loop) {
            return 0.8; // 找到循环模式
        }
    }
    
    return 0.0;
}

double AdaptiveReplacer::AccessPatternAnalyzer::calculate_hot_cold_ratio() const {
    if (access_history_.empty()) {
        return 0.0;
    }
    
    std::unordered_map<size_t, size_t> access_counts;
    for (const auto& entry : access_history_) {
        access_counts[entry.first]++;
    }
    
    std::vector<size_t> counts;
    for (const auto& pair : access_counts) {
        counts.push_back(pair.second);
    }
    
    if (counts.empty()) {
        return 0.0;
    }
    
    std::sort(counts.begin(), counts.end());
    size_t median = counts[counts.size() / 2];
    size_t max_count = counts.back();
    
    return median > 0 ? static_cast<double>(max_count) / median : 0.0;
}

// ==================== LRUReplacer 实现（增强版） ====================

LRUReplacer::LRUReplacer(size_t num_pages)
    : num_pages_(num_pages), head_(nullptr), tail_(nullptr) {
    pinned_.resize(num_pages, false);
    stats_ = ReplacerStats();
}

void LRUReplacer::record_access(size_t frame_id) {
    if (frame_id >= num_pages_) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    bool hit = node_map_.find(frame_id) != node_map_.end();
    update_stats(hit);
    
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
    stats_ = ReplacerStats();
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

void LRUReplacer::update_stats(bool hit) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.total_accesses++;
    if (hit) {
        stats_.hits++;
    } else {
        stats_.compulsory_misses++;
    }
    if (stats_.total_accesses > 0) {
        stats_.hit_ratio = static_cast<double>(stats_.hits) / stats_.total_accesses;
    }
}

// 增强功能实现
void LRUReplacer::record_access_batch(const std::vector<size_t>& frame_ids) {
    for (size_t frame_id : frame_ids) {
        record_access(frame_id);
    }
}

void LRUReplacer::pin_batch(const std::vector<size_t>& frame_ids) {
    for (size_t frame_id : frame_ids) {
        pin(frame_id);
    }
}

void LRUReplacer::unpin_batch(const std::vector<size_t>& frame_ids) {
    for (size_t frame_id : frame_ids) {
        unpin(frame_id);
    }
}

std::vector<size_t> LRUReplacer::select_victims(size_t count) {
    std::vector<size_t> victims;
    victims.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        auto victim = select_victim();
        if (victim.has_value()) {
            victims.push_back(victim.value());
        } else {
            break;
        }
    }
    return victims;
}

double LRUReplacer::get_heat_score(size_t frame_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = node_map_.find(frame_id);
    if (it == node_map_.end()) {
        return 0.0;
    }
    // 距离头部越近，热度越高
    Node* node = it->second.get();
    size_t distance = 0;
    Node* current = head_;
    while (current != nullptr && current != node) {
        distance++;
        current = current->next;
    }
    return distance == 0 ? 1.0 : 1.0 / (1.0 + distance);
}

double LRUReplacer::get_cold_score(size_t frame_id) const {
    return 1.0 - get_heat_score(frame_id);
}

void LRUReplacer::tune(const std::string& param, double value) {
    // LRU 没有可调参数
    (void)param;
    (void)value;
}

IAdvancedReplacer::ReplacerStats LRUReplacer::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

std::string LRUReplacer::export_state() const {
    // 简化实现：只导出基本状态
    return "LRU";
}

bool LRUReplacer::import_state(const std::string& state) {
    // 简化实现
    (void)state;
    return true;
}

// ==================== ClockReplacer 实现（增强版） ====================

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
    stats_ = ReplacerStats();
}

void ClockReplacer::record_access(size_t frame_id) {
    if (frame_id >= num_pages_) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    bool hit = in_replacer_[frame_id].load();
    update_stats(hit);
    
    ref_bits_[frame_id].store(true);
    if (!in_replacer_[frame_id].load() && !pinned_[frame_id].load()) {
        in_replacer_[frame_id].store(true);
    }
}

void ClockReplacer::pin(size_t frame_id) {
    if (frame_id >= num_pages_) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    pinned_[frame_id].store(true);
    in_replacer_[frame_id].store(false);
}

bool ClockReplacer::unpin(size_t frame_id) {
    if (frame_id >= num_pages_) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    pinned_[frame_id].store(false);
    if (!in_replacer_[frame_id].load()) {
        in_replacer_[frame_id].store(true);
    }
    
    return true;
}

std::optional<size_t> ClockReplacer::select_victim() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    size_t start_hand = clock_hand_;
    size_t scanned = 0;
    
    while (scanned < num_pages_) {
        if (!pinned_[clock_hand_].load() && in_replacer_[clock_hand_].load()) {
            if (!ref_bits_[clock_hand_].load()) {
                // 找到受害者
                size_t victim = clock_hand_;
                in_replacer_[victim].store(false);
                ref_bits_[victim].store(false);
                clock_hand_ = (clock_hand_ + 1) % num_pages_;
                return victim;
            } else {
                // 给第二次机会
                ref_bits_[clock_hand_].store(false);
            }
        }
        
        clock_hand_ = (clock_hand_ + 1) % num_pages_;
        scanned++;
        
        if (clock_hand_ == start_hand && scanned >= num_pages_) {
            break;
        }
    }
    
    return std::nullopt;
}

void ClockReplacer::remove(size_t frame_id) {
    if (frame_id >= num_pages_) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    in_replacer_[frame_id].store(false);
    ref_bits_[frame_id].store(false);
}

size_t ClockReplacer::get_replacable_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (size_t i = 0; i < num_pages_; ++i) {
        if (!pinned_[i].load() && in_replacer_[i].load()) {
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
    stats_ = ReplacerStats();
}

void ClockReplacer::update_stats(bool hit) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.total_accesses++;
    if (hit) {
        stats_.hits++;
    } else {
        stats_.compulsory_misses++;
    }
    if (stats_.total_accesses > 0) {
        stats_.hit_ratio = static_cast<double>(stats_.hits) / stats_.total_accesses;
    }
}

// 增强功能实现
void ClockReplacer::record_access_batch(const std::vector<size_t>& frame_ids) {
    for (size_t frame_id : frame_ids) {
        record_access(frame_id);
    }
}

void ClockReplacer::pin_batch(const std::vector<size_t>& frame_ids) {
    for (size_t frame_id : frame_ids) {
        pin(frame_id);
    }
}

void ClockReplacer::unpin_batch(const std::vector<size_t>& frame_ids) {
    for (size_t frame_id : frame_ids) {
        unpin(frame_id);
    }
}

std::vector<size_t> ClockReplacer::select_victims(size_t count) {
    std::vector<size_t> victims;
    victims.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        auto victim = select_victim();
        if (victim.has_value()) {
            victims.push_back(victim.value());
        } else {
            break;
        }
    }
    return victims;
}

double ClockReplacer::get_heat_score(size_t frame_id) const {
    if (frame_id >= num_pages_) {
        return 0.0;
    }
    // 基于引用位和是否在替换器中
    bool ref = ref_bits_[frame_id].load();
    bool in = in_replacer_[frame_id].load();
    return (ref ? 0.5 : 0.0) + (in ? 0.5 : 0.0);
}

double ClockReplacer::get_cold_score(size_t frame_id) const {
    return 1.0 - get_heat_score(frame_id);
}

void ClockReplacer::tune(const std::string& param, double value) {
    // Clock 没有可调参数
    (void)param;
    (void)value;
}

IAdvancedReplacer::ReplacerStats ClockReplacer::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

std::string ClockReplacer::export_state() const {
    // 简化实现：只导出基本状态
    return "Clock";
}

bool ClockReplacer::import_state(const std::string& state) {
    // 简化实现
    (void)state;
    return true;
}

// ==================== 辅助函数声明 ====================
namespace {
    uint64_t get_current_time_ns_helper();
}

// ==================== AdaptiveReplacer 实现 ====================

AdaptiveReplacer::AdaptiveReplacer(size_t num_pages, const ReplacerConfig& config)
    : config_(config), last_adapt_time_(std::chrono::steady_clock::now()) {
    analyzer_ = std::make_unique<AccessPatternAnalyzer>(config.monitor_window_size);
    
    // 初始使用LRU
    ReplacerConfig lru_config = config;
    lru_config.type = ReplacerConfig::Type::LRU;
    current_replacer_ = std::make_unique<LRUReplacer>(num_pages);
}

AdaptiveReplacer::~AdaptiveReplacer() = default;

void AdaptiveReplacer::record_access(size_t frame_id) {
    uint64_t timestamp = get_current_time_ns_helper();
    analyzer_->record_access(frame_id, timestamp);
    current_replacer_->record_access(frame_id);
    
    // 定期检查是否需要调整策略
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_adapt_time_) >= config_.adapt_interval) {
        adapt_to_pattern();
        last_adapt_time_ = now;
    }
}

void AdaptiveReplacer::record_access_batch(const std::vector<size_t>& frame_ids) {
    for (size_t frame_id : frame_ids) {
        record_access(frame_id);
    }
}

void AdaptiveReplacer::pin(size_t frame_id) {
    current_replacer_->pin(frame_id);
}

void AdaptiveReplacer::pin_batch(const std::vector<size_t>& frame_ids) {
    for (size_t frame_id : frame_ids) {
        pin(frame_id);
    }
}

bool AdaptiveReplacer::unpin(size_t frame_id) {
    return current_replacer_->unpin(frame_id);
}

void AdaptiveReplacer::unpin_batch(const std::vector<size_t>& frame_ids) {
    for (size_t frame_id : frame_ids) {
        unpin(frame_id);
    }
}

std::optional<size_t> AdaptiveReplacer::select_victim() {
    return current_replacer_->select_victim();
}

std::vector<size_t> AdaptiveReplacer::select_victims(size_t count) {
    std::vector<size_t> victims;
    for (size_t i = 0; i < count; ++i) {
        auto victim = select_victim();
        if (victim.has_value()) {
            victims.push_back(victim.value());
            remove(victim.value());
        } else {
            break;
        }
    }
    return victims;
}

void AdaptiveReplacer::remove(size_t frame_id) {
    current_replacer_->remove(frame_id);
}

size_t AdaptiveReplacer::get_replacable_count() const {
    return current_replacer_->get_replacable_count();
}

size_t AdaptiveReplacer::get_total_count() const {
    return current_replacer_->get_total_count();
}

void AdaptiveReplacer::reset() {
    current_replacer_->reset();
    analyzer_ = std::make_unique<AccessPatternAnalyzer>(config_.monitor_window_size);
}


double AdaptiveReplacer::get_heat_score(size_t frame_id) const {
    if (auto advanced = dynamic_cast<IAdvancedReplacer*>(current_replacer_.get())) {
        return advanced->get_heat_score(frame_id);
    }
    return 0.5; // 默认值
}

double AdaptiveReplacer::get_cold_score(size_t frame_id) const {
    return 1.0 - get_heat_score(frame_id);
}

void AdaptiveReplacer::tune(const std::string& param, double value) {
    if (auto advanced = dynamic_cast<IAdvancedReplacer*>(current_replacer_.get())) {
        advanced->tune(param, value);
    }
}

IAdvancedReplacer::ReplacerStats AdaptiveReplacer::get_stats() const {
    if (auto advanced = dynamic_cast<IAdvancedReplacer*>(current_replacer_.get())) {
        return advanced->get_stats();
    }
    return ReplacerStats();
}

std::string AdaptiveReplacer::export_state() const {
    if (auto advanced = dynamic_cast<IAdvancedReplacer*>(current_replacer_.get())) {
        return advanced->export_state();
    }
    return "";
}

bool AdaptiveReplacer::import_state(const std::string& state) {
    if (auto advanced = dynamic_cast<IAdvancedReplacer*>(current_replacer_.get())) {
        return advanced->import_state(state);
    }
    return false;
}

void AdaptiveReplacer::adapt_to_pattern() {
    if (!config_.enable_adaptive_behavior) {
        return;
    }
    
    auto analysis = analyzer_->get_analysis();
    
    // 根据分析结果选择替换策略
    auto new_replacer = create_replacer_for_pattern(analysis.pattern);
    if (new_replacer) {
        // 迁移状态（简化实现）
        current_replacer_ = std::move(new_replacer);
    }
}

std::unique_ptr<IAdvancedReplacer> AdaptiveReplacer::create_replacer_for_pattern(
    AccessPatternAnalyzer::Pattern pattern) {
    
    size_t num_pages = current_replacer_->get_total_count();
    
    switch (pattern) {
        case AccessPatternAnalyzer::Pattern::SEQUENTIAL:
            return std::make_unique<LRUReplacer>(num_pages);
        case AccessPatternAnalyzer::Pattern::RANDOM:
        case AccessPatternAnalyzer::Pattern::HOT_COLD:
            return std::make_unique<ARCReplacer>(num_pages, config_.arc_config);
        default:
            return std::make_unique<LRUReplacer>(num_pages);
    }
}

// ==================== SequentialPrefetchPredictor 实现 ====================

SequentialPrefetchPredictor::SequentialPrefetchPredictor(size_t lookahead, bool adaptive)
    : lookahead_(lookahead), adaptive_(adaptive), last_page_id_(0), last_stride_(0) {
}

void SequentialPrefetchPredictor::record_access(uint64_t page_id, uint64_t timestamp_ns) {
    if (last_page_id_ > 0) {
        int64_t stride = static_cast<int64_t>(page_id) - static_cast<int64_t>(last_page_id_);
        last_stride_ = stride;
        recent_strides_.push_back(stride);
        
        // 保持最近10个步长
        if (recent_strides_.size() > 10) {
            recent_strides_.pop_front();
        }
    }
    last_page_id_ = page_id;
}

std::vector<uint64_t> SequentialPrefetchPredictor::predict_next_pages(uint64_t current_page_id, size_t max_count) {
    std::vector<uint64_t> predictions;
    
    size_t lookahead = adaptive_ ? calculate_adaptive_lookahead() : lookahead_;
    lookahead = std::min(lookahead, max_count);
    
    // 使用最近的步长进行预测
    int64_t stride = last_stride_;
    if (stride == 0 && !recent_strides_.empty()) {
        // 计算平均步长
        int64_t sum = 0;
        for (int64_t s : recent_strides_) {
            sum += s;
        }
        stride = recent_strides_.size() > 0 ? sum / static_cast<int64_t>(recent_strides_.size()) : 1;
    }
    
    if (stride == 0) {
        stride = 1; // 默认步长为1
    }
    
    for (size_t i = 1; i <= lookahead; ++i) {
        uint64_t next_page = current_page_id + static_cast<uint64_t>(stride * static_cast<int64_t>(i));
        predictions.push_back(next_page);
    }
    
    return predictions;
}

void SequentialPrefetchPredictor::train(const std::vector<std::vector<uint64_t>>& access_sequences) {
    // 简化实现：从访问序列中学习步长模式
    for (const auto& sequence : access_sequences) {
        for (size_t i = 1; i < sequence.size(); ++i) {
            int64_t stride = static_cast<int64_t>(sequence[i]) - static_cast<int64_t>(sequence[i-1]);
            recent_strides_.push_back(stride);
            if (recent_strides_.size() > 10) {
                recent_strides_.pop_front();
            }
        }
    }
}

std::string SequentialPrefetchPredictor::export_model() const {
    std::ostringstream oss;
    oss << "Sequential:" << lookahead_ << ":" << adaptive_ << ":" << last_stride_;
    return oss.str();
}

bool SequentialPrefetchPredictor::import_model(const std::string& model_data) {
    // 简化实现
    return true;
}

size_t SequentialPrefetchPredictor::calculate_adaptive_lookahead() const {
    if (recent_strides_.empty()) {
        return lookahead_;
    }
    
    // 计算步长的稳定性
    int64_t avg_stride = 0;
    for (int64_t stride : recent_strides_) {
        avg_stride += stride;
    }
    avg_stride /= static_cast<int64_t>(recent_strides_.size());
    
    // 如果步长稳定，增加预取量
    bool stable = true;
    for (int64_t stride : recent_strides_) {
        if (std::abs(stride - avg_stride) > 1) {
            stable = false;
            break;
        }
    }
    
    return stable ? lookahead_ * 2 : lookahead_;
}

// ==================== WriteCombiner 实现 ====================

WriteCombiner::WriteCombiner(size_t max_combine_size, std::chrono::milliseconds combine_window)
    : max_combine_size_(max_combine_size), combine_window_(combine_window) {
    combine_thread_ = std::thread(&WriteCombiner::combine_loop, this);
}

WriteCombiner::~WriteCombiner() {
    stop();
    if (combine_thread_.joinable()) {
        combine_thread_.join();
    }
}

std::future<bool> WriteCombiner::schedule_write(const WriteOperation& op) {
    std::promise<bool> promise;
    std::future<bool> future = promise.get_future();
    
    WriteOperation new_op;
    new_op.page_id = op.page_id;
    new_op.data = op.data;
    new_op.offset = op.offset;
    new_op.size = op.size;
    new_op.timestamp_ns = op.timestamp_ns;
    new_op.promise = std::move(promise);
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_writes_.push_back(std::move(new_op));
        stats_.total_operations++;
    }
    
    cv_.notify_one();
    return future;
}

void WriteCombiner::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    cv_.notify_all();
}

void WriteCombiner::stop() {
    running_.store(false);
    cv_.notify_all();
}

WriteCombiner::Stats WriteCombiner::get_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats result = stats_;
    if (result.total_operations > 0) {
        result.combine_ratio = static_cast<double>(result.combined_operations) / result.total_operations;
    }
    return result;
}

void WriteCombiner::combine_loop() {
    while (running_.load()) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        if (pending_writes_.empty()) {
            cv_.wait_for(lock, combine_window_);
            continue;
        }
        
        auto batch = extract_batch();
        lock.unlock();
        
        if (!batch.empty()) {
            // 执行合并写入（简化实现）
            for (auto& op : batch) {
                op.promise.set_value(true);
            }
            
            {
                std::lock_guard<std::mutex> stats_lock(mutex_);
                stats_.flushed_batches++;
                stats_.combined_operations += batch.size();
                stats_.saved_io_ops += batch.size() > 1 ? batch.size() - 1 : 0;
            }
        }
    }
}

std::vector<WriteCombiner::WriteOperation> WriteCombiner::extract_batch() {
    std::vector<WriteOperation> batch;
    
    if (pending_writes_.empty()) {
        return batch;
    }
    
    auto cutoff_time = std::chrono::steady_clock::now() + combine_window_;
    
    while (batch.size() < max_combine_size_ && !pending_writes_.empty()) {
        auto now = std::chrono::steady_clock::now();
        if (now >= cutoff_time) {
            break;
        }
        
        batch.push_back(std::move(pending_writes_.front()));
        pending_writes_.pop_front();
    }
    
    return batch;
}

// ==================== EnhancedBufferFrame 实现 ====================

EnhancedBufferFrame::EnhancedBufferFrame(size_t page_size, size_t frame_id)
    : page_(), frame_id_(frame_id) {
    // 成员变量已在声明时初始化
    // 首次访问时间设为0，表示尚未访问
    first_access_time_.store(0);
    last_access_time_.store(0);
    access_count_.store(0);
    state_.store(PageState::FREE);
    // page_size 参数保留用于兼容，Page 是固定 4KB 大小
    (void)page_size;
}

// ========== 基础方法实现 ==========

void EnhancedBufferFrame::pin() {
    pin_count_.fetch_add(1, std::memory_order_relaxed);
}

void EnhancedBufferFrame::unpin() {
    uint32_t old_count = pin_count_.fetch_sub(1, std::memory_order_relaxed);
    if (old_count == 0) {
        // 已经是0，不应该再减少
        pin_count_.store(0);
    }
}

void EnhancedBufferFrame::mark_accessed() {
    uint64_t now = bufferpool_now_ns();
    last_access_time_.store(now, std::memory_order_relaxed);
    access_count_.fetch_add(1, std::memory_order_relaxed);
    
    // 如果是第一次访问，记录首次访问时间
    uint64_t first = first_access_time_.load();
    if (first == 0) {
        first_access_time_.store(now, std::memory_order_relaxed);
    }
}

bool EnhancedBufferFrame::try_lock() {
    bool expected = false;
    return locked_.compare_exchange_strong(expected, true, std::memory_order_acquire);
}

void EnhancedBufferFrame::unlock() {
    locked_.store(false, std::memory_order_release);
}

double EnhancedBufferFrame::heat_score() const {
    uint64_t now = get_current_time_ns_helper();
    uint64_t last_access = last_access_time();
    uint32_t count = access_count();
    
    if (count == 0 || last_access == 0) {
        return 0.0;
    }
    
    // 热度 = 访问次数 / (时间衰减因子)
    uint64_t age_ns = now > last_access ? now - last_access : 1;
    double time_decay = 1.0 + (age_ns / 1e9); // 秒级衰减
    return static_cast<double>(count) / time_decay;
}

void EnhancedBufferFrame::record_io_time(uint64_t read_time_ns, uint64_t write_time_ns) {
    if (read_time_ns > 0) {
        total_read_time_ns_.fetch_add(read_time_ns);
    }
    if (write_time_ns > 0) {
        total_write_time_ns_.fetch_add(write_time_ns);
    }
}

uint32_t EnhancedBufferFrame::calculate_checksum() const {
    // 简化实现：使用简单的校验和
    const char* data = static_cast<const char*>(raw_data());
    size_t size = kPageSize;
    uint32_t checksum = 0;
    for (size_t i = 0; i < size; ++i) {
        checksum = (checksum << 1) ^ static_cast<uint8_t>(data[i]);
    }
    return checksum;
}

bool EnhancedBufferFrame::validate_checksum() const {
    uint32_t current = calculate_checksum();
    uint32_t stored = checksum_.load();
    return current == stored;
}

bool EnhancedBufferFrame::compress() {
#ifdef HAVE_ZSTD
    std::lock_guard<std::mutex> lock(compress_mutex_);
    
    if (compressed_.load()) {
        return true; // 已经压缩
    }
    
    // 使用 ZSTD 压缩
    const char* src_data = static_cast<const char*>(raw_data());
    size_t src_size = kPageSize;
    
    // ZSTD 压缩级别（1-22，默认3，数值越大压缩率越高但速度越慢）
    // 对于页面压缩，使用中等压缩级别以平衡速度和压缩率
    const int compression_level = 3;
    
    // 计算压缩后的最大大小（ZSTD 建议使用 ZSTD_compressBound）
    size_t max_compressed_size = ZSTD_compressBound(src_size);
    
    // 分配压缩缓冲区
    compressed_buffer_.resize(max_compressed_size);
    
    // 执行压缩
    size_t compressed_size = ZSTD_compress(
        compressed_buffer_.data(), max_compressed_size,
        src_data, src_size,
        compression_level
    );
    
    // 检查压缩是否成功
    if (ZSTD_isError(compressed_size)) {
        // 压缩失败
        compressed_buffer_.clear();
        return false;
    }
    
    // 检查压缩是否有效（压缩后大小小于原大小，且至少节省10%）
    // 如果压缩效果不明显，保持未压缩状态
    const double min_compression_ratio = 0.9; // 至少压缩10%
    if (compressed_size < src_size * min_compression_ratio) {
        // 调整缓冲区大小到实际压缩后的大小
        compressed_buffer_.resize(compressed_size);
        compressed_.store(true);
        compressed_size_.store(compressed_size);
        return true;
    } else {
        // 压缩效果不明显，保持未压缩状态
        compressed_buffer_.clear();
        return false;
    }
#else
    // ZSTD 未启用，压缩功能不可用
    return false;
#endif
}

bool EnhancedBufferFrame::decompress() {
#ifdef HAVE_ZSTD
    std::lock_guard<std::mutex> lock(compress_mutex_);
    
    if (!compressed_.load()) {
        return true; // 未压缩，无需解压
    }
    
    if (compressed_buffer_.empty()) {
        return false; // 没有压缩数据
    }
    
    // 使用 ZSTD 解压
    char* dst_data = static_cast<char*>(raw_data());
    size_t dst_size = kPageSize;
    size_t compressed_size = compressed_buffer_.size();
    
    // 执行解压
    size_t decompressed_size = ZSTD_decompress(
        dst_data, dst_size,
        compressed_buffer_.data(), compressed_size
    );
    
    // 检查解压是否成功
    if (ZSTD_isError(decompressed_size)) {
        // 解压失败
        return false;
    }
    
    // 检查解压后的大小是否正确
    if (decompressed_size != dst_size) {
        // 解压后大小不匹配（可能是数据损坏）
        return false;
    }
    
    // 解压成功，清除压缩状态
    compressed_.store(false);
    compressed_size_.store(0);
    compressed_buffer_.clear();
    
    return true;
#else
    // ZSTD 未启用，解压功能不可用
    return false;
#endif
}

// ==================== EnhancedBufferPoolConfig 实现 ====================

EnhancedBufferPoolConfig EnhancedBufferPoolConfig::default_config() {
    EnhancedBufferPoolConfig config;
    config.pool_size = 1024;
    config.page_size = kPageSize;
    config.replacer_type = EnhancedBufferPoolConfig::ReplacerType::LRU;
    config.flush_strategy = EnhancedBufferPoolConfig::FlushStrategy::WRITE_BACK;
    
    // 动态配置
    config.dynamic_config.enabled = false;
    config.dynamic_config.min_pool_size = 64;
    config.dynamic_config.max_pool_size = 65536;
    
    // 预取配置
    config.prefetch_config.enabled = true;
    config.prefetch_config.strategy = PrefetchConfig::Strategy::SEQUENTIAL;
    config.prefetch_config.lookahead = 4;
    
    // 写优化配置
    config.write_optimization_config.enable_write_combining = true;
    config.write_optimization_config.max_combine_size = 16;
    
    // 监控配置
    config.monitoring_config.enable_performance_counters = true;
    
    return config;
}

bool EnhancedBufferPoolConfig::validate() const {
    // 基础配置验证
    if (pool_size == 0 || page_size == 0) {
        return false;
    }
    if (pool_size > 65536) {
        return false; // 限制最大缓冲池大小
    }
    
    if (dynamic_config.enabled) {
        if (dynamic_config.min_pool_size > dynamic_config.max_pool_size) {
            return false;
        }
    }
    
    return true;
}

// ==================== EnhancedBufferPool 核心实现 ====================
// 注意：这是一个简化实现，完整实现需要更多代码

EnhancedBufferPool::BufferPoolPtr EnhancedBufferPool::create(
    FileManager* file_manager,
    const EnhancedBufferPoolConfig& config) {
    
    if (!file_manager || !config.validate()) {
        return nullptr;
    }
    
    auto pool = BufferPoolPtr(new EnhancedBufferPool(file_manager, config));
    if (!pool->init()) {
        return nullptr;
    }
    
    return pool;
}

EnhancedBufferPool::EnhancedBufferPool(FileManager* file_manager, const EnhancedBufferPoolConfig& config)
    : config_(config), file_manager_(file_manager) {
}

EnhancedBufferPool::~EnhancedBufferPool() {
    cleanup();
}

bool EnhancedBufferPool::init() {
    // 创建替换器
    ReplacerConfig replacer_config;
    replacer_config.type = ReplacerConfig::Type::LRU; // 默认使用LRU
    
    switch (config_.replacer_type) {
        case EnhancedBufferPoolConfig::ReplacerType::LRU:
            replacer_ = std::make_unique<LRUReplacer>(config_.pool_size);
            break;
        case EnhancedBufferPoolConfig::ReplacerType::CLOCK:
            replacer_ = std::make_unique<ClockReplacer>(config_.pool_size);
            break;
        default:
            replacer_ = std::make_unique<LRUReplacer>(config_.pool_size);
    }
    
    // 创建预取预测器
    if (config_.prefetch_config.enabled) {
        prefetch_predictor_ = std::make_unique<SequentialPrefetchPredictor>(
            config_.prefetch_config.lookahead,
            config_.prefetch_config.adaptive_lookahead);
    }
    
    // 创建写合并器
    if (config_.write_optimization_config.enable_write_combining) {
        write_combiner_ = std::make_unique<WriteCombiner>(
            config_.write_optimization_config.max_combine_size,
            std::chrono::milliseconds(config_.write_optimization_config.combine_window_ms));
    }
    
    // 分配帧
    frames_.reserve(config_.pool_size);
    frame_to_page_.resize(config_.pool_size, 0);
    
    for (size_t i = 0; i < config_.pool_size; ++i) {
        frames_.push_back(std::make_unique<EnhancedBufferFrame>(config_.page_size, i));
        free_list_.push_back(i);
    }
    
    // 初始化统计信息
    stats_.total_frames = config_.pool_size;
    stats_.free_frames = config_.pool_size;
    
    // 启动后台线程
    start_background_threads();
    
    return true;
}

void EnhancedBufferPool::cleanup() {
    stop_background_threads();
    flush_all();
}

EnhancedBufferFrame* EnhancedBufferPool::fetch_page(uint64_t page_id, bool exclusive) {
    auto start_time = std::chrono::steady_clock::now();
    BUFFER_POOL_PERF_COUNTER(fetch_page_calls);
    
    if (page_id == 0) {
        return nullptr;
    }
    
    // 查找页面是否已在缓冲池中
    {
        std::shared_lock lock(page_table_mutex_);
        auto it = page_table_.find(page_id);
        if (it != page_table_.end()) {
            size_t frame_id = it->second;
            EnhancedBufferFrame* frame = get_frame(frame_id);
            if (frame) {
                frame->mark_accessed();
                frame->pin();
                replacer_->record_access(frame_id);
                BUFFER_POOL_PERF_COUNTER(fetch_page_hits);
                
                auto end_time = std::chrono::steady_clock::now();
                auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
                perf_counters_.fetch_latency.record(latency);
                
                record_access_history(page_id);
                return frame;
            }
        }
    }
    
    // 缓存未命中
    BUFFER_POOL_PERF_COUNTER(fetch_page_misses);
    
    // 加载页面
    EnhancedBufferFrame* frame = load_page(page_id, exclusive);
    
    auto end_time = std::chrono::steady_clock::now();
    auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
    perf_counters_.fetch_latency.record(latency);
    
    if (frame) {
        record_access_history(page_id);
    }
    
    return frame;
}

EnhancedBufferFrame* EnhancedBufferPool::new_page() {
    BUFFER_POOL_PERF_COUNTER(new_page_calls);
    
    // 分配新页面ID
    // 实际实现应该从PageAllocator或FileManager获取
    // 这里使用简化实现，实际应该：
    // 1. 调用PageAllocator::allocate_page()获取新页面ID
    // 2. 或者从FileManager获取下一个可用的页面ID
    // 3. 确保页面在文件中已分配空间
    // 4. 初始化页面头信息
    
    static std::atomic<uint64_t> next_page_id{1};
    uint64_t page_id = next_page_id.fetch_add(1);
    
    // 实际实现中，新页面可能需要：
    // if (page_allocator_) {
    //     page_id = page_allocator_->allocate_page();
    //     if (page_id == 0) {
    //         return nullptr; // 分配失败
    //     }
    // }
    
    return load_page(page_id, false);
}

void EnhancedBufferPool::unpin_page(uint64_t page_id, bool mark_dirty) {
    BUFFER_POOL_PERF_COUNTER(unpin_page_calls);
    
    std::shared_lock lock(page_table_mutex_);
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return;
    }
    
    size_t frame_id = it->second;
    EnhancedBufferFrame* frame = get_frame(frame_id);
    if (!frame) {
        return;
    }
    
    if (mark_dirty) {
        frame->mark_dirty();
    }
    
    frame->unpin();
    replacer_->unpin(frame_id);
}

bool EnhancedBufferPool::delete_page(uint64_t page_id) {
    std::unique_lock lock(page_table_mutex_);
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }
    
    size_t frame_id = it->second;
    EnhancedBufferFrame* frame = get_frame(frame_id);
    if (frame && frame->is_pinned()) {
        return false;
    }
    
    page_table_.erase(it);
    replacer_->remove(frame_id);
    deallocate_frame(frame_id);
    
    return true;
}

std::vector<EnhancedBufferFrame*> EnhancedBufferPool::fetch_pages(
    const std::vector<uint64_t>& page_ids, bool exclusive) {
    std::vector<EnhancedBufferFrame*> frames;
    frames.reserve(page_ids.size());
    
    for (uint64_t page_id : page_ids) {
        auto frame = fetch_page(page_id, exclusive);
        if (frame) {
            frames.push_back(frame);
        }
    }
    
    return frames;
}

std::vector<EnhancedBufferFrame*> EnhancedBufferPool::new_pages(size_t count) {
    std::vector<EnhancedBufferFrame*> frames;
    frames.reserve(count);
    
    for (size_t i = 0; i < count; ++i) {
        auto frame = new_page();
        if (frame) {
            frames.push_back(frame);
        }
    }
    
    return frames;
}

void EnhancedBufferPool::unpin_pages(const std::vector<uint64_t>& page_ids, bool mark_dirty) {
    for (uint64_t page_id : page_ids) {
        unpin_page(page_id, mark_dirty);
    }
}

bool EnhancedBufferPool::prefetch_pages(const std::vector<uint64_t>& page_ids, bool low_priority) {
    if (!config_.prefetch_config.enabled) {
        return false;
    }
    
    BUFFER_POOL_PERF_COUNTER(prefetch_calls);
    
    std::lock_guard<std::mutex> lock(prefetch_queue_mutex_);
    for (uint64_t page_id : page_ids) {
        PrefetchRequest req;
        req.page_id = page_id;
        req.priority = low_priority ? 0 : 100;
        req.timestamp_ns = get_current_time_ns();
        req.predictive = false;
        prefetch_queue_.push(req);
    }
    
    return true;
}

bool EnhancedBufferPool::prefetch_predictive(uint64_t current_page_id) {
    if (!prefetch_predictor_) {
        return false;
    }
    
    auto predictions = prefetch_predictor_->predict_next_pages(
        current_page_id, config_.prefetch_config.max_concurrent_prefetch);
    
    return prefetch_pages(predictions, true);
}

void EnhancedBufferPool::flush_all() {
    std::shared_lock lock(page_table_mutex_);
    for (const auto& pair : page_table_) {
        flush_page(pair.first);
    }
}

bool EnhancedBufferPool::flush_page(uint64_t page_id) {
    BUFFER_POOL_PERF_COUNTER(flush_page_calls);
    
    std::shared_lock lock(page_table_mutex_);
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }
    
    size_t frame_id = it->second;
    return flush_frame(frame_id);
}

void EnhancedBufferPool::flush_dirty_pages(bool async) {
    std::vector<uint64_t> dirty_pages;
    
    {
        std::shared_lock lock(page_table_mutex_);
        for (const auto& pair : page_table_) {
            size_t frame_id = pair.second;
            EnhancedBufferFrame* frame = get_frame(frame_id);
            if (frame && frame->is_dirty()) {
                dirty_pages.push_back(pair.first);
            }
        }
    }
    
    if (async) {
        // 异步刷盘（简化实现）
        for (uint64_t page_id : dirty_pages) {
            flush_page(page_id);
        }
    } else {
        for (uint64_t page_id : dirty_pages) {
            flush_page(page_id);
        }
    }
}

EnhancedBufferFrame* EnhancedBufferPool::get_frame(size_t frame_id) {
    if (frame_id >= frames_.size()) {
        return nullptr;
    }
    return frames_[frame_id].get();
}

size_t EnhancedBufferPool::allocate_frame() {
    std::unique_lock<std::shared_mutex> lock(free_list_mutex_);
    
    if (!free_list_.empty()) {
        size_t frame_id = free_list_.back();
        free_list_.pop_back();
        return frame_id;
    }
    
    // 没有空闲帧，需要驱逐
    if (auto advanced = dynamic_cast<IAdvancedReplacer*>(replacer_.get())) {
        auto victims = advanced->select_victims(1);
        if (!victims.empty()) {
            size_t frame_id = victims[0];
            evict_frame(frame_id);
            return frame_id;
        }
    } else {
        auto victim = replacer_->select_victim();
        if (victim.has_value()) {
            size_t frame_id = victim.value();
            evict_frame(frame_id);
            return frame_id;
        }
    }
    
    return SIZE_MAX;
}

void EnhancedBufferPool::deallocate_frame(size_t frame_id) {
    EnhancedBufferFrame* frame = get_frame(frame_id);
    if (!frame) {
        return;
    }
    
    frame->set_page_id(0);
    frame->clear_dirty();
    frame->unpin();
    frame_to_page_[frame_id] = 0;
    
    std::unique_lock<std::shared_mutex> lock(free_list_mutex_);
    free_list_.push_back(frame_id);
}

EnhancedBufferFrame* EnhancedBufferPool::load_page(uint64_t page_id, bool exclusive) {
    size_t frame_id = allocate_frame();
    if (frame_id == SIZE_MAX) {
        return nullptr;
    }
    
    EnhancedBufferFrame* frame = get_frame(frame_id);
    if (!frame) {
        return nullptr;
    }
    
    // 如果帧中有旧页面，先驱逐
    if (frame->is_valid()) {
        evict_frame(frame_id);
    }
    
    // 从文件加载页面
    // 计算页面在文件中的偏移量
    uint64_t offset = page_id * config_.page_size;
    
    // 实际实现需要调用FileManager读取页面
    if (file_manager_) {
        // 实际实现需要：
        // 1. 根据page_id确定文件路径（可能通过PageAllocator或文件映射）
        // 2. 打开或获取文件句柄（可以使用FileCacheManager）
        // 3. 使用pread读取页面数据到frame->page().GetData()
        // 4. 处理IO错误和页面不存在的情况
        //
        // 示例代码框架：
        // std::string data_file_path = get_data_file_path(page_id);
        // auto file_handle = file_cache_manager_->get_file(data_file_path, 
        //                                                  FileHandle::OpenMode::ReadWrite,
        //                                                  file_config_);
        // if (file_handle && file_handle->is_open()) {
        //     auto start_io = std::chrono::steady_clock::now();
        //     size_t bytes_read = file_handle->pread(frame->page().GetData(), 
        //                                            config_.page_size, offset);
        //     auto end_io = std::chrono::steady_clock::now();
        //     auto io_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
        //         end_io - start_io).count();
        //     
        //     if (bytes_read != config_.page_size) {
        //         // 读取失败或页面不存在（可能是新页面）
        //         // 对于新页面，可以初始化为空页面
        //         std::memset(frame->page().GetData(), 0, config_.page_size);
        //     }
        //     
        //     // 记录IO统计
        //     frame->record_io_time(io_time, 0);
        //     perf_counters_.io_read_bytes += bytes_read;
        //     perf_counters_.io_read_time_ns += io_time;
        // } else {
        //     // 文件打开失败
        //     return nullptr;
        // }
        
        // 当前简化实现：初始化页面数据
        // 对于新页面或首次访问，数据可能未初始化
        std::memset(frame->page().GetData(), 0, config_.page_size);
    }
    
    frame->set_page_id(page_id);
    frame->pin();
    
    // 更新访问统计
    uint64_t now = get_current_time_ns_helper();
    frame->mark_accessed(); // This will handle first_access_time and last_access_time
    
    // 调用父类的mark_accessed（更新父类的访问时间）
    frame->mark_accessed();
    frame->set_state(EnhancedBufferFrame::PageState::CLEAN);
    
    // 更新页表
    {
        std::unique_lock lock(page_table_mutex_);
        page_table_[page_id] = frame_id;
    }
    
    frame_to_page_[frame_id] = page_id;
    
    // 通知替换器
    replacer_->pin(frame_id);
    
    return frame;
}

bool EnhancedBufferPool::evict_frame(size_t frame_id) {
    BUFFER_POOL_PERF_COUNTER(evict_page_calls);
    
    EnhancedBufferFrame* frame = get_frame(frame_id);
    if (!frame || !frame->is_valid()) {
        return false;
    }
    
    uint64_t page_id = frame->page_id();
    
    // 如果是脏页，先刷盘
    if (frame->is_dirty()) {
        if (!flush_frame(frame_id)) {
            return false;
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
    frame_to_page_[frame_id] = 0;
    
    return true;
}

bool EnhancedBufferPool::flush_frame(size_t frame_id) {
    auto start_time = std::chrono::steady_clock::now();
    
    EnhancedBufferFrame* frame = get_frame(frame_id);
    if (!frame || !frame->is_valid() || !frame->is_dirty()) {
        return true;
    }
    
    uint64_t page_id = frame->page_id();
    
    // 计算页面在文件中的偏移量
    uint64_t offset = page_id * config_.page_size;
    
    // 实际实现需要调用FileManager写入页面
    if (file_manager_) {
        // 实际实现需要：
        // 1. 根据page_id确定文件路径
        // 2. 打开或获取文件句柄
        // 3. 使用pwrite写入页面数据
        // 4. 确保写入成功并持久化
        //
        // 示例代码框架：
        // std::string data_file_path = get_data_file_path(page_id);
        // auto file_handle = file_cache_manager_->get_file(data_file_path, 
        //                                                  FileHandle::OpenMode::ReadWrite,
        //                                                  file_config_);
        // if (file_handle && file_handle->is_open()) {
        //     // 确保文件足够大（可能需要扩展文件）
        //     uint64_t required_size = offset + config_.page_size;
        //     if (file_handle->size() < required_size) {
        //         file_handle->truncate(required_size);
        //     }
        //     
        //     auto start_io = std::chrono::steady_clock::now();
        //     size_t bytes_written = file_handle->pwrite(frame->page().GetData(), 
        //                                                 config_.page_size, offset);
        //     auto end_io = std::chrono::steady_clock::now();
        //     auto io_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
        //         end_io - start_io).count();
        //     
        //     if (bytes_written != config_.page_size) {
        //         return false; // 写入失败
        //     }
        //     
        //     // 根据刷盘策略决定是否立即同步
        //     if (config_.flush_strategy == EnhancedBufferPoolConfig::FlushStrategy::WRITE_THROUGH) {
        //         file_handle->sync(); // 立即同步到磁盘
        //     }
        //     
        //     // 记录IO统计
        //     frame->record_io_time(0, io_time);
        //     perf_counters_.io_write_bytes += bytes_written;
        //     perf_counters_.io_write_time_ns += io_time;
        //     
        //     return true;
        // }
        
        // 当前简化实现：假设写入成功
        // 实际使用时需要实现完整的IO逻辑
    }
    
    frame->clear_dirty();
    
    auto end_time = std::chrono::steady_clock::now();
    auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
    perf_counters_.flush_latency.record(latency);
    
    return true;
}

void EnhancedBufferPool::start_background_threads() {
    if (running_.exchange(true)) {
        return;
    }
    
    if (config_.flush_strategy == EnhancedBufferPoolConfig::FlushStrategy::PERIODIC ||
        config_.flush_strategy == EnhancedBufferPoolConfig::FlushStrategy::WRITE_BACK) {
        flush_thread_ = std::thread(&EnhancedBufferPool::flush_thread_func, this);
    }
    
    if (config_.prefetch_config.enabled) {
        prefetch_thread_ = std::thread(&EnhancedBufferPool::prefetch_thread_func, this);
    }
    
    if (config_.enable_statistics) {
        stats_thread_ = std::thread(&EnhancedBufferPool::stats_thread_func, this);
    }
}

void EnhancedBufferPool::stop_background_threads() {
    if (!running_.exchange(false)) {
        return;
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

void EnhancedBufferPool::flush_thread_func() {
    while (running_.load()) {
        std::this_thread::sleep_for(config_.flush_interval);
        
        if (!running_.load()) {
            break;
        }
        
        flush_dirty_pages(true);
    }
}

void EnhancedBufferPool::prefetch_thread_func() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        if (!running_.load()) {
            break;
        }
        
        process_prefetch_queue();
    }
}

void EnhancedBufferPool::stats_thread_func() {
    while (running_.load()) {
        std::this_thread::sleep_for(config_.stats_interval);
        
        if (!running_.load()) {
            break;
        }
        
        update_statistics();
    }
}

void EnhancedBufferPool::process_prefetch_queue() {
    std::vector<PrefetchRequest> requests;
    
    {
        std::lock_guard<std::mutex> lock(prefetch_queue_mutex_);
        size_t count = std::min(config_.prefetch_config.max_concurrent_prefetch, 
                               static_cast<size_t>(prefetch_queue_.size()));
        for (size_t i = 0; i < count; ++i) {
            if (!prefetch_queue_.empty()) {
                requests.push_back(prefetch_queue_.top());
                prefetch_queue_.pop();
            }
        }
    }
    
    for (const auto& req : requests) {
        fetch_page(req.page_id, false);
    }
}

void EnhancedBufferPool::update_statistics() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    // 更新基本统计
    stats_.used_frames = page_table_.size();
    stats_.free_frames = config_.pool_size - stats_.used_frames;
    stats_.total_frames = config_.pool_size;
    
    // 统计脏页和固定页
    size_t dirty_count = 0;
    size_t pinned_count = 0;
    for (const auto& pair : page_table_) {
        size_t frame_id = pair.second;
        EnhancedBufferFrame* frame = get_frame(frame_id);
        if (frame) {
            if (frame->is_dirty()) {
                dirty_count++;
            }
            if (frame->is_pinned()) {
                pinned_count++;
            }
        }
    }
    stats_.dirty_frames = dirty_count;
    stats_.pinned_frames = pinned_count;
    
    // 更新性能计数器
    stats_.perf_counters = perf_counters_;
    
    // 更新内存使用统计
    stats_.total_memory_bytes = config_.pool_size * config_.page_size;
    stats_.used_memory_bytes = stats_.used_frames * config_.page_size;
    stats_.wasted_memory_bytes = stats_.free_frames * config_.page_size;
    stats_.memory_utilization = stats_.total_memory_bytes > 0 
        ? static_cast<double>(stats_.used_memory_bytes) / stats_.total_memory_bytes 
        : 0.0;
    
    // 更新IO统计
    stats_.read_operations = perf_counters_.fetch_page_calls;
    stats_.write_operations = perf_counters_.flush_page_calls;
    stats_.total_io_operations = stats_.read_operations + stats_.write_operations;
    stats_.avg_read_latency_ms = perf_counters_.avg_read_latency_ns() / 1e6;
    stats_.avg_write_latency_ms = perf_counters_.avg_write_latency_ns() / 1e6;
    
    // 计算IO吞吐量（简化实现）
    if (perf_counters_.io_read_time_ns + perf_counters_.io_write_time_ns > 0) {
        uint64_t total_time_ns = perf_counters_.io_read_time_ns + perf_counters_.io_write_time_ns;
        uint64_t total_bytes = perf_counters_.io_read_bytes + perf_counters_.io_write_bytes;
        double total_time_sec = total_time_ns / 1e9;
        stats_.io_throughput_bytes_per_sec = total_time_sec > 0 
            ? static_cast<uint64_t>(total_bytes / total_time_sec) 
            : 0;
    }
    
    // 更新预取效果（简化实现）
    if (perf_counters_.prefetch_calls > 0) {
        stats_.prefetch_accuracy = static_cast<double>(perf_counters_.prefetch_hits) 
                                   / perf_counters_.prefetch_calls;
    }
    
    // 更新写优化效果
    if (write_combiner_) {
        auto wc_stats = write_combiner_->get_stats();
        stats_.write_combine_ratio = wc_stats.combined_operations;
        stats_.group_commit_count = wc_stats.flushed_batches;
    }
}

void EnhancedBufferPool::record_access_history(uint64_t page_id) {
    if (config_.monitoring_config.enable_access_tracing) {
        std::lock_guard<std::mutex> lock(history_mutex_);
        access_history_.push_back(page_id);
        if (access_history_.size() > config_.monitoring_config.trace_buffer_size) {
            access_history_.pop_front();
        }
    }
}

EnhancedBufferPool::EnhancedStatistics EnhancedBufferPool::get_statistics() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    EnhancedStatistics result = stats_;
    result.perf_counters = perf_counters_;
    
    // 更新实时统计
    uint64_t hits = perf_counters_.fetch_page_hits;
    uint64_t misses = perf_counters_.fetch_page_misses;
    uint64_t total = hits + misses;
    result.hit_count = hits;
    result.miss_count = misses;
    result.hit_ratio = total > 0 ? static_cast<double>(hits) / total : 0.0;
    result.evict_count = perf_counters_.evict_page_calls;
    result.flush_count = perf_counters_.flush_page_calls;
    result.prefetch_count = perf_counters_.prefetch_calls;
    
    return result;
}

// EnhancedStatistics::to_string() 实现
std::string EnhancedBufferPool::EnhancedStatistics::to_string(bool detailed) const {
    std::ostringstream oss;
    oss << "=== Enhanced BufferPool Statistics ===\n\n";
    
    // 基本统计
    oss << "Basic Statistics:\n";
    oss << "  Total Frames: " << total_frames << "\n";
    oss << "  Used Frames: " << used_frames << "\n";
    oss << "  Free Frames: " << free_frames << "\n";
    oss << "  Dirty Frames: " << dirty_frames << "\n";
    oss << "  Pinned Frames: " << pinned_frames << "\n";
    oss << "  Hit Ratio: " << std::fixed << std::setprecision(2) << (hit_ratio * 100) << "%\n";
    oss << "  Hit Count: " << hit_count << "\n";
    oss << "  Miss Count: " << miss_count << "\n";
    oss << "  Evict Count: " << evict_count << "\n";
    oss << "  Flush Count: " << flush_count << "\n";
    oss << "  Prefetch Count: " << prefetch_count << "\n";
    
    if (detailed) {
        // 内存使用
        oss << "\nMemory Usage:\n";
        oss << "  Total Memory: " << (total_memory_bytes / 1024 / 1024) << " MB\n";
        oss << "  Used Memory: " << (used_memory_bytes / 1024 / 1024) << " MB\n";
        oss << "  Wasted Memory: " << (wasted_memory_bytes / 1024 / 1024) << " MB\n";
        oss << "  Memory Utilization: " << std::fixed << std::setprecision(2) 
            << (memory_utilization * 100) << "%\n";
        
        // IO统计
        oss << "\nIO Statistics:\n";
        oss << "  Total IO Operations: " << total_io_operations << "\n";
        oss << "  Read Operations: " << read_operations << "\n";
        oss << "  Write Operations: " << write_operations << "\n";
        oss << "  Avg Read Latency: " << std::fixed << std::setprecision(2) 
            << avg_read_latency_ms << " ms\n";
        oss << "  Avg Write Latency: " << std::fixed << std::setprecision(2) 
            << avg_write_latency_ms << " ms\n";
        oss << "  IO Throughput: " << (io_throughput_bytes_per_sec / 1024 / 1024) << " MB/s\n";
        
        // 预取效果
        oss << "\nPrefetch Statistics:\n";
        oss << "  Prefetch Accuracy: " << std::fixed << std::setprecision(2) 
            << (prefetch_accuracy * 100) << "%\n";
        oss << "  Prefetch Coverage: " << std::fixed << std::setprecision(2) 
            << (prefetch_coverage * 100) << "%\n";
        
        // 写优化效果
        oss << "\nWrite Optimization:\n";
        oss << "  Write Combine Ratio: " << write_combine_ratio << "\n";
        oss << "  Group Commit Count: " << group_commit_count << "\n";
        
        // 访问模式
        oss << "\nAccess Pattern:\n";
        oss << "  Pattern: " << access_pattern << "\n";
        oss << "  Locality Score: " << std::fixed << std::setprecision(3) << locality_score << "\n";
        oss << "  Sequential Score: " << std::fixed << std::setprecision(3) << sequential_score << "\n";
        
        // 性能计数器详情
        oss << "\nPerformance Counters:\n";
        oss << perf_counters.to_string();
    }
    
    return oss.str();
}

// EnhancedStatistics::to_json() 实现
std::string EnhancedBufferPool::EnhancedStatistics::to_json() const {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"basic\": {\n";
    oss << "    \"total_frames\": " << total_frames << ",\n";
    oss << "    \"used_frames\": " << used_frames << ",\n";
    oss << "    \"free_frames\": " << free_frames << ",\n";
    oss << "    \"dirty_frames\": " << dirty_frames << ",\n";
    oss << "    \"pinned_frames\": " << pinned_frames << ",\n";
    oss << "    \"hit_ratio\": " << std::fixed << std::setprecision(4) << hit_ratio << ",\n";
    oss << "    \"hit_count\": " << hit_count << ",\n";
    oss << "    \"miss_count\": " << miss_count << ",\n";
    oss << "    \"evict_count\": " << evict_count << ",\n";
    oss << "    \"flush_count\": " << flush_count << ",\n";
    oss << "    \"prefetch_count\": " << prefetch_count << "\n";
    oss << "  },\n";
    oss << "  \"memory\": {\n";
    oss << "    \"total_memory_bytes\": " << total_memory_bytes << ",\n";
    oss << "    \"used_memory_bytes\": " << used_memory_bytes << ",\n";
    oss << "    \"wasted_memory_bytes\": " << wasted_memory_bytes << ",\n";
    oss << "    \"memory_utilization\": " << std::fixed << std::setprecision(4) << memory_utilization << "\n";
    oss << "  },\n";
    oss << "  \"io\": {\n";
    oss << "    \"total_operations\": " << total_io_operations << ",\n";
    oss << "    \"read_operations\": " << read_operations << ",\n";
    oss << "    \"write_operations\": " << write_operations << ",\n";
    oss << "    \"avg_read_latency_ms\": " << std::fixed << std::setprecision(2) << avg_read_latency_ms << ",\n";
    oss << "    \"avg_write_latency_ms\": " << std::fixed << std::setprecision(2) << avg_write_latency_ms << ",\n";
    oss << "    \"io_throughput_bytes_per_sec\": " << io_throughput_bytes_per_sec << "\n";
    oss << "  },\n";
    oss << "  \"prefetch\": {\n";
    oss << "    \"accuracy\": " << std::fixed << std::setprecision(4) << prefetch_accuracy << ",\n";
    oss << "    \"coverage\": " << std::fixed << std::setprecision(4) << prefetch_coverage << "\n";
    oss << "  },\n";
    oss << "  \"write_optimization\": {\n";
    oss << "    \"write_combine_ratio\": " << write_combine_ratio << ",\n";
    oss << "    \"group_commit_count\": " << group_commit_count << "\n";
    oss << "  },\n";
    oss << "  \"access_pattern\": {\n";
    oss << "    \"pattern\": \"" << access_pattern << "\",\n";
    oss << "    \"locality_score\": " << std::fixed << std::setprecision(4) << locality_score << ",\n";
    oss << "    \"sequential_score\": " << std::fixed << std::setprecision(4) << sequential_score << "\n";
    oss << "  },\n";
    oss << "  \"performance_counters\": " << perf_counters.to_json() << "\n";
    oss << "}";
    return oss.str();
}

BufferPoolPerformanceCounters EnhancedBufferPool::get_performance_counters() const {
    return perf_counters_;
}

void EnhancedBufferPool::reset_statistics() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    perf_counters_.reset();
    stats_ = EnhancedStatistics();
}

bool EnhancedBufferPool::resize(size_t new_size) {
    // 简化实现
    return false;
}

bool EnhancedBufferPool::shrink_to_fit() {
    return false;
}

bool EnhancedBufferPool::expand(size_t additional_pages) {
    return false;
}

bool EnhancedBufferPool::update_config(const EnhancedBufferPoolConfig& new_config) {
    // 简化实现
    return false;
}

EnhancedBufferPool::DiagnosticInfo EnhancedBufferPool::get_diagnostic_info() const {
    DiagnosticInfo info;
    // 简化实现
    return info;
}

bool EnhancedBufferPool::checkpoint(const std::string& path) {
    return false;
}

bool EnhancedBufferPool::restore_from_checkpoint(const std::string& path) {
    return false;
}

std::string EnhancedBufferPool::export_state() const {
    return "";
}

bool EnhancedBufferPool::import_state(const std::string& state) {
    return false;
}

EnhancedBufferPool::HealthStatus EnhancedBufferPool::health_check() const {
    HealthStatus status;
    status.healthy = true;
    status.status = "OK";
    
    // 检查基本状态
    if (!running_.load()) {
        status.healthy = false;
        status.errors.push_back("BufferPool is not running");
    }
    
    // 检查内存使用
    std::shared_lock page_table_lock(page_table_mutex_);
    size_t used_frames = page_table_.size();
    double utilization = static_cast<double>(used_frames) / config_.pool_size;
    
    if (utilization > 0.95) {
        status.warnings.push_back("Buffer pool utilization is very high: " + 
                                 std::to_string(utilization * 100) + "%");
    }
    
    // 检查脏页数量
    size_t dirty_count = 0;
    for (const auto& pair : page_table_) {
        size_t frame_id = pair.second;
        const EnhancedBufferFrame* frame = const_cast<EnhancedBufferPool*>(this)->get_frame(frame_id);
        if (frame && frame->is_dirty()) {
            dirty_count++;
        }
    }
    
    if (dirty_count > config_.dirty_page_threshold) {
        status.warnings.push_back("Too many dirty pages: " + std::to_string(dirty_count));
    }
    
    // 检查命中率
    double hit_ratio = perf_counters_.hit_ratio();
    if (hit_ratio < 0.5 && perf_counters_.fetch_page_calls > 100) {
        status.warnings.push_back("Low hit ratio: " + std::to_string(hit_ratio * 100) + "%");
    }
    
    // 检查IO延迟
    double avg_read_latency = perf_counters_.avg_read_latency_ns() / 1e6; // 转换为毫秒
    if (avg_read_latency > 10.0 && perf_counters_.fetch_page_calls > 100) {
        status.warnings.push_back("High read latency: " + std::to_string(avg_read_latency) + "ms");
    }
    
    status.details["used_frames"] = std::to_string(used_frames);
    status.details["total_frames"] = std::to_string(config_.pool_size);
    status.details["dirty_frames"] = std::to_string(dirty_count);
    status.details["hit_ratio"] = std::to_string(hit_ratio * 100) + "%";
    status.details["utilization"] = std::to_string(utilization * 100) + "%";
    
    if (!status.errors.empty()) {
        status.healthy = false;
        status.status = "ERROR";
    } else if (!status.warnings.empty()) {
        status.status = "WARNING";
    }
    
    return status;
}

EnhancedBufferPool::TuningAdvice EnhancedBufferPool::get_tuning_advice() const {
    TuningAdvice advice;
    
    // 分析当前状态
    auto stats = get_statistics();
    double hit_ratio = stats.perf_counters.hit_ratio();
    double utilization = static_cast<double>(stats.used_frames) / stats.total_frames;
    size_t dirty_count = stats.dirty_frames;
    
    // 建议1: 命中率低
    if (hit_ratio < 0.7 && stats.perf_counters.fetch_page_calls > 1000) {
        TuningAdvice::Suggestion sugg;
        sugg.description = "Low buffer pool hit ratio: " + std::to_string(hit_ratio * 100) + "%";
        sugg.action = "Consider increasing buffer pool size or using a better replacement policy";
        sugg.expected_improvement = (0.9 - hit_ratio) * 100; // 假设可以提升到90%
        sugg.priority = 8;
        advice.suggestions.push_back(sugg);
    }
    
    // 建议2: 利用率高
    if (utilization > 0.9) {
        TuningAdvice::Suggestion sugg;
        sugg.description = "High buffer pool utilization: " + std::to_string(utilization * 100) + "%";
        sugg.action = "Consider increasing pool_size in configuration";
        sugg.expected_improvement = 10.0; // 假设可以提升10%性能
        sugg.priority = 7;
        advice.suggestions.push_back(sugg);
    }
    
    // 建议3: 脏页过多
    if (dirty_count > config_.dirty_page_threshold) {
        TuningAdvice::Suggestion sugg;
        sugg.description = "Too many dirty pages: " + std::to_string(dirty_count);
        sugg.action = "Consider reducing flush_interval or using WRITE_THROUGH strategy";
        sugg.expected_improvement = 5.0;
        sugg.priority = 6;
        advice.suggestions.push_back(sugg);
    }
    
    // 建议4: IO延迟高
    double avg_read_latency_ms = stats.perf_counters.avg_read_latency_ns() / 1e6;
    if (avg_read_latency_ms > 5.0 && stats.perf_counters.fetch_page_calls > 100) {
        TuningAdvice::Suggestion sugg;
        sugg.description = "High read latency: " + std::to_string(avg_read_latency_ms) + "ms";
        sugg.action = "Consider enabling prefetch or using faster storage";
        sugg.expected_improvement = 15.0;
        sugg.priority = 7;
        advice.suggestions.push_back(sugg);
    }
    
    // 建议5: 替换策略
    if (hit_ratio < 0.6 && config_.replacer_type == EnhancedBufferPoolConfig::ReplacerType::LRU) {
        TuningAdvice::Suggestion sugg;
        sugg.description = "LRU replacement policy may not be optimal for current access pattern";
        sugg.action = "Consider using ARC or Adaptive replacement policy";
        sugg.expected_improvement = 10.0;
        sugg.priority = 5;
        advice.suggestions.push_back(sugg);
    }
    
    // 按优先级排序
    std::sort(advice.suggestions.begin(), advice.suggestions.end(),
              [](const auto& a, const auto& b) { return a.priority > b.priority; });
    
    // 生成摘要
    if (advice.suggestions.empty()) {
        advice.summary = "Buffer pool is well-tuned. No major optimizations needed.";
    } else {
        std::ostringstream oss;
        oss << "Found " << advice.suggestions.size() << " optimization suggestions. ";
        oss << "Top priority: " << advice.suggestions[0].description;
        advice.summary = oss.str();
    }
    
    return advice;
}

// 辅助函数
namespace {
    uint64_t get_current_time_ns_helper() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
}

uint64_t EnhancedBufferPool::get_current_time_ns() {
    return get_current_time_ns_helper();
}

// ==================== NumaAwareAllocator 实现 ====================

EnhancedBufferPool::NumaAwareAllocator::NumaAwareAllocator(bool enable_huge_pages, int numa_node, size_t alignment)
    : enable_huge_pages_(enable_huge_pages), numa_node_(numa_node), alignment_(alignment) {
}

EnhancedBufferPool::NumaAwareAllocator::~NumaAwareAllocator() = default;

void* EnhancedBufferPool::NumaAwareAllocator::allocate(size_t size) {
    // 简化实现：使用标准分配
    (void)enable_huge_pages_;
    (void)numa_node_;
    (void)alignment_;
    return std::malloc(size);
}

void EnhancedBufferPool::NumaAwareAllocator::deallocate(void* ptr, size_t size) {
    (void)size;
    std::free(ptr);
}

} // namespace core
} // namespace mementodb

