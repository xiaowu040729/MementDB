#ifndef METRICS_COLLECTOR_H
#define METRICS_COLLECTOR_H

#include "Metrics.h"
#include <memory>
#include <functional>
#include <unordered_map>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <shared_mutex>
#include <chrono>
#include <optional>
#include <type_traits>
#include <typeinfo>
#include <typeindex>
#include <system_error>
#include <variant>

namespace mementodb {
namespace net {
    // 前向声明，减少耦合
    class Server;
    class Connection;
    class Protocol;
    class ThreadPool;
    class IOLoop;
}

namespace utils {

// 组件统计接口（统一接口）
class IStatisticsProvider {
public:
    virtual ~IStatisticsProvider() = default;
    
    virtual std::string component_name() const = 0;
    virtual std::string instance_id() const = 0;
    
    // 获取统计信息（键值对）
    virtual std::unordered_map<std::string, std::variant<int64_t, uint64_t, double, std::string>> 
    get_statistics() const = 0;
    
    // 获取统计信息（带标签）
    struct StatisticsWithLabels {
        std::unordered_map<std::string, std::variant<int64_t, uint64_t, double, std::string>> values;
        std::unordered_map<std::string, std::string> labels;
        std::chrono::system_clock::time_point collection_time;
    };
    
    virtual StatisticsWithLabels get_statistics_with_labels() const;
    
    // 获取组件类型
    virtual std::string component_type() const = 0;
};

// 统计收集器配置
struct MetricsCollectorConfig {
    std::chrono::seconds collection_interval{10};          // 收集间隔
    size_t max_batch_size = 100;                          // 最大批处理大小
    bool enable_async_collection = true;                  // 启用异步收集
    bool enable_incremental_metrics = true;               // 启用增量指标计算
    bool enable_auto_registration = false;                // 启用自动注册
    std::chrono::seconds stats_retention{3600};           // 统计保留时间
    bool enable_health_metrics = true;                    // 启用健康指标
    bool enable_performance_metrics = true;               // 启用性能指标
    bool enable_detailed_metrics = false;                 // 启用详细指标
    std::string collector_name = "default";               // 收集器名称
    std::unordered_map<std::string, std::string> default_labels; // 默认标签
    
    // 验证配置
    bool validate() const;
    
    // 获取默认配置
    static MetricsCollectorConfig default_config();
};

// 指标定义
struct MetricDefinition {
    std::string name;
    std::string description;
    MetricType type;
    std::string unit;
    bool is_incremental = false;          // 是否为增量指标（如计数器）
    bool is_derived = false;              // 是否为派生指标（如QPS）
    std::string source_field;             // 源字段名
    std::function<double(double, double)> derive_function; // 派生函数
    std::unordered_map<std::string, std::string> default_labels;
    std::chrono::seconds retention;       // 保留时间
    
    // 验证定义
    bool validate() const;
    
    // 创建标准定义
    static MetricDefinition counter(const std::string& name, 
                                    const std::string& description,
                                    const std::string& unit = "",
                                    bool incremental = true);
    
    static MetricDefinition gauge(const std::string& name,
                                  const std::string& description,
                                  const std::string& unit = "");
    
    static MetricDefinition histogram(const std::string& name,
                                      const std::string& description,
                                      const std::string& unit = "");
    
    static MetricDefinition summary(const std::string& name,
                                    const std::string& description,
                                    const std::string& unit = "");
    
    static MetricDefinition rate(const std::string& name,
                                 const std::string& description,
                                 const std::string& unit = "per_second");
};

// 组件适配器（将各种组件适配到统一接口）
template<typename T>
class ComponentAdapter : public IStatisticsProvider {
public:
    ComponentAdapter(std::shared_ptr<T> component, 
                     const std::string& component_type,
                     const std::string& instance_id = "",
                     const std::unordered_map<std::string, std::string>& labels = {})
        : component_(std::move(component))
        , component_type_(component_type)
        , instance_id_(instance_id.empty() ? generate_instance_id() : instance_id)
        , labels_(labels) {}
    
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
        if constexpr (std::is_same_v<T, net::Server>) {
            return get_server_statistics();
        } else if constexpr (std::is_same_v<T, net::Connection>) {
            return get_connection_statistics();
        } else if constexpr (std::is_same_v<T, net::Protocol>) {
            return get_protocol_statistics();
        } else if constexpr (std::is_same_v<T, net::ThreadPool>) {
            return get_thread_pool_statistics();
        } else if constexpr (std::is_same_v<T, net::IOLoop>) {
            return get_io_loop_statistics();
        } else {
            // 通用适配：尝试调用get_statistics方法
            return get_generic_statistics();
        }
    }
    
    const std::unordered_map<std::string, std::string>& labels() const {
        return labels_;
    }
    
    std::shared_ptr<T> component() const {
        return component_;
    }
    
private:
    std::shared_ptr<T> component_;
    std::string component_type_;
    std::string instance_id_;
    std::unordered_map<std::string, std::string> labels_;
    
    // 生成实例ID
    static std::string generate_instance_id() {
        static std::atomic<uint64_t> counter{0};
        return std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
    }
    
    // 各种组件的统计获取方法
    std::unordered_map<std::string, std::variant<int64_t, uint64_t, double, std::string>> 
    get_server_statistics() const;
    
    std::unordered_map<std::string, std::variant<int64_t, uint64_t, double, std::string>> 
    get_connection_statistics() const;
    
    std::unordered_map<std::string, std::variant<int64_t, uint64_t, double, std::string>> 
    get_protocol_statistics() const;
    
    std::unordered_map<std::string, std::variant<int64_t, uint64_t, double, std::string>> 
    get_thread_pool_statistics() const;
    
    std::unordered_map<std::string, std::variant<int64_t, uint64_t, double, std::string>> 
    get_io_loop_statistics() const;
    
    std::unordered_map<std::string, std::variant<int64_t, uint64_t, double, std::string>> 
    get_generic_statistics() const;
};

// 统计收集器事件
struct CollectionEvent {
    enum class Type {
        STARTED,
        COMPLETED,
        FAILED,
        COMPONENT_ADDED,
        COMPONENT_REMOVED,
        METRIC_UPDATED
    } type;
    
    std::string collector_name;
    std::string component_name;
    std::string metric_name;
    std::error_code error;
    size_t metrics_collected = 0;
    std::chrono::milliseconds duration{0};
    std::chrono::system_clock::time_point timestamp;
    
    std::string to_string() const;
};

// 统计收集器状态
enum class CollectorState {
    STOPPED,
    STARTING,
    RUNNING,
    PAUSED,
    STOPPING,
    ERROR
};

/**
 * @brief 高级统计收集器
 * @details 统一收集各种组件的统计信息并转换为指标
 * 支持动态组件注册、自动收集、增量计算、派生指标等高级特性
 * 
 * 特性：
 * 1. 支持多种组件类型（通过适配器统一接口）
 * 2. 动态指标注册和配置
 * 3. 支持增量指标和派生指标（如QPS）
 * 4. 异步收集和批处理
 * 5. 完整的错误处理和事件通知
 * 6. 支持健康检查和性能指标
 * 
 * 使用示例：
 *   auto collector = MetricsCollector::create();
 *   
 *   // 注册组件
 *   collector->register_component<net::Server>(server, "server", {{"role", "master"}});
 *   collector->register_component<net::ThreadPool>(thread_pool, "thread_pool");
 *   
 *   // 注册自定义指标
 *   collector->register_metric(MetricDefinition::counter("custom_total", "Custom counter"));
 *   
 *   // 启动自动收集
 *   collector->start();
 *   
 *   // 手动收集
 *   collector->collect();
 */
class MetricsCollector : public std::enable_shared_from_this<MetricsCollector> {
public:
    using ComponentPtr = std::shared_ptr<IStatisticsProvider>;
    using EventCallback = std::function<void(const CollectionEvent&)>;
    using ErrorCallback = std::function<void(const std::error_code&, const std::string&)>;
    
    // 创建收集器
    static std::shared_ptr<MetricsCollector> create(
        std::shared_ptr<MetricRegistry> registry = nullptr,
        const MetricsCollectorConfig& config = MetricsCollectorConfig::default_config());
    
    // 创建收集器（构建器模式）
    class Builder {
    public:
        Builder();
        
        Builder& with_registry(std::shared_ptr<MetricRegistry> registry);
        Builder& with_config(const MetricsCollectorConfig& config);
        Builder& with_name(const std::string& name);
        Builder& with_default_labels(const std::unordered_map<std::string, std::string>& labels);
        
        Builder& on_event(EventCallback callback);
        Builder& on_error(ErrorCallback callback);
        
        Builder& enable_async_collection(bool enable);
        Builder& enable_health_metrics(bool enable);
        
        std::shared_ptr<MetricsCollector> build();
        
    private:
        std::shared_ptr<MetricRegistry> registry_;
        MetricsCollectorConfig config_;
        EventCallback event_callback_;
        ErrorCallback error_callback_;
    };
    
    ~MetricsCollector();
    
    // 禁止拷贝
    MetricsCollector(const MetricsCollector&) = delete;
    MetricsCollector& operator=(const MetricsCollector&) = delete;
    
    /**
     * @brief 注册组件（通用方法）
     */
    std::string register_component(ComponentPtr component);
    
    /**
     * @brief 注册组件（模板方法，自动创建适配器）
     */
    template<typename T>
    std::string register_component(std::shared_ptr<T> component,
                                  const std::string& component_type,
                                  const std::unordered_map<std::string, std::string>& labels = {},
                                  const std::string& instance_id = "");
    
    /**
     * @brief 注册自定义统计提供器
     */
    std::string register_custom_provider(
        const std::string& component_type,
        std::function<std::unordered_map<std::string, std::variant<int64_t, uint64_t, double, std::string>>()> provider,
        const std::unordered_map<std::string, std::string>& labels = {},
        const std::string& instance_id = "");
    
    /**
     * @brief 移除组件
     */
    bool unregister_component(const std::string& component_id);
    
    /**
     * @brief 注册指标定义
     */
    bool register_metric(const MetricDefinition& definition);
    
    /**
     * @brief 注册指标映射（组件字段 -> 指标）
     */
    bool register_metric_mapping(const std::string& component_type,
                                const std::string& source_field,
                                const MetricDefinition& metric_def);
    
    /**
     * @brief 启动收集器
     */
    bool start();
    
    /**
     * @brief 停止收集器
     */
    void stop(bool graceful = true);
    
    /**
     * @brief 暂停收集器
     */
    void pause();
    
    /**
     * @brief 恢复收集器
     */
    void resume();
    
    /**
     * @brief 执行一次收集
     */
    bool collect();
    
    /**
     * @brief 执行批量收集
     */
    bool collect_batch(const std::vector<std::string>& component_ids);
    
    /**
     * @brief 获取收集器状态
     */
    CollectorState get_state() const { return state_.load(); }
    
    /**
     * @brief 获取配置
     */
    const MetricsCollectorConfig& get_config() const { return config_; }
    
    /**
     * @brief 更新配置
     */
    bool update_config(const MetricsCollectorConfig& new_config);
    
    /**
     * @brief 获取注册的组件列表
     */
    std::vector<std::string> get_registered_components() const;
    
    /**
     * @brief 获取组件信息
     */
    struct ComponentInfo {
        std::string id;
        std::string type;
        std::string instance_id;
        std::unordered_map<std::string, std::string> labels;
        std::chrono::system_clock::time_point registered_time;
        std::chrono::system_clock::time_point last_collection;
        size_t collection_count = 0;
    };
    
    ComponentInfo get_component_info(const std::string& component_id) const;
    
    /**
     * @brief 获取统计信息
     */
    struct CollectorStatistics {
        size_t total_components = 0;
        size_t active_components = 0;
        size_t total_metrics = 0;
        uint64_t total_collections = 0;
        uint64_t failed_collections = 0;
        std::chrono::milliseconds total_collection_time{0};
        std::chrono::milliseconds avg_collection_time{0};
        std::chrono::milliseconds max_collection_time{0};
        std::chrono::system_clock::time_point last_collection;
        std::chrono::system_clock::time_point start_time;
        CollectorState state = CollectorState::STOPPED;
    };
    
    CollectorStatistics get_statistics() const;
    
    /**
     * @brief 重置统计信息
     */
    void reset_statistics();
    
    /**
     * @brief 设置事件回调
     */
    void set_event_callback(EventCallback callback);
    
    /**
     * @brief 设置错误回调
     */
    void set_error_callback(ErrorCallback callback);
    
    /**
     * @brief 获取底层指标注册表
     */
    std::shared_ptr<MetricRegistry> get_registry() const { return registry_; }
    
    /**
     * @brief 导出收集的指标
     */
    std::string export_metrics(const std::string& format = "prometheus") const;
    
    /**
     * @brief 清理过期数据
     */
    size_t cleanup(std::chrono::system_clock::time_point cutoff = std::chrono::system_clock::now());
    
    /**
     * @brief 健康检查
     */
    struct HealthStatus {
        bool healthy;
        std::string status;
        std::unordered_map<std::string, std::string> details;
    };
    
    HealthStatus health_check() const;
    
private:
    MetricsCollector(std::shared_ptr<MetricRegistry> registry, const MetricsCollectorConfig& config);
    
    // 内部数据结构
    struct ComponentEntry {
        std::string id;
        ComponentPtr component;
        std::chrono::system_clock::time_point registered_time;
        std::chrono::system_clock::time_point last_collection;
        size_t collection_count = 0;
        std::unordered_map<std::string, double> last_values; // 上次收集的值（用于增量计算）
        bool enabled = true;
    };
    
    struct MetricEntry {
        MetricDefinition definition;
        std::shared_ptr<IMetric> metric;
        std::chrono::system_clock::time_point created_time;
        std::unordered_map<std::string, std::string> additional_labels;
    };
    
    // 主要组件
    std::shared_ptr<MetricRegistry> registry_;
    MetricsCollectorConfig config_;
    
    // 状态管理
    std::atomic<CollectorState> state_{CollectorState::STOPPED};
    std::atomic<bool> paused_{false};
    
    // 组件和指标存储
    mutable std::shared_mutex components_mutex_;
    std::unordered_map<std::string, ComponentEntry> components_;
    
    mutable std::shared_mutex metrics_mutex_;
    std::unordered_map<std::string, MetricEntry> metrics_;
    
    // 字段到指标的映射
    std::unordered_multimap<std::string, std::string> field_to_metric_; // field_name -> metric_id
    
    // 统计信息
    mutable std::mutex stats_mutex_;
    CollectorStatistics stats_;
    
    // 回调函数
    EventCallback event_callback_;
    ErrorCallback error_callback_;
    
    // 收集线程
    std::thread collection_thread_;
    std::atomic<bool> collection_running_{false};
    std::condition_variable collection_cv_;
    std::mutex collection_mutex_;
    
    // 私有方法
    bool init();
    void cleanup();
    
    void collection_loop();
    bool collect_component(const std::string& component_id);
    bool collect_component_entry(ComponentEntry& entry);
    
    void update_statistics(std::chrono::milliseconds duration, bool success);
    void notify_event(const CollectionEvent& event);
    void notify_error(const std::error_code& ec, const std::string& message);
    
    // 指标处理
    bool create_metric_from_definition(const MetricDefinition& definition);
    bool update_metric(const std::string& metric_id, 
                      double value, 
                      const std::unordered_map<std::string, std::string>& labels);
    
    // 增量计算
    double calculate_incremental_value(const std::string& component_id,
                                      const std::string& field_name,
                                      double current_value);
    
    // 派生指标计算
    double calculate_derived_value(const MetricDefinition& definition,
                                  double current_value,
                                  double last_value);
    
    // 标签处理
    std::unordered_map<std::string, std::string> merge_labels(
        const std::unordered_map<std::string, std::string>& component_labels,
        const std::unordered_map<std::string, std::string>& metric_labels,
        const std::unordered_map<std::string, std::string>& default_labels) const;
    
    // ID生成
    std::string generate_component_id(const std::string& prefix = "comp_");
    std::string generate_metric_id(const std::string& prefix = "metric_");
    
    // 辅助方法
    void safe_invoke_callback(const std::function<void()>& callback);
};

// 预定义组件适配器工厂
namespace metrics_adapter {
    
    // Server适配器
    std::shared_ptr<IStatisticsProvider> create_server_adapter(
        std::shared_ptr<net::Server> server,
        const std::string& instance_id = "",
        const std::unordered_map<std::string, std::string>& labels = {});
    
    // Connection适配器
    std::shared_ptr<IStatisticsProvider> create_connection_adapter(
        std::shared_ptr<net::Connection> connection,
        const std::string& instance_id = "",
        const std::unordered_map<std::string, std::string>& labels = {});
    
    // Protocol适配器
    std::shared_ptr<IStatisticsProvider> create_protocol_adapter(
        std::shared_ptr<net::Protocol> protocol,
        const std::string& instance_id = "",
        const std::unordered_map<std::string, std::string>& labels = {});
    
    // ThreadPool适配器
    std::shared_ptr<IStatisticsProvider> create_thread_pool_adapter(
        std::shared_ptr<net::ThreadPool> thread_pool,
        const std::string& instance_id = "",
        const std::unordered_map<std::string, std::string>& labels = {});
    
    // IOLoop适配器
    std::shared_ptr<IStatisticsProvider> create_io_loop_adapter(
        std::shared_ptr<net::IOLoop> io_loop,
        const std::string& instance_id = "",
        const std::unordered_map<std::string, std::string>& labels = {});
    
} // namespace metrics_adapter

// 预定义指标定义集合
namespace metrics_definitions {
    
    // Server指标
    extern const std::vector<MetricDefinition> SERVER_METRICS;
    
    // Connection指标
    extern const std::vector<MetricDefinition> CONNECTION_METRICS;
    
    // Protocol指标
    extern const std::vector<MetricDefinition> PROTOCOL_METRICS;
    
    // ThreadPool指标
    extern const std::vector<MetricDefinition> THREAD_POOL_METRICS;
    
    // IOLoop指标
    extern const std::vector<MetricDefinition> IO_LOOP_METRICS;
    
    // 系统指标
    extern const std::vector<MetricDefinition> SYSTEM_METRICS;
    
    // 注册所有预定义指标
    void register_all_predefined_metrics(std::shared_ptr<MetricsCollector> collector);
    
} // namespace metrics_definitions

// 模板方法实现
template<typename T>
std::string MetricsCollector::register_component(
    std::shared_ptr<T> component,
    const std::string& component_type,
    const std::unordered_map<std::string, std::string>& labels,
    const std::string& instance_id) {
    
    auto adapter = std::make_shared<ComponentAdapter<T>>(
        component, component_type, instance_id, labels);
    
    return register_component(adapter);
}

} // namespace utils
} // namespace mementodb

#endif // METRICS_COLLECTOR_H
