#ifndef METRICS_H
#define METRICS_H

#include <string>
#include <map>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <chrono>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <type_traits>
#include <optional>
#include <variant>
#include <queue>
#include <deque>
#include <numeric>
#include <system_error>
#include <cstring>

// 可选依赖：用于高性能近似计算
#ifdef METRICS_USE_TDIGEST
#include "tdigest/TDigest.h"
#endif

namespace mementodb {
namespace utils {

// 指标命名空间
class MetricNamespace {
public:
    MetricNamespace(const std::string& name, const std::string& description = "");
    ~MetricNamespace();
    
    const std::string& name() const { return name_; }
    const std::string& description() const { return description_; }
    
private:
    std::string name_;
    std::string description_;
};

// 指标值类型
using MetricValue = std::variant<
    int64_t,
    uint64_t,
    double,
    bool
>;

// 标签系统
class LabelSet {
public:
    using Labels = std::map<std::string, std::string>;
    using LabelHash = size_t;
    
    explicit LabelSet(const Labels& labels = {});
    
    bool empty() const { return labels_.empty(); }
    size_t size() const { return labels_.size(); }
    
    const Labels& labels() const { return labels_; }
    std::string get(const std::string& key, const std::string& default_val = "") const;
    bool has(const std::string& key) const;
    
    void set(const std::string& key, const std::string& value);
    void remove(const std::string& key);
    
    LabelHash hash() const { return hash_; }
    
    bool operator==(const LabelSet& other) const;
    bool operator<(const LabelSet& other) const;
    
    std::string to_string() const;
    
private:
    Labels labels_;
    LabelHash hash_;
    
    void update_hash();
};

// 标签匹配器
class LabelMatcher {
public:
    enum class MatchType {
        EQUAL,      // 等于
        NOT_EQUAL,  // 不等于
        REGEX,      // 正则匹配
        PREFIX,     // 前缀匹配
        SUFFIX,     // 后缀匹配
        EXISTS,     // 存在
        NOT_EXISTS  // 不存在
    };
    
    struct Condition {
        std::string label;
        std::string pattern;
        MatchType type;
        
        bool matches(const LabelSet& labels) const;
    };
    
    LabelMatcher() = default;
    explicit LabelMatcher(const std::vector<Condition>& conditions);
    
    void add_condition(const Condition& condition);
    void add_condition(const std::string& label, const std::string& pattern, MatchType type);
    
    bool matches(const LabelSet& labels) const;
    std::string to_string() const;
    
private:
    std::vector<Condition> conditions_;
};

// 指标样本（带时间戳）
struct MetricSample {
    MetricValue value;
    std::chrono::system_clock::time_point timestamp;
    LabelSet labels;
    std::map<std::string, std::string> metadata;
    
    MetricSample(MetricValue v = 0.0, LabelSet l = {}, std::map<std::string, std::string> meta = {})
        : value(std::move(v)), 
          timestamp(std::chrono::system_clock::now()), 
          labels(std::move(l)),
          metadata(std::move(meta)) {}
    
    template<typename T>
    T get_value() const {
        try {
            return std::get<T>(value);
        } catch (const std::bad_variant_access&) {
            throw std::runtime_error("Invalid metric value type");
        }
    }
};

// 指标类型
enum class MetricType {
    COUNTER,        // 计数器（只增不减）
    GAUGE,          // 仪表盘（可增可减）
    HISTOGRAM,      // 直方图（分布统计）
    SUMMARY,        // 摘要（分位数统计）
    TIMING,         // 计时指标（毫秒级）
    EVENT,          // 事件指标（带时间戳的事件）
    RATE,           // 速率指标（变化率）
    DERIVATIVE,     // 导数指标
    CUMULATIVE      // 累积指标
};

// 直方图配置
struct HistogramConfig {
    std::vector<double> buckets;
    bool linear_buckets = false;
    double linear_start = 0.0;
    double linear_width = 0.0;
    int linear_count = 0;
    bool exponential_buckets = false;
    double exponential_start = 0.0;
    double exponential_factor = 0.0;
    int exponential_count = 0;
    
    static HistogramConfig linear(double start, double width, int count);
    static HistogramConfig exponential(double start, double factor, int count);
    static HistogramConfig default_config();
    
    std::vector<double> get_buckets() const;
};

// 摘要配置
struct SummaryConfig {
    std::vector<double> quantiles = {0.5, 0.9, 0.95, 0.99, 0.999};
    size_t max_samples = 1000;  // 最大样本数
    size_t age_buckets = 5;     // 年龄桶数量
    std::chrono::seconds max_age = std::chrono::seconds(600); // 最大年龄
    
#ifdef METRICS_USE_TDIGEST
    double compression = 100.0; // T-Digest压缩参数
#endif
    
    static SummaryConfig default_config();
};

// 指标配置
struct MetricConfig {
    MetricType type = MetricType::GAUGE;
    std::string unit;  // 单位（如 "seconds", "bytes", "requests"）
    std::chrono::seconds ttl = std::chrono::seconds(0); // 生存时间（0表示永不过期）
    bool enabled = true;
    bool reset_on_export = false; // 导出后是否重置
    std::map<std::string, std::string> metadata;
    
    // 类型特定配置
    std::optional<HistogramConfig> histogram_config;
    std::optional<SummaryConfig> summary_config;
    
    static MetricConfig counter_config(const std::string& unit = "");
    static MetricConfig gauge_config(const std::string& unit = "");
    static MetricConfig histogram_config_static(const HistogramConfig& hist_config = HistogramConfig::default_config());
    static MetricConfig summary_config_static(const SummaryConfig& sum_config = SummaryConfig::default_config());
};

// 指标描述
struct MetricDescriptor {
    std::string name;
    std::string description;
    MetricType type;
    std::string unit;
    std::vector<std::string> label_names;
    std::chrono::system_clock::time_point created_time;
    MetricConfig config;
    
    std::string to_string() const;
};

// 指标数据点
struct DataPoint {
    MetricValue value;
    std::chrono::system_clock::time_point timestamp;
    std::chrono::milliseconds duration; // 对于计时指标
    std::map<std::string, std::string> metadata;
    
    DataPoint(MetricValue v, 
              std::chrono::system_clock::time_point ts = std::chrono::system_clock::now(),
              std::chrono::milliseconds dur = std::chrono::milliseconds(0),
              std::map<std::string, std::string> meta = {})
        : value(std::move(v)), timestamp(ts), duration(dur), metadata(std::move(meta)) {}
};

// 指标时间序列
class TimeSeries {
public:
    TimeSeries(const std::string& name, const LabelSet& labels, const MetricConfig& config);
    ~TimeSeries();
    
    const std::string& name() const { return name_; }
    const LabelSet& labels() const { return labels_; }
    const MetricConfig& config() const { return config_; }
    
    // 数据操作
    void record(const DataPoint& point);
    void record(MetricValue value, std::map<std::string, std::string> metadata = {});
    
    // 查询操作
    std::vector<DataPoint> get_points(std::chrono::system_clock::time_point start,
                                      std::chrono::system_clock::time_point end) const;
    std::optional<DataPoint> last_point() const;
    
    // 聚合操作
    struct Statistics {
        double min = 0.0;
        double max = 0.0;
        double mean = 0.0;
        double median = 0.0;
        double stddev = 0.0;
        double p90 = 0.0;
        double p95 = 0.0;
        double p99 = 0.0;
        uint64_t count = 0;
        double sum = 0.0;
    };
    
    Statistics statistics(std::chrono::system_clock::time_point start,
                         std::chrono::system_clock::time_point end) const;
    
    // 清理过期数据
    size_t cleanup(std::chrono::system_clock::time_point cutoff);
    
    // 导出
    std::string to_prometheus() const;
    std::string to_json() const;
    std::string to_influxdb() const;
    
private:
    std::string name_;
    LabelSet labels_;
    MetricConfig config_;
    
    mutable std::shared_mutex data_mutex_;
    std::deque<DataPoint> data_points_;
    std::chrono::system_clock::time_point first_point_time_;
    std::chrono::system_clock::time_point last_point_time_;
    
    // 采样配置
    size_t max_points_ = 10000;
    std::chrono::seconds max_age_ = std::chrono::seconds(3600);
    
    void apply_sampling();
};

// 指标接口
class IMetric {
public:
    virtual ~IMetric() = default;
    
    virtual const MetricDescriptor& descriptor() const = 0;
    virtual MetricType type() const = 0;
    virtual const std::string& name() const = 0;
    virtual const std::string& description() const = 0;
    virtual const std::string& unit() const = 0;
    
    // 获取所有时间序列
    virtual std::vector<std::shared_ptr<TimeSeries>> get_time_series() const = 0;
    
    // 根据标签获取时间序列
    virtual std::shared_ptr<TimeSeries> get_time_series(const LabelSet& labels) = 0;
    
    // 查询匹配的时间序列
    virtual std::vector<std::shared_ptr<TimeSeries>> find_time_series(const LabelMatcher& matcher) const = 0;
    
    // 记录数据
    virtual void record(double value, const LabelSet& labels = {}, 
                       std::map<std::string, std::string> metadata = {}) = 0;
    
    // 批量记录
    virtual void record_batch(const std::vector<double>& values, 
                             const std::vector<LabelSet>& labels = {},
                             const std::vector<std::map<std::string, std::string>>& metadata = {}) = 0;
    
    // 导出
    virtual std::string to_prometheus() const = 0;
    virtual std::string to_json() const = 0;
    virtual std::string to_influxdb() const = 0;
    
    // 清理
    virtual size_t cleanup(std::chrono::system_clock::time_point cutoff) = 0;
    virtual void reset() = 0;
};

// 计数器指标
class CounterMetric : public IMetric {
public:
    CounterMetric(const std::string& name, 
                  const std::string& description = "",
                  const MetricConfig& config = MetricConfig::counter_config());
    
    const MetricDescriptor& descriptor() const override { return descriptor_; }
    MetricType type() const override { return MetricType::COUNTER; }
    const std::string& name() const override { return descriptor_.name; }
    const std::string& description() const override { return descriptor_.description; }
    const std::string& unit() const override { return descriptor_.unit; }
    
    // 增加计数器
    void inc(double value = 1.0, const LabelSet& labels = {}, 
             std::map<std::string, std::string> metadata = {});
    
    // 获取当前值
    double get(const LabelSet& labels = {}) const;
    
    // 重置计数器
    void reset(const LabelSet& labels = {});
    
    // IMetric接口实现
    std::vector<std::shared_ptr<TimeSeries>> get_time_series() const override;
    std::shared_ptr<TimeSeries> get_time_series(const LabelSet& labels) override;
    std::vector<std::shared_ptr<TimeSeries>> find_time_series(const LabelMatcher& matcher) const override;
    
    void record(double value, const LabelSet& labels = {},
               std::map<std::string, std::string> metadata = {}) override;
    void record_batch(const std::vector<double>& values,
                     const std::vector<LabelSet>& labels = {},
                     const std::vector<std::map<std::string, std::string>>& metadata = {}) override;
    
    std::string to_prometheus() const override;
    std::string to_json() const override;
    std::string to_influxdb() const override;
    
    size_t cleanup(std::chrono::system_clock::time_point cutoff) override;
    void reset() override;
    
private:
    MetricDescriptor descriptor_;
    mutable std::shared_mutex series_mutex_;
    std::map<LabelSet, std::shared_ptr<TimeSeries>> time_series_;
    
    std::shared_ptr<TimeSeries> get_or_create_series(const LabelSet& labels);
};

// 仪表盘指标
class GaugeMetric : public IMetric {
public:
    GaugeMetric(const std::string& name,
                const std::string& description = "",
                const MetricConfig& config = MetricConfig::gauge_config());
    
    const MetricDescriptor& descriptor() const override { return descriptor_; }
    MetricType type() const override { return MetricType::GAUGE; }
    const std::string& name() const override { return descriptor_.name; }
    const std::string& description() const override { return descriptor_.description; }
    const std::string& unit() const override { return descriptor_.unit; }
    
    // 设置值
    void set(double value, const LabelSet& labels = {},
             std::map<std::string, std::string> metadata = {});
    
    // 增加/减少
    void inc(double value = 1.0, const LabelSet& labels = {},
             std::map<std::string, std::string> metadata = {});
    void dec(double value = 1.0, const LabelSet& labels = {},
             std::map<std::string, std::string> metadata = {});
    
    // 获取当前值
    double get(const LabelSet& labels = {}) const;
    
    // IMetric接口实现
    std::vector<std::shared_ptr<TimeSeries>> get_time_series() const override;
    std::shared_ptr<TimeSeries> get_time_series(const LabelSet& labels) override;
    std::vector<std::shared_ptr<TimeSeries>> find_time_series(const LabelMatcher& matcher) const override;
    
    void record(double value, const LabelSet& labels = {},
               std::map<std::string, std::string> metadata = {}) override;
    void record_batch(const std::vector<double>& values,
                     const std::vector<LabelSet>& labels = {},
                     const std::vector<std::map<std::string, std::string>>& metadata = {}) override;
    
    std::string to_prometheus() const override;
    std::string to_json() const override;
    std::string to_influxdb() const override;
    
    size_t cleanup(std::chrono::system_clock::time_point cutoff) override;
    void reset() override;
    
private:
    MetricDescriptor descriptor_;
    mutable std::shared_mutex series_mutex_;
    std::map<LabelSet, std::shared_ptr<TimeSeries>> time_series_;
    
    std::shared_ptr<TimeSeries> get_or_create_series(const LabelSet& labels);
};

// 直方图指标
class HistogramMetric : public IMetric {
public:
    HistogramMetric(const std::string& name,
                    const std::string& description = "",
                    const MetricConfig& config = MetricConfig::histogram_config_static());
    
    const MetricDescriptor& descriptor() const override { return descriptor_; }
    MetricType type() const override { return MetricType::HISTOGRAM; }
    const std::string& name() const override { return descriptor_.name; }
    const std::string& description() const override { return descriptor_.description; }
    const std::string& unit() const override { return descriptor_.unit; }
    
    // 记录观察值
    void observe(double value, const LabelSet& labels = {},
                 std::map<std::string, std::string> metadata = {});
    
    // 获取统计信息
    struct HistogramStatistics {
        uint64_t count = 0;
        double sum = 0.0;
        double min = 0.0;
        double max = 0.0;
        double mean = 0.0;
        std::map<double, uint64_t> buckets;
        std::optional<double> bucket_for(double value) const;
    };
    
    HistogramStatistics get_statistics(const LabelSet& labels = {}) const;
    
    // IMetric接口实现
    std::vector<std::shared_ptr<TimeSeries>> get_time_series() const override;
    std::shared_ptr<TimeSeries> get_time_series(const LabelSet& labels) override;
    std::vector<std::shared_ptr<TimeSeries>> find_time_series(const LabelMatcher& matcher) const override;
    
    void record(double value, const LabelSet& labels = {},
               std::map<std::string, std::string> metadata = {}) override;
    void record_batch(const std::vector<double>& values,
                     const std::vector<LabelSet>& labels = {},
                     const std::vector<std::map<std::string, std::string>>& metadata = {}) override;
    
    std::string to_prometheus() const override;
    std::string to_json() const override;
    std::string to_influxdb() const override;
    
    size_t cleanup(std::chrono::system_clock::time_point cutoff) override;
    void reset() override;
    
private:
    MetricDescriptor descriptor_;
    HistogramConfig histogram_config_;
    mutable std::shared_mutex series_mutex_;
    std::map<LabelSet, std::shared_ptr<TimeSeries>> time_series_;
    
    std::shared_ptr<TimeSeries> get_or_create_series(const LabelSet& labels);
    
    // 直方图特定数据结构
    struct BucketCounts {
        std::vector<std::atomic<uint64_t>> counts;
        std::atomic<double> sum{0.0};
        std::atomic<uint64_t> total{0};
    };
    
    mutable std::shared_mutex bucket_mutex_;
    std::map<LabelSet, std::shared_ptr<BucketCounts>> bucket_data_;
};

// 摘要指标（使用T-Digest近似算法）
class SummaryMetric : public IMetric {
public:
    SummaryMetric(const std::string& name,
                  const std::string& description = "",
                  const MetricConfig& config = MetricConfig::summary_config_static());
    
    const MetricDescriptor& descriptor() const override { return descriptor_; }
    MetricType type() const override { return MetricType::SUMMARY; }
    const std::string& name() const override { return descriptor_.name; }
    const std::string& description() const override { return descriptor_.description; }
    const std::string& unit() const override { return descriptor_.unit; }
    
    // 记录观察值
    void observe(double value, const LabelSet& labels = {},
                 std::map<std::string, std::string> metadata = {});
    
    // 获取分位数
    double quantile(double q, const LabelSet& labels = {}) const;
    std::map<double, double> quantiles(const std::vector<double>& qs, const LabelSet& labels = {}) const;
    
    // 获取统计信息
    struct SummaryStatistics {
        uint64_t count = 0;
        double sum = 0.0;
        double min = 0.0;
        double max = 0.0;
        double mean = 0.0;
        std::map<double, double> quantiles;
    };
    
    SummaryStatistics get_statistics(const LabelSet& labels = {}) const;
    
    // IMetric接口实现
    std::vector<std::shared_ptr<TimeSeries>> get_time_series() const override;
    std::shared_ptr<TimeSeries> get_time_series(const LabelSet& labels) override;
    std::vector<std::shared_ptr<TimeSeries>> find_time_series(const LabelMatcher& matcher) const override;
    
    void record(double value, const LabelSet& labels = {},
               std::map<std::string, std::string> metadata = {}) override;
    void record_batch(const std::vector<double>& values,
                     const std::vector<LabelSet>& labels = {},
                     const std::vector<std::map<std::string, std::string>>& metadata = {}) override;
    
    std::string to_prometheus() const override;
    std::string to_json() const override;
    std::string to_influxdb() const override;
    
    size_t cleanup(std::chrono::system_clock::time_point cutoff) override;
    void reset() override;
    
private:
    MetricDescriptor descriptor_;
    SummaryConfig summary_config_;
    mutable std::shared_mutex series_mutex_;
    std::map<LabelSet, std::shared_ptr<TimeSeries>> time_series_;
    
    std::shared_ptr<TimeSeries> get_or_create_series(const LabelSet& labels);
    
#ifdef METRICS_USE_TDIGEST
    // T-Digest数据结构
    struct TDigestData {
        std::unique_ptr<TDigest> digest;
        std::atomic<double> sum{0.0};
        std::atomic<uint64_t> count{0};
        std::chrono::system_clock::time_point last_update;
    };
    
    mutable std::shared_mutex tdigest_mutex_;
    std::map<LabelSet, std::shared_ptr<TDigestData>> tdigest_data_;
#else
    // 简单实现：存储样本
    struct SampleData {
        std::vector<double> samples;
        std::atomic<double> sum{0.0};
        std::atomic<uint64_t> count{0};
        std::chrono::system_clock::time_point last_update;
        mutable std::mutex mutex_;
        
        void add_sample(double value);
        double get_quantile(double q) const;
    };
    
    mutable std::shared_mutex sample_mutex_;
    std::map<LabelSet, std::shared_ptr<SampleData>> sample_data_;
#endif
};

// 指标注册表
class MetricRegistry : public std::enable_shared_from_this<MetricRegistry> {
public:
    using MetricFactory = std::function<std::shared_ptr<IMetric>(
        const std::string&, const std::string&, const MetricConfig&)>;
    
    // 创建注册表
    static std::shared_ptr<MetricRegistry> create();
    
    // 创建命名空间
    std::shared_ptr<MetricNamespace> create_namespace(const std::string& name, 
                                                      const std::string& description = "");
    
    // 注册指标
    std::shared_ptr<IMetric> register_metric(const std::string& name,
                                             const std::string& description,
                                             MetricType type,
                                             const MetricConfig& config = MetricConfig());
    
    // 获取指标
    std::shared_ptr<IMetric> get_metric(const std::string& name) const;
    
    // 获取所有指标
    std::vector<std::shared_ptr<IMetric>> get_all_metrics() const;
    
    // 根据标签匹配查找指标
    std::vector<std::shared_ptr<IMetric>> find_metrics(const LabelMatcher& matcher) const;
    
    // 删除指标
    bool unregister_metric(const std::string& name);
    
    // 导出
    std::string to_prometheus() const;
    std::string to_json() const;
    std::string to_influxdb() const;
    
    // 清理过期数据
    size_t cleanup(std::chrono::system_clock::time_point cutoff = std::chrono::system_clock::now());
    
    // 重置所有指标
    void reset_all();
    
    // 获取统计信息
    struct RegistryStatistics {
        size_t total_metrics = 0;
        size_t total_time_series = 0;
        size_t counters = 0;
        size_t gauges = 0;
        size_t histograms = 0;
        size_t summaries = 0;
        size_t other_types = 0;
        size_t expired_metrics = 0;
        std::chrono::system_clock::time_point last_cleanup;
    };
    
    RegistryStatistics get_statistics() const;
    
    // 设置全局配置
    void set_default_config(const MetricConfig& config);
    void set_auto_cleanup_enabled(bool enabled);
    void set_cleanup_interval(std::chrono::seconds interval);
    
    // 启动/停止自动清理
    void start_auto_cleanup();
    void stop_auto_cleanup();
    
private:
    MetricRegistry();
    
    mutable std::shared_mutex metrics_mutex_;
    std::unordered_map<std::string, std::shared_ptr<IMetric>> metrics_;
    std::unordered_map<std::string, std::shared_ptr<MetricNamespace>> namespaces_;
    
    MetricConfig default_config_;
    bool auto_cleanup_enabled_ = true;
    std::chrono::seconds cleanup_interval_ = std::chrono::seconds(60);
    std::atomic<bool> cleanup_running_{false};
    std::thread cleanup_thread_;
    
    std::unordered_map<MetricType, MetricFactory> metric_factories_;
    
    void init_factories();
    void cleanup_loop();
};

// 全局指标管理器
class MetricsManager {
public:
    // 初始化
    static void initialize();
    static void shutdown();
    
    // 获取全局注册表
    static std::shared_ptr<MetricRegistry> global_registry();
    
    // 创建注册表
    static std::shared_ptr<MetricRegistry> create_registry();
    
    // 便捷方法：创建指标
    static std::shared_ptr<CounterMetric> create_counter(const std::string& name,
                                                         const std::string& description = "",
                                                         const MetricConfig& config = MetricConfig::counter_config());
    
    static std::shared_ptr<GaugeMetric> create_gauge(const std::string& name,
                                                     const std::string& description = "",
                                                     const MetricConfig& config = MetricConfig::gauge_config());
    
    static std::shared_ptr<HistogramMetric> create_histogram(const std::string& name,
                                                             const std::string& description = "",
                                                             const MetricConfig& config = MetricConfig::histogram_config_static());
    
    static std::shared_ptr<SummaryMetric> create_summary(const std::string& name,
                                                         const std::string& description = "",
                                                         const MetricConfig& config = MetricConfig::summary_config_static());
    
    // 系统指标收集
    class SystemMetricsCollector {
    public:
        SystemMetricsCollector(std::shared_ptr<MetricRegistry> registry = global_registry());
        ~SystemMetricsCollector();
        
        void start();
        void stop();
        
        void collect_cpu_usage();
        void collect_memory_usage();
        void collect_disk_usage();
        void collect_network_io();
        void collect_process_stats();
        
        void set_collection_interval(std::chrono::seconds interval);
        void set_enabled_metrics(const std::vector<std::string>& metrics);
        
    private:
        std::shared_ptr<MetricRegistry> registry_;
        std::atomic<bool> running_{false};
        std::chrono::seconds interval_ = std::chrono::seconds(10);
        std::thread collection_thread_;
        
        std::shared_ptr<GaugeMetric> cpu_usage_;
        std::shared_ptr<GaugeMetric> memory_usage_;
        std::shared_ptr<GaugeMetric> disk_usage_;
        std::shared_ptr<CounterMetric> network_rx_;
        std::shared_ptr<CounterMetric> network_tx_;
        std::shared_ptr<GaugeMetric> open_files_;
        
        void collection_loop();
        
#ifdef _WIN32
        // Windows特定实现
#elif defined(__linux__)
        // Linux特定实现
        struct LinuxSystemStats {
            uint64_t cpu_user = 0;
            uint64_t cpu_nice = 0;
            uint64_t cpu_system = 0;
            uint64_t cpu_idle = 0;
            uint64_t cpu_iowait = 0;
            uint64_t cpu_irq = 0;
            uint64_t cpu_softirq = 0;
        };
        
        LinuxSystemStats prev_cpu_stats_;
        std::chrono::system_clock::time_point last_cpu_collection_;
        
        void collect_linux_cpu_usage();
        void collect_linux_memory_usage();
#endif
    };
    
    // 获取系统指标收集器
    static std::shared_ptr<SystemMetricsCollector> system_metrics_collector();
    
    // 指标中间件（用于HTTP服务器等）
    class MetricsMiddleware {
    public:
        struct RequestMetrics {
            std::shared_ptr<CounterMetric> request_counter;
            std::shared_ptr<HistogramMetric> request_duration;
            std::shared_ptr<CounterMetric> error_counter;
        };
        
        static RequestMetrics create_http_metrics(const std::string& service_name);
        
        // HTTP中间件
        class HttpMiddleware {
        public:
            HttpMiddleware(const RequestMetrics& metrics);
            
            std::function<void(const std::string&, const std::string&, uint64_t)> 
            get_handler();
            
        private:
            RequestMetrics metrics_;
        };
    };
    
    // 导出器（将指标推送到外部系统）
    class MetricsExporter {
    public:
        virtual ~MetricsExporter() = default;
        
        virtual void export_metrics(const std::shared_ptr<MetricRegistry>& registry) = 0;
        virtual void start() = 0;
        virtual void stop() = 0;
        
        virtual std::string name() const = 0;
    };
    
    class PrometheusExporter : public MetricsExporter {
    public:
        PrometheusExporter(const std::string& endpoint, uint16_t port = 9090);
        
        void export_metrics(const std::shared_ptr<MetricRegistry>& registry) override;
        void start() override;
        void stop() override;
        
        std::string name() const override { return "prometheus"; }
        
    private:
        std::string endpoint_;
        uint16_t port_;
        std::atomic<bool> running_{false};
        std::thread server_thread_;
    };
    
    class InfluxDBExporter : public MetricsExporter {
    public:
        InfluxDBExporter(const std::string& url, 
                         const std::string& database,
                         const std::string& username = "",
                         const std::string& password = "");
        
        void export_metrics(const std::shared_ptr<MetricRegistry>& registry) override;
        void start() override;
        void stop() override;
        
        std::string name() const override { return "influxdb"; }
        
        void set_batch_size(size_t size);
        void set_flush_interval(std::chrono::seconds interval);
        
    private:
        std::string url_;
        std::string database_;
        std::string username_;
        std::string password_;
        std::atomic<bool> running_{false};
        std::thread export_thread_;
        size_t batch_size_ = 1000;
        std::chrono::seconds flush_interval_ = std::chrono::seconds(10);
        
        void export_loop();
    };
    
    // 注册导出器
    static void register_exporter(std::shared_ptr<MetricsExporter> exporter);
    static void unregister_exporter(const std::string& name);
    static void start_all_exporters();
    static void stop_all_exporters();
    
private:
    MetricsManager() = delete;
    ~MetricsManager() = delete;
    
    static std::shared_ptr<MetricRegistry> global_registry_;
    static std::shared_ptr<SystemMetricsCollector> system_collector_;
    static std::vector<std::shared_ptr<MetricsExporter>> exporters_;
    static std::once_flag init_flag_;
    static std::mutex exporters_mutex_;
};

// 便捷宏
#define METRICS_COUNTER(name, ...) \
    mementodb::utils::MetricsManager::create_counter(name, ##__VA_ARGS__)

#define METRICS_GAUGE(name, ...) \
    mementodb::utils::MetricsManager::create_gauge(name, ##__VA_ARGS__)

#define METRICS_HISTOGRAM(name, ...) \
    mementodb::utils::MetricsManager::create_histogram(name, ##__VA_ARGS__)

#define METRICS_SUMMARY(name, ...) \
    mementodb::utils::MetricsManager::create_summary(name, ##__VA_ARGS__)

#define METRICS_INCREMENT(counter, value) \
    if (counter) counter->inc(value)

#define METRICS_TIMED(histogram, code_block) \
    { \
        auto start = std::chrono::steady_clock::now(); \
        try { \
            code_block \
        } catch (...) { \
            auto end = std::chrono::steady_clock::now(); \
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start); \
            if (histogram) histogram->observe(duration.count()); \
            throw; \
        } \
        auto end = std::chrono::steady_clock::now(); \
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start); \
        if (histogram) histogram->observe(duration.count()); \
    }

} // namespace utils
} // namespace mementodb

#endif // METRICS_H
