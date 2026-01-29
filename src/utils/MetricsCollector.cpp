// File: src/utils/MetricsCollector.cpp

#include "MetricsCollector.h"
#include "../net/Server.hpp"
#include "../net/Connection.hpp"
#include "../net/Protocol.hpp"
#include "../net/ThreadPool.hpp"
#include "../net/IOLoop.hpp"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <random>

namespace mementodb {
namespace utils {

// ==================== IStatisticsProvider 实现 ====================

IStatisticsProvider::StatisticsWithLabels IStatisticsProvider::get_statistics_with_labels() const {
    StatisticsWithLabels result;
    result.values = get_statistics();
    result.collection_time = std::chrono::system_clock::now();
    return result;
}

// ==================== MetricsCollectorConfig 实现 ====================

bool MetricsCollectorConfig::validate() const {
    if (collection_interval.count() <= 0) {
        return false;
    }
    if (max_batch_size == 0) {
        return false;
    }
    return true;
}

MetricsCollectorConfig MetricsCollectorConfig::default_config() {
    MetricsCollectorConfig config;
    config.collection_interval = std::chrono::seconds(10);
    config.max_batch_size = 100;
    config.enable_async_collection = true;
    config.enable_incremental_metrics = true;
    config.enable_auto_registration = false;
    config.stats_retention = std::chrono::seconds(3600);
    config.enable_health_metrics = true;
    config.enable_performance_metrics = true;
    config.enable_detailed_metrics = false;
    config.collector_name = "default";
    return config;
}

// ==================== MetricDefinition 实现 ====================

bool MetricDefinition::validate() const {
    if (name.empty()) {
        return false;
    }
    // 注意：MetricDefinition 没有 config 成员，这些检查暂时跳过
    // 如果需要，应该在 MetricDefinition 中添加 config 成员
    (void)type; // 避免未使用变量警告
    return true;
}

MetricDefinition MetricDefinition::counter(const std::string& name,
                                           const std::string& description,
                                           const std::string& unit,
                                           bool incremental) {
    MetricDefinition def;
    def.name = name;
    def.description = description;
    def.type = MetricType::COUNTER;
    def.unit = unit;
    def.is_incremental = incremental;
    return def;
}

MetricDefinition MetricDefinition::gauge(const std::string& name,
                                         const std::string& description,
                                         const std::string& unit) {
    MetricDefinition def;
    def.name = name;
    def.description = description;
    def.type = MetricType::GAUGE;
    def.unit = unit;
    return def;
}

MetricDefinition MetricDefinition::histogram(const std::string& name,
                                            const std::string& description,
                                            const std::string& unit) {
    MetricDefinition def;
    def.name = name;
    def.description = description;
    def.type = MetricType::HISTOGRAM;
    def.unit = unit;
    return def;
}

MetricDefinition MetricDefinition::summary(const std::string& name,
                                          const std::string& description,
                                          const std::string& unit) {
    MetricDefinition def;
    def.name = name;
    def.description = description;
    def.type = MetricType::SUMMARY;
    def.unit = unit;
    return def;
}

MetricDefinition MetricDefinition::rate(const std::string& name,
                                        const std::string& description,
                                        const std::string& unit) {
    MetricDefinition def;
    def.name = name;
    def.description = description;
    def.type = MetricType::RATE;
    def.unit = unit;
    def.is_derived = true;
    return def;
}

// ==================== ComponentAdapter 实现 ====================

// Server统计获取
template<>
std::unordered_map<std::string, std::variant<int64_t, uint64_t, double, std::string>>
ComponentAdapter<net::Server>::get_server_statistics() const {
    std::unordered_map<std::string, std::variant<int64_t, uint64_t, double, std::string>> result;
    
    if (!component_) {
        return result;
    }
    
    try {
        auto stats = component_->get_statistics();
        
        result["total_connections"] = static_cast<uint64_t>(stats.total_connections);
        result["active_connections"] = static_cast<uint64_t>(stats.active_connections);
        result["peak_connections"] = static_cast<uint64_t>(stats.peak_connections);
        result["rejected_connections"] = static_cast<uint64_t>(stats.rejected_connections);
        result["total_requests"] = static_cast<uint64_t>(stats.total_requests);
        result["requests_per_second"] = static_cast<uint64_t>(stats.requests_per_second);
        result["peak_requests_per_second"] = static_cast<uint64_t>(stats.peak_requests_per_second);
        result["failed_requests"] = static_cast<uint64_t>(stats.failed_requests);
        result["bytes_received"] = static_cast<uint64_t>(stats.bytes_received);
        result["bytes_sent"] = static_cast<uint64_t>(stats.bytes_sent);
        result["network_errors"] = static_cast<uint64_t>(stats.network_errors);
        result["slow_commands"] = static_cast<uint64_t>(stats.slow_commands);
        result["protocol_errors"] = static_cast<uint64_t>(stats.protocol_errors);
        result["uptime"] = static_cast<uint64_t>(stats.uptime);
        result["cpu_usage"] = stats.cpu_usage;
        result["memory_usage"] = static_cast<uint64_t>(stats.memory_usage);
        result["open_files"] = static_cast<uint64_t>(stats.open_files);
    } catch (...) {
        // 忽略错误
    }
    
    return result;
}

// Connection统计获取
template<>
std::unordered_map<std::string, std::variant<int64_t, uint64_t, double, std::string>>
ComponentAdapter<net::Connection>::get_connection_statistics() const {
    std::unordered_map<std::string, std::variant<int64_t, uint64_t, double, std::string>> result;
    
    if (!component_) {
        return result;
    }
    
    try {
        auto stats = component_->get_statistics();
        
        result["bytes_received"] = static_cast<uint64_t>(stats.bytes_received);
        result["bytes_sent"] = static_cast<uint64_t>(stats.bytes_sent);
        result["commands_processed"] = static_cast<uint64_t>(stats.commands_processed);
        result["errors"] = static_cast<uint64_t>(stats.errors);
    } catch (...) {
        // 忽略错误
    }
    
    return result;
}

// Protocol统计获取
template<>
std::unordered_map<std::string, std::variant<int64_t, uint64_t, double, std::string>>
ComponentAdapter<net::Protocol>::get_protocol_statistics() const {
    std::unordered_map<std::string, std::variant<int64_t, uint64_t, double, std::string>> result;
    
    if (!component_) {
        return result;
    }
    
    try {
        auto stats = component_->get_statistics();
        
        result["requests_parsed"] = static_cast<uint64_t>(stats.requests_parsed);
        result["bytes_parsed"] = static_cast<uint64_t>(stats.bytes_parsed);
        result["commands_processed"] = static_cast<uint64_t>(stats.commands_processed);
        result["pipeline_requests"] = static_cast<uint64_t>(stats.pipeline_requests);
        result["parse_errors"] = static_cast<uint64_t>(stats.parse_errors);
        result["protocol_errors"] = static_cast<uint64_t>(stats.protocol_errors);
    } catch (...) {
        // 忽略错误
    }
    
    return result;
}

// ThreadPool统计获取
template<>
std::unordered_map<std::string, std::variant<int64_t, uint64_t, double, std::string>>
ComponentAdapter<net::ThreadPool>::get_thread_pool_statistics() const {
    std::unordered_map<std::string, std::variant<int64_t, uint64_t, double, std::string>> result;
    
    if (!component_) {
        return result;
    }
    
    try {
        auto stats = component_->get_statistics();
        
        result["total_threads"] = static_cast<uint64_t>(stats.total_threads);
        result["active_threads"] = static_cast<uint64_t>(stats.active_threads);
        result["idle_threads"] = static_cast<uint64_t>(stats.idle_threads);
        result["max_threads"] = static_cast<uint64_t>(stats.max_threads);
        result["total_tasks_submitted"] = static_cast<uint64_t>(stats.total_tasks_submitted);
        result["total_tasks_completed"] = static_cast<uint64_t>(stats.total_tasks_completed);
        result["total_tasks_failed"] = static_cast<uint64_t>(stats.total_tasks_failed);
        result["pending_tasks"] = static_cast<uint64_t>(stats.pending_tasks);
        result["queue_capacity"] = static_cast<uint64_t>(stats.queue_capacity);
        result["queue_usage"] = static_cast<uint64_t>(stats.queue_usage);
    } catch (...) {
        // 忽略错误
    }
    
    return result;
}

// IOLoop统计获取
template<>
std::unordered_map<std::string, std::variant<int64_t, uint64_t, double, std::string>>
ComponentAdapter<net::IOLoop>::get_io_loop_statistics() const {
    std::unordered_map<std::string, std::variant<int64_t, uint64_t, double, std::string>> result;
    
    if (!component_) {
        return result;
    }
    
    try {
        auto stats = component_->get_statistics();
        
        result["fd_count"] = static_cast<uint64_t>(stats.fd_count);
        result["timer_count"] = static_cast<uint64_t>(stats.timer_count);
        result["signal_count"] = static_cast<uint64_t>(stats.signal_count);
        result["processed_events"] = static_cast<uint64_t>(stats.processed_events);
        result["pending_tasks"] = static_cast<uint64_t>(stats.pending_tasks);
        result["cycles"] = static_cast<uint64_t>(stats.cycles);
        result["total_cycle_time_us"] = static_cast<uint64_t>(stats.total_cycle_time.count());
        result["max_cycle_time_us"] = static_cast<uint64_t>(stats.max_cycle_time.count());
    } catch (...) {
        // 忽略错误
    }
    
    return result;
}

// 通用统计获取
template<typename T>
std::unordered_map<std::string, std::variant<int64_t, uint64_t, double, std::string>>
ComponentAdapter<T>::get_generic_statistics() const {
    std::unordered_map<std::string, std::variant<int64_t, uint64_t, double, std::string>> result;
    
    // 尝试调用get_statistics方法（如果存在）
    // 这里简化处理，实际可能需要更复杂的反射机制
    return result;
}

// ==================== CollectionEvent 实现 ====================

std::string CollectionEvent::to_string() const {
    std::ostringstream oss;
    oss << "CollectionEvent{type=" << static_cast<int>(type)
        << ", collector=" << collector_name
        << ", component=" << component_name
        << ", metric=" << metric_name
        << ", metrics_collected=" << metrics_collected
        << ", duration_ms=" << duration.count() << "}";
    return oss.str();
}

// ==================== MetricsCollector 实现 ====================

std::shared_ptr<MetricsCollector> MetricsCollector::create(
    std::shared_ptr<MetricRegistry> registry,
    const MetricsCollectorConfig& config) {
    
    if (!registry) {
        registry = MetricsManager::global_registry();
    }
    
    auto collector = std::shared_ptr<MetricsCollector>(
        new MetricsCollector(registry, config));
    
    if (!collector->init()) {
        return nullptr;
    }
    
    return collector;
}

MetricsCollector::Builder::Builder() {
    config_ = MetricsCollectorConfig::default_config();
}

MetricsCollector::Builder& MetricsCollector::Builder::with_registry(std::shared_ptr<MetricRegistry> registry) {
    registry_ = registry;
    return *this;
}

MetricsCollector::Builder& MetricsCollector::Builder::with_config(const MetricsCollectorConfig& config) {
    config_ = config;
    return *this;
}

MetricsCollector::Builder& MetricsCollector::Builder::with_name(const std::string& name) {
    config_.collector_name = name;
    return *this;
}

MetricsCollector::Builder& MetricsCollector::Builder::with_default_labels(
    const std::unordered_map<std::string, std::string>& labels) {
    config_.default_labels = labels;
    return *this;
}

MetricsCollector::Builder& MetricsCollector::Builder::on_event(EventCallback callback) {
    event_callback_ = callback;
    return *this;
}

MetricsCollector::Builder& MetricsCollector::Builder::on_error(ErrorCallback callback) {
    error_callback_ = callback;
    return *this;
}

MetricsCollector::Builder& MetricsCollector::Builder::enable_async_collection(bool enable) {
    config_.enable_async_collection = enable;
    return *this;
}

MetricsCollector::Builder& MetricsCollector::Builder::enable_health_metrics(bool enable) {
    config_.enable_health_metrics = enable;
    return *this;
}

std::shared_ptr<MetricsCollector> MetricsCollector::Builder::build() {
    if (!registry_) {
        registry_ = MetricsManager::global_registry();
    }
    
    auto collector = std::shared_ptr<MetricsCollector>(
        new MetricsCollector(registry_, config_));
    
    if (event_callback_) {
        collector->set_event_callback(event_callback_);
    }
    
    if (error_callback_) {
        collector->set_error_callback(error_callback_);
    }
    
    if (!collector->init()) {
        return nullptr;
    }
    
    return collector;
}

MetricsCollector::MetricsCollector(std::shared_ptr<MetricRegistry> registry,
                                   const MetricsCollectorConfig& config)
    : registry_(registry), config_(config) {
}

MetricsCollector::~MetricsCollector() {
    stop(false);
    // cleanup() 有歧义，直接调用带参数的版本
    cleanup(std::chrono::system_clock::now());
}

bool MetricsCollector::init() {
    if (!registry_) {
        return false;
    }
    
    if (!config_.validate()) {
        return false;
    }
    
    stats_.start_time = std::chrono::system_clock::now();
    return true;
}

void MetricsCollector::cleanup() {
    stop(false);
    
    std::unique_lock components_lock(components_mutex_);
    components_.clear();
    
    std::unique_lock metrics_lock(metrics_mutex_);
    metrics_.clear();
}

std::string MetricsCollector::register_component(ComponentPtr component) {
    if (!component) {
        return "";
    }
    
    std::string component_id = generate_component_id();
    
    std::unique_lock lock(components_mutex_);
    ComponentEntry entry;
    entry.id = component_id;
    entry.component = component;
    entry.registered_time = std::chrono::system_clock::now();
    entry.enabled = true;
    
    components_[component_id] = entry;
    
    CollectionEvent event;
    event.type = CollectionEvent::Type::COMPONENT_ADDED;
    event.collector_name = config_.collector_name;
    event.component_name = component->component_name();
    event.timestamp = std::chrono::system_clock::now();
    notify_event(event);
    
    return component_id;
}

bool MetricsCollector::unregister_component(const std::string& component_id) {
    std::unique_lock lock(components_mutex_);
    auto it = components_.find(component_id);
    if (it == components_.end()) {
        return false;
    }
    
    CollectionEvent event;
    event.type = CollectionEvent::Type::COMPONENT_REMOVED;
    event.collector_name = config_.collector_name;
    event.component_name = it->second.component->component_name();
    event.timestamp = std::chrono::system_clock::now();
    
    components_.erase(it);
    
    notify_event(event);
    return true;
}

bool MetricsCollector::register_metric(const MetricDefinition& definition) {
    if (!definition.validate()) {
        return false;
    }
    
    std::string metric_id = generate_metric_id();
    
    if (!create_metric_from_definition(definition)) {
        return false;
    }
    
    std::unique_lock lock(metrics_mutex_);
    MetricEntry entry;
    entry.definition = definition;
    entry.created_time = std::chrono::system_clock::now();
    
    // 获取刚创建的指标
    auto metric = registry_->get_metric(definition.name);
    if (!metric) {
        return false;
    }
    
    entry.metric = metric;
    metrics_[metric_id] = entry;
    
    return true;
}

bool MetricsCollector::register_metric_mapping(const std::string& component_type,
                                               const std::string& source_field,
                                               const MetricDefinition& metric_def) {
    if (!register_metric(metric_def)) {
        return false;
    }
    
    std::unique_lock lock(metrics_mutex_);
    field_to_metric_.insert({source_field, metric_def.name});
    
    return true;
}

bool MetricsCollector::start() {
    CollectorState expected = CollectorState::STOPPED;
    if (!state_.compare_exchange_strong(expected, CollectorState::STARTING)) {
        return false;
    }
    
    if (config_.enable_async_collection) {
        collection_running_.store(true);
        collection_thread_ = std::thread(&MetricsCollector::collection_loop, this);
    }
    
    state_.store(CollectorState::RUNNING);
    
    CollectionEvent event;
    event.type = CollectionEvent::Type::STARTED;
    event.collector_name = config_.collector_name;
    event.timestamp = std::chrono::system_clock::now();
    notify_event(event);
    
    return true;
}

void MetricsCollector::stop(bool graceful) {
    CollectorState expected = CollectorState::RUNNING;
    if (!state_.compare_exchange_strong(expected, CollectorState::STOPPING)) {
        expected = CollectorState::PAUSED;
        if (!state_.compare_exchange_strong(expected, CollectorState::STOPPING)) {
            return;
        }
    }
    
    collection_running_.store(false);
    collection_cv_.notify_all();
    
    if (collection_thread_.joinable()) {
        if (graceful) {
            collection_thread_.join();
        } else {
            collection_thread_.detach();
        }
    }
    
    state_.store(CollectorState::STOPPED);
}

void MetricsCollector::pause() {
    paused_.store(true);
    state_.store(CollectorState::PAUSED);
}

void MetricsCollector::resume() {
    paused_.store(false);
    if (state_.load() == CollectorState::PAUSED) {
        state_.store(CollectorState::RUNNING);
    }
}

bool MetricsCollector::collect() {
    if (state_.load() == CollectorState::STOPPED) {
        return false;
    }
    
    CollectionEvent start_event;
    start_event.type = CollectionEvent::Type::STARTED;
    start_event.collector_name = config_.collector_name;
    start_event.timestamp = std::chrono::system_clock::now();
    
    auto start_time = std::chrono::steady_clock::now();
    size_t metrics_collected = 0;
    bool success = true;
    
    std::shared_lock lock(components_mutex_);
    std::vector<std::string> component_ids;
    for (const auto& pair : components_) {
        component_ids.push_back(pair.first);
    }
    lock.unlock();
    
    for (const auto& component_id : component_ids) {
        if (collect_component(component_id)) {
            metrics_collected++;
        } else {
            success = false;
        }
    }
    
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    update_statistics(duration, success);
    
    CollectionEvent end_event;
    end_event.type = success ? CollectionEvent::Type::COMPLETED : CollectionEvent::Type::FAILED;
    end_event.collector_name = config_.collector_name;
    end_event.metrics_collected = metrics_collected;
    end_event.duration = duration;
    end_event.timestamp = std::chrono::system_clock::now();
    notify_event(end_event);
    
    return success;
}

bool MetricsCollector::collect_batch(const std::vector<std::string>& component_ids) {
    bool success = true;
    for (const auto& component_id : component_ids) {
        if (!collect_component(component_id)) {
            success = false;
        }
    }
    return success;
}

bool MetricsCollector::collect_component(const std::string& component_id) {
    std::shared_lock lock(components_mutex_);
    auto it = components_.find(component_id);
    if (it == components_.end()) {
        return false;
    }
    
    auto entry = it->second;
    lock.unlock();
    
    return collect_component_entry(entry);
}

bool MetricsCollector::collect_component_entry(ComponentEntry& entry) {
    if (!entry.component || !entry.enabled) {
        return false;
    }
    
    try {
        auto stats = entry.component->get_statistics();
        auto stats_with_labels = entry.component->get_statistics_with_labels();
        
        // 合并标签
        auto merged_labels = merge_labels(
            stats_with_labels.labels,
            {},
            config_.default_labels);
        
        // 更新指标
        for (const auto& stat_pair : stats) {
            std::string field_name = stat_pair.first;
            double value = 0.0;
            
            // 转换值
            std::visit([&value](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_integral_v<T>) {
                    value = static_cast<double>(v);
                } else if constexpr (std::is_floating_point_v<T>) {
                    value = static_cast<double>(v);
                }
            }, stat_pair.second);
            
            // 处理增量指标
            if (config_.enable_incremental_metrics) {
                value = calculate_incremental_value(entry.id, field_name, value);
            }
            
            // 查找对应的指标
            std::shared_lock metrics_lock(metrics_mutex_);
            auto range = field_to_metric_.equal_range(field_name);
            for (auto it = range.first; it != range.second; ++it) {
                std::string metric_name = it->second;
                update_metric(metric_name, value, merged_labels);
            }
        }
        
        entry.last_collection = std::chrono::system_clock::now();
        entry.collection_count++;
        
        return true;
    } catch (const std::exception& e) {
        notify_error(std::make_error_code(std::errc::io_error), 
                    "Failed to collect from component: " + std::string(e.what()));
        return false;
    }
}

void MetricsCollector::collection_loop() {
    while (collection_running_.load()) {
        if (!paused_.load()) {
            collect();
        }
        
        std::unique_lock<std::mutex> lock(collection_mutex_);
        collection_cv_.wait_for(lock, config_.collection_interval, 
                               [this] { return !collection_running_.load(); });
    }
}

double MetricsCollector::calculate_incremental_value(const std::string& component_id,
                                                     const std::string& field_name,
                                                     double current_value) {
    std::unique_lock lock(components_mutex_);
    auto it = components_.find(component_id);
    if (it == components_.end()) {
        return current_value;
    }
    
    auto& entry = it->second;
    auto last_it = entry.last_values.find(field_name);
    
    if (last_it == entry.last_values.end()) {
        entry.last_values[field_name] = current_value;
        return current_value;
    }
    
    double last_value = last_it->second;
    double increment = current_value - last_value;
    entry.last_values[field_name] = current_value;
    
    return increment >= 0 ? increment : current_value; // 防止负值
}

double MetricsCollector::calculate_derived_value(const MetricDefinition& definition,
                                                 double current_value,
                                                 double last_value) {
    if (!definition.is_derived || !definition.derive_function) {
        return current_value;
    }
    
    return definition.derive_function(current_value, last_value);
}

bool MetricsCollector::create_metric_from_definition(const MetricDefinition& definition) {
    MetricConfig config;
    config.type = definition.type;
    config.unit = definition.unit;
    
    switch (definition.type) {
        case MetricType::COUNTER:
            return registry_->register_metric(definition.name, definition.description, 
                                            MetricType::COUNTER, 
                                            MetricConfig::counter_config(definition.unit)) != nullptr;
        case MetricType::GAUGE:
            return registry_->register_metric(definition.name, definition.description,
                                            MetricType::GAUGE,
                                            MetricConfig::gauge_config(definition.unit)) != nullptr;
        case MetricType::HISTOGRAM:
            return registry_->register_metric(definition.name, definition.description,
                                            MetricType::HISTOGRAM,
                                            MetricConfig::histogram_config_static()) != nullptr;
        case MetricType::SUMMARY:
            return registry_->register_metric(definition.name, definition.description,
                                            MetricType::SUMMARY,
                                            MetricConfig::summary_config_static()) != nullptr;
        default:
            return false;
    }
}

bool MetricsCollector::update_metric(const std::string& metric_id,
                                    double value,
                                    const std::unordered_map<std::string, std::string>& labels) {
    auto metric = registry_->get_metric(metric_id);
    if (!metric) {
        return false;
    }
    
    // 将 unordered_map 转换为 map
    std::map<std::string, std::string> label_map(labels.begin(), labels.end());
    LabelSet label_set(label_map);
    
    switch (metric->type()) {
        case MetricType::COUNTER: {
            auto counter = std::static_pointer_cast<CounterMetric>(metric);
            counter->inc(value, label_set);
            break;
        }
        case MetricType::GAUGE: {
            auto gauge = std::static_pointer_cast<GaugeMetric>(metric);
            gauge->set(value, label_set);
            break;
        }
        case MetricType::HISTOGRAM: {
            auto histogram = std::static_pointer_cast<HistogramMetric>(metric);
            histogram->observe(value, label_set);
            break;
        }
        case MetricType::SUMMARY: {
            auto summary = std::static_pointer_cast<SummaryMetric>(metric);
            summary->observe(value, label_set);
            break;
        }
        default:
            return false;
    }
    
    return true;
}

std::unordered_map<std::string, std::string> MetricsCollector::merge_labels(
    const std::unordered_map<std::string, std::string>& component_labels,
    const std::unordered_map<std::string, std::string>& metric_labels,
    const std::unordered_map<std::string, std::string>& default_labels) const {
    
    std::unordered_map<std::string, std::string> result = default_labels;
    
    for (const auto& pair : component_labels) {
        result[pair.first] = pair.second;
    }
    
    for (const auto& pair : metric_labels) {
        result[pair.first] = pair.second;
    }
    
    return result;
}

std::string MetricsCollector::generate_component_id(const std::string& prefix) {
    static std::atomic<uint64_t> counter{0};
    return prefix + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

std::string MetricsCollector::generate_metric_id(const std::string& prefix) {
    static std::atomic<uint64_t> counter{0};
    return prefix + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

void MetricsCollector::update_statistics(std::chrono::milliseconds duration, bool success) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    stats_.total_collections++;
    if (!success) {
        stats_.failed_collections++;
    }
    
    stats_.total_collection_time += duration;
    stats_.avg_collection_time = std::chrono::milliseconds(
        stats_.total_collection_time.count() / stats_.total_collections);
    
    if (duration > stats_.max_collection_time) {
        stats_.max_collection_time = duration;
    }
    
    stats_.last_collection = std::chrono::system_clock::now();
}

void MetricsCollector::notify_event(const CollectionEvent& event) {
    if (event_callback_) {
        safe_invoke_callback([this, event]() {
            event_callback_(event);
        });
    }
}

void MetricsCollector::notify_error(const std::error_code& ec, const std::string& message) {
    if (error_callback_) {
        safe_invoke_callback([this, ec, message]() {
            error_callback_(ec, message);
        });
    }
}

void MetricsCollector::safe_invoke_callback(const std::function<void()>& callback) {
    try {
        callback();
    } catch (...) {
        // 忽略回调中的异常
    }
}

std::vector<std::string> MetricsCollector::get_registered_components() const {
    std::shared_lock lock(components_mutex_);
    std::vector<std::string> result;
    result.reserve(components_.size());
    for (const auto& pair : components_) {
        result.push_back(pair.first);
    }
    return result;
}

MetricsCollector::ComponentInfo MetricsCollector::get_component_info(const std::string& component_id) const {
    std::shared_lock lock(components_mutex_);
    ComponentInfo info;
    
    auto it = components_.find(component_id);
    if (it == components_.end()) {
        return info;
    }
    
    const auto& entry = it->second;
    info.id = entry.id;
    info.type = entry.component->component_type();
    info.instance_id = entry.component->instance_id();
    info.registered_time = entry.registered_time;
    info.last_collection = entry.last_collection;
    info.collection_count = entry.collection_count;
    
    // 获取标签
    if (auto adapter = std::dynamic_pointer_cast<ComponentAdapter<net::Server>>(entry.component)) {
        info.labels = adapter->labels();
    } else if (auto adapter = std::dynamic_pointer_cast<ComponentAdapter<net::Connection>>(entry.component)) {
        info.labels = adapter->labels();
    } else if (auto adapter = std::dynamic_pointer_cast<ComponentAdapter<net::Protocol>>(entry.component)) {
        info.labels = adapter->labels();
    } else if (auto adapter = std::dynamic_pointer_cast<ComponentAdapter<net::ThreadPool>>(entry.component)) {
        info.labels = adapter->labels();
    }
    
    return info;
}

MetricsCollector::CollectorStatistics MetricsCollector::get_statistics() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    CollectorStatistics result = stats_;
    
    std::shared_lock components_lock(components_mutex_);
    result.total_components = components_.size();
    result.active_components = 0;
    for (const auto& pair : components_) {
        if (pair.second.enabled) {
            result.active_components++;
        }
    }
    
    std::shared_lock metrics_lock(metrics_mutex_);
    result.total_metrics = metrics_.size();
    result.state = state_.load();
    
    return result;
}

void MetricsCollector::reset_statistics() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = CollectorStatistics();
    stats_.start_time = std::chrono::system_clock::now();
}

void MetricsCollector::set_event_callback(EventCallback callback) {
    event_callback_ = callback;
}

void MetricsCollector::set_error_callback(ErrorCallback callback) {
    error_callback_ = callback;
}

bool MetricsCollector::update_config(const MetricsCollectorConfig& new_config) {
    if (!new_config.validate()) {
        return false;
    }
    
    config_ = new_config;
    return true;
}

std::string MetricsCollector::export_metrics(const std::string& format) const {
    if (!registry_) {
        return "";
    }
    
    if (format == "prometheus") {
        return registry_->to_prometheus();
    } else if (format == "json") {
        return registry_->to_json();
    } else if (format == "influxdb") {
        return registry_->to_influxdb();
    }
    
    return "";
}

size_t MetricsCollector::cleanup(std::chrono::system_clock::time_point cutoff) {
    if (!registry_) {
        return 0;
    }
    
    return registry_->cleanup(cutoff);
}

MetricsCollector::HealthStatus MetricsCollector::health_check() const {
    HealthStatus status;
    status.healthy = true;
    status.status = "healthy";
    
    if (state_.load() == CollectorState::ERROR) {
        status.healthy = false;
        status.status = "error";
        status.details["state"] = "error";
    }
    
    if (!registry_) {
        status.healthy = false;
        status.status = "unhealthy";
        status.details["registry"] = "missing";
    }
    
    std::shared_lock lock(components_mutex_);
    if (components_.empty()) {
        status.details["components"] = "none";
    } else {
        status.details["components"] = std::to_string(components_.size());
    }
    
    return status;
}

std::string MetricsCollector::register_custom_provider(
    const std::string& component_type,
    std::function<std::unordered_map<std::string, std::variant<int64_t, uint64_t, double, std::string>>()> provider,
    const std::unordered_map<std::string, std::string>& labels,
    const std::string& instance_id) {
    
    // 创建自定义提供器适配器
    class CustomProvider : public IStatisticsProvider {
    public:
        CustomProvider(const std::string& type,
                      const std::string& id,
                      const std::unordered_map<std::string, std::string>& lbls,
                      std::function<std::unordered_map<std::string, std::variant<int64_t, uint64_t, double, std::string>>()> prov)
            : component_type_(type), instance_id_(id), labels_(lbls), provider_(prov) {}
        
        std::string component_name() const override {
            return component_type_ + "_" + instance_id_;
        }
        
        std::string instance_id() const override {
            return instance_id_;
        }
        
        std::string component_type() const override {
            return component_type_;
        }
        
        std::unordered_map<std::string, std::variant<int64_t, uint64_t, double, std::string>> 
        get_statistics() const override {
            return provider_();
        }
        
    private:
        std::string component_type_;
        std::string instance_id_;
        std::unordered_map<std::string, std::string> labels_;
        std::function<std::unordered_map<std::string, std::variant<int64_t, uint64_t, double, std::string>>()> provider_;
    };
    
    std::string id = instance_id.empty() ? generate_component_id() : instance_id;
    auto custom_provider = std::make_shared<CustomProvider>(component_type, id, labels, provider);
    
    return register_component(custom_provider);
}

// ==================== metrics_adapter 实现 ====================

namespace metrics_adapter {

std::shared_ptr<IStatisticsProvider> create_server_adapter(
    std::shared_ptr<net::Server> server,
    const std::string& instance_id,
    const std::unordered_map<std::string, std::string>& labels) {
    return std::make_shared<ComponentAdapter<net::Server>>(server, "server", instance_id, labels);
}

std::shared_ptr<IStatisticsProvider> create_connection_adapter(
    std::shared_ptr<net::Connection> connection,
    const std::string& instance_id,
    const std::unordered_map<std::string, std::string>& labels) {
    return std::make_shared<ComponentAdapter<net::Connection>>(connection, "connection", instance_id, labels);
}

std::shared_ptr<IStatisticsProvider> create_protocol_adapter(
    std::shared_ptr<net::Protocol> protocol,
    const std::string& instance_id,
    const std::unordered_map<std::string, std::string>& labels) {
    return std::make_shared<ComponentAdapter<net::Protocol>>(protocol, "protocol", instance_id, labels);
}

std::shared_ptr<IStatisticsProvider> create_thread_pool_adapter(
    std::shared_ptr<net::ThreadPool> thread_pool,
    const std::string& instance_id,
    const std::unordered_map<std::string, std::string>& labels) {
    return std::make_shared<ComponentAdapter<net::ThreadPool>>(thread_pool, "thread_pool", instance_id, labels);
}

std::shared_ptr<IStatisticsProvider> create_io_loop_adapter(
    std::shared_ptr<net::IOLoop> io_loop,
    const std::string& instance_id,
    const std::unordered_map<std::string, std::string>& labels) {
    return std::make_shared<ComponentAdapter<net::IOLoop>>(io_loop, "io_loop", instance_id, labels);
}

} // namespace metrics_adapter

// ==================== metrics_definitions 实现 ====================

namespace metrics_definitions {

const std::vector<MetricDefinition> SERVER_METRICS = {
    MetricDefinition::counter("mementodb_server_connections_total", "Total server connections", "connections", true),
    MetricDefinition::gauge("mementodb_server_connections_active", "Active server connections", "connections"),
    MetricDefinition::counter("mementodb_server_requests_total", "Total server requests", "requests", true),
    MetricDefinition::counter("mementodb_server_bytes_received_total", "Total bytes received", "bytes", true),
    MetricDefinition::counter("mementodb_server_bytes_sent_total", "Total bytes sent", "bytes", true),
    MetricDefinition::histogram("mementodb_server_request_duration_seconds", "Request processing duration", "seconds")
};

const std::vector<MetricDefinition> CONNECTION_METRICS = {
    MetricDefinition::counter("mementodb_connection_bytes_received_total", "Total bytes received per connection", "bytes", true),
    MetricDefinition::counter("mementodb_connection_bytes_sent_total", "Total bytes sent per connection", "bytes", true),
    MetricDefinition::counter("mementodb_connection_commands_total", "Total commands processed per connection", "commands", true)
};

const std::vector<MetricDefinition> PROTOCOL_METRICS = {
    MetricDefinition::counter("mementodb_protocol_requests_parsed_total", "Total requests parsed", "requests", true),
    MetricDefinition::counter("mementodb_protocol_parse_errors_total", "Total parse errors", "errors", true)
};

const std::vector<MetricDefinition> THREAD_POOL_METRICS = {
    MetricDefinition::gauge("mementodb_thread_pool_threads_active", "Active threads", "threads"),
    MetricDefinition::gauge("mementodb_thread_pool_tasks_pending", "Pending tasks", "tasks"),
    MetricDefinition::counter("mementodb_thread_pool_tasks_completed_total", "Total completed tasks", "tasks", true)
};

const std::vector<MetricDefinition> IO_LOOP_METRICS = {
    MetricDefinition::counter("mementodb_io_loop_events_processed_total", "Total events processed", "events", true),
    MetricDefinition::counter("mementodb_io_loop_errors_total", "Total IO errors", "errors", true)
};

const std::vector<MetricDefinition> SYSTEM_METRICS = {
    MetricDefinition::gauge("mementodb_system_cpu_usage_percent", "CPU usage percentage", "percent"),
    MetricDefinition::gauge("mementodb_system_memory_usage_bytes", "Memory usage", "bytes"),
    MetricDefinition::gauge("mementodb_system_open_files", "Number of open file descriptors", "files")
};

void register_all_predefined_metrics(std::shared_ptr<MetricsCollector> collector) {
    for (const auto& def : SERVER_METRICS) {
        collector->register_metric(def);
    }
    for (const auto& def : CONNECTION_METRICS) {
        collector->register_metric(def);
    }
    for (const auto& def : PROTOCOL_METRICS) {
        collector->register_metric(def);
    }
    for (const auto& def : THREAD_POOL_METRICS) {
        collector->register_metric(def);
    }
    for (const auto& def : IO_LOOP_METRICS) {
        collector->register_metric(def);
    }
    for (const auto& def : SYSTEM_METRICS) {
        collector->register_metric(def);
    }
}

} // namespace metrics_definitions

} // namespace utils
} // namespace mementodb
