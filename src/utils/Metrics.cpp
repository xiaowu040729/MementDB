// File: src/utils/Metrics_New.cpp
// 新的高级Metrics系统实现

#include "Metrics.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <regex>
#include <thread>
#include <numeric>

#ifdef _WIN32
    #include <windows.h>
    #include <psapi.h>
#else
    #include <unistd.h>
    #include <sys/resource.h>
    #include <sys/stat.h>
    #include <dirent.h>
    #include <fstream>
#endif

namespace mementodb {
namespace utils {

// ==================== MetricNamespace 实现 ====================

MetricNamespace::MetricNamespace(const std::string& name, const std::string& description)
    : name_(name), description_(description) {
}

MetricNamespace::~MetricNamespace() = default;

// ==================== LabelSet 实现 ====================

LabelSet::LabelSet(const Labels& labels) : labels_(labels) {
    update_hash();
}

std::string LabelSet::get(const std::string& key, const std::string& default_val) const {
    auto it = labels_.find(key);
    return it != labels_.end() ? it->second : default_val;
}

bool LabelSet::has(const std::string& key) const {
    return labels_.find(key) != labels_.end();
}

void LabelSet::set(const std::string& key, const std::string& value) {
    labels_[key] = value;
    update_hash();
}

void LabelSet::remove(const std::string& key) {
    labels_.erase(key);
    update_hash();
}

bool LabelSet::operator==(const LabelSet& other) const {
    return labels_ == other.labels_;
}

bool LabelSet::operator<(const LabelSet& other) const {
    return labels_ < other.labels_;
}

std::string LabelSet::to_string() const {
    std::ostringstream oss;
    bool first = true;
    for (const auto& pair : labels_) {
        if (!first) oss << ",";
        first = false;
        oss << pair.first << "=" << pair.second;
    }
    return oss.str();
}

void LabelSet::update_hash() {
    std::hash<std::string> hasher;
    hash_ = 0;
    for (const auto& pair : labels_) {
        hash_ ^= hasher(pair.first) ^ (hasher(pair.second) << 1);
    }
}

// ==================== LabelMatcher 实现 ====================

LabelMatcher::LabelMatcher(const std::vector<Condition>& conditions)
    : conditions_(conditions) {
}

void LabelMatcher::add_condition(const Condition& condition) {
    conditions_.push_back(condition);
}

void LabelMatcher::add_condition(const std::string& label, const std::string& pattern, MatchType type) {
    conditions_.push_back({label, pattern, type});
}

bool LabelMatcher::matches(const LabelSet& labels) const {
    for (const auto& condition : conditions_) {
        if (!condition.matches(labels)) {
            return false;
        }
    }
    return true;
}

std::string LabelMatcher::to_string() const {
    std::ostringstream oss;
    bool first = true;
    for (const auto& condition : conditions_) {
        if (!first) oss << " AND ";
        first = false;
        oss << condition.label << " " << static_cast<int>(condition.type) << " " << condition.pattern;
    }
    return oss.str();
}

bool LabelMatcher::Condition::matches(const LabelSet& labels) const {
    std::string value = labels.get(label, "");
    
    switch (type) {
        case MatchType::EQUAL:
            return value == pattern;
        case MatchType::NOT_EQUAL:
            return value != pattern;
        case MatchType::REGEX: {
            std::regex regex_pattern(pattern);
            return std::regex_match(value, regex_pattern);
        }
        case MatchType::PREFIX:
            return value.length() >= pattern.length() && 
                   value.substr(0, pattern.length()) == pattern;
        case MatchType::SUFFIX:
            return value.length() >= pattern.length() && 
                   value.substr(value.length() - pattern.length()) == pattern;
        case MatchType::EXISTS:
            return labels.has(label);
        case MatchType::NOT_EXISTS:
            return !labels.has(label);
        default:
            return false;
    }
}

// ==================== HistogramConfig 实现 ====================

HistogramConfig HistogramConfig::linear(double start, double width, int count) {
    HistogramConfig config;
    config.linear_buckets = true;
    config.linear_start = start;
    config.linear_width = width;
    config.linear_count = count;
    return config;
}

HistogramConfig HistogramConfig::exponential(double start, double factor, int count) {
    HistogramConfig config;
    config.exponential_buckets = true;
    config.exponential_start = start;
    config.exponential_factor = factor;
    config.exponential_count = count;
    return config;
}

HistogramConfig HistogramConfig::default_config() {
    HistogramConfig config;
    config.buckets = {0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0};
    return config;
}

std::vector<double> HistogramConfig::get_buckets() const {
    if (!buckets.empty()) {
        return buckets;
    }
    
    std::vector<double> result;
    
    if (linear_buckets) {
        for (int i = 0; i < linear_count; ++i) {
            result.push_back(linear_start + i * linear_width);
        }
    } else if (exponential_buckets) {
        double value = exponential_start;
        for (int i = 0; i < exponential_count; ++i) {
            result.push_back(value);
            value *= exponential_factor;
        }
    }
    
    return result;
}

// ==================== SummaryConfig 实现 ====================

SummaryConfig SummaryConfig::default_config() {
    SummaryConfig config;
    config.quantiles = {0.5, 0.9, 0.95, 0.99, 0.999};
    config.max_samples = 1000;
    config.age_buckets = 5;
    config.max_age = std::chrono::seconds(600);
    return config;
}

// ==================== MetricConfig 实现 ====================

MetricConfig MetricConfig::counter_config(const std::string& unit) {
    MetricConfig config;
    config.type = MetricType::COUNTER;
    config.unit = unit;
    return config;
}

MetricConfig MetricConfig::gauge_config(const std::string& unit) {
    MetricConfig config;
    config.type = MetricType::GAUGE;
    config.unit = unit;
    return config;
}

MetricConfig MetricConfig::histogram_config_static(const HistogramConfig& hist_config) {
    MetricConfig config;
    config.type = MetricType::HISTOGRAM;
    config.histogram_config = hist_config;
    return config;
}

MetricConfig MetricConfig::summary_config_static(const SummaryConfig& sum_config) {
    MetricConfig config;
    config.type = MetricType::SUMMARY;
    config.summary_config = sum_config;
    return config;
}

// ==================== MetricDescriptor 实现 ====================

std::string MetricDescriptor::to_string() const {
    std::ostringstream oss;
    oss << "MetricDescriptor{name=" << name 
        << ", type=" << static_cast<int>(type)
        << ", unit=" << unit
        << ", description=" << description << "}";
    return oss.str();
}

// ==================== TimeSeries 实现 ====================

TimeSeries::TimeSeries(const std::string& name, const LabelSet& labels, const MetricConfig& config)
    : name_(name), labels_(labels), config_(config),
      first_point_time_(std::chrono::system_clock::now()),
      last_point_time_(std::chrono::system_clock::now()) {
}

TimeSeries::~TimeSeries() = default;

void TimeSeries::record(const DataPoint& point) {
    std::unique_lock lock(data_mutex_);
    data_points_.push_back(point);
    last_point_time_ = point.timestamp;
    if (data_points_.size() == 1) {
        first_point_time_ = point.timestamp;
    }
    apply_sampling();
}

void TimeSeries::record(MetricValue value, std::map<std::string, std::string> metadata) {
    record(DataPoint(value, std::chrono::system_clock::now(), std::chrono::milliseconds(0), metadata));
}

std::vector<DataPoint> TimeSeries::get_points(std::chrono::system_clock::time_point start,
                                              std::chrono::system_clock::time_point end) const {
    std::shared_lock lock(data_mutex_);
    std::vector<DataPoint> result;
    for (const auto& point : data_points_) {
        if (point.timestamp >= start && point.timestamp <= end) {
            result.push_back(point);
        }
    }
    return result;
}

std::optional<DataPoint> TimeSeries::last_point() const {
    std::shared_lock lock(data_mutex_);
    if (data_points_.empty()) {
        return std::nullopt;
    }
    return data_points_.back();
}

TimeSeries::Statistics TimeSeries::statistics(std::chrono::system_clock::time_point start,
                                               std::chrono::system_clock::time_point end) const {
    std::shared_lock lock(data_mutex_);
    Statistics stats;
    
    std::vector<double> values;
    for (const auto& point : data_points_) {
        if (point.timestamp >= start && point.timestamp <= end) {
            double value = 0.0;
            if (std::holds_alternative<double>(point.value)) {
                value = std::get<double>(point.value);
            } else if (std::holds_alternative<int64_t>(point.value)) {
                value = static_cast<double>(std::get<int64_t>(point.value));
            } else if (std::holds_alternative<uint64_t>(point.value)) {
                value = static_cast<double>(std::get<uint64_t>(point.value));
            }
            values.push_back(value);
            stats.sum += value;
        }
    }
    
    if (values.empty()) {
        return stats;
    }
    
    std::sort(values.begin(), values.end());
    stats.count = values.size();
    stats.min = values.front();
    stats.max = values.back();
    stats.mean = stats.sum / stats.count;
    
    // 计算中位数
    if (stats.count % 2 == 0) {
        stats.median = (values[stats.count / 2 - 1] + values[stats.count / 2]) / 2.0;
    } else {
        stats.median = values[stats.count / 2];
    }
    
    // 计算分位数
    auto percentile = [&values](double p) -> double {
        size_t index = static_cast<size_t>(p * (values.size() - 1));
        if (index >= values.size()) index = values.size() - 1;
        return values[index];
    };
    
    stats.p90 = percentile(0.9);
    stats.p95 = percentile(0.95);
    stats.p99 = percentile(0.99);
    
    // 计算标准差
    double variance = 0.0;
    for (double v : values) {
        variance += (v - stats.mean) * (v - stats.mean);
    }
    stats.stddev = std::sqrt(variance / stats.count);
    
    return stats;
}

size_t TimeSeries::cleanup(std::chrono::system_clock::time_point cutoff) {
    std::unique_lock lock(data_mutex_);
    size_t removed = 0;
    auto it = data_points_.begin();
    while (it != data_points_.end()) {
        if (it->timestamp < cutoff) {
            it = data_points_.erase(it);
            removed++;
        } else {
            ++it;
        }
    }
    return removed;
}

std::string TimeSeries::to_prometheus() const {
    std::shared_lock lock(data_mutex_);
    if (data_points_.empty()) {
        return "";
    }
    
    std::ostringstream oss;
    auto last = data_points_.back();
    oss << name_;
    if (!labels_.empty()) {
        oss << "{";
        bool first = true;
        for (const auto& label : labels_.labels()) {
            if (!first) oss << ",";
            first = false;
            oss << label.first << "=\"" << label.second << "\"";
        }
        oss << "}";
    }
    oss << " ";
    
    double value = 0.0;
    if (std::holds_alternative<double>(last.value)) {
        value = std::get<double>(last.value);
    } else if (std::holds_alternative<int64_t>(last.value)) {
        value = static_cast<double>(std::get<int64_t>(last.value));
    } else if (std::holds_alternative<uint64_t>(last.value)) {
        value = static_cast<double>(std::get<uint64_t>(last.value));
    }
    
    oss << std::fixed << std::setprecision(6) << value << "\n";
    return oss.str();
}

std::string TimeSeries::to_json() const {
    std::shared_lock lock(data_mutex_);
    std::ostringstream oss;
    oss << "{\"name\":\"" << name_ << "\",\"labels\":{";
    bool first = true;
    for (const auto& label : labels_.labels()) {
        if (!first) oss << ",";
        first = false;
        oss << "\"" << label.first << "\":\"" << label.second << "\"";
    }
    oss << "},\"points\":[";
    first = true;
    for (const auto& point : data_points_) {
        if (!first) oss << ",";
        first = false;
        double value = 0.0;
        if (std::holds_alternative<double>(point.value)) {
            value = std::get<double>(point.value);
        } else if (std::holds_alternative<int64_t>(point.value)) {
            value = static_cast<double>(std::get<int64_t>(point.value));
        } else if (std::holds_alternative<uint64_t>(point.value)) {
            value = static_cast<double>(std::get<uint64_t>(point.value));
        }
        oss << "{\"value\":" << value << ",\"timestamp\":" 
            << std::chrono::duration_cast<std::chrono::milliseconds>(
                point.timestamp.time_since_epoch()).count() << "}";
    }
    oss << "]}";
    return oss.str();
}

std::string TimeSeries::to_influxdb() const {
    std::shared_lock lock(data_mutex_);
    std::ostringstream oss;
    for (const auto& point : data_points_) {
        oss << name_;
        if (!labels_.empty()) {
            bool first = true;
            for (const auto& label : labels_.labels()) {
                if (first) oss << ",";
                first = false;
                oss << label.first << "=" << label.second;
            }
        }
        oss << " ";
        
        double value = 0.0;
        if (std::holds_alternative<double>(point.value)) {
            value = std::get<double>(point.value);
        } else if (std::holds_alternative<int64_t>(point.value)) {
            value = static_cast<double>(std::get<int64_t>(point.value));
        } else if (std::holds_alternative<uint64_t>(point.value)) {
            value = static_cast<double>(std::get<uint64_t>(point.value));
        }
        
        oss << "value=" << value << " ";
        oss << std::chrono::duration_cast<std::chrono::nanoseconds>(
            point.timestamp.time_since_epoch()).count() << "\n";
    }
    return oss.str();
}

void TimeSeries::apply_sampling() {
    // 如果超过最大点数，移除最旧的点
    while (data_points_.size() > max_points_) {
        data_points_.pop_front();
    }
    
    // 移除过期的点
    auto cutoff = std::chrono::system_clock::now() - max_age_;
    auto it = data_points_.begin();
    while (it != data_points_.end() && it->timestamp < cutoff) {
        it = data_points_.erase(it);
    }
}

// ==================== CounterMetric 实现 ====================

CounterMetric::CounterMetric(const std::string& name, 
                             const std::string& description,
                             const MetricConfig& config)
    : descriptor_({name, description, MetricType::COUNTER, config.unit, {}, 
                   std::chrono::system_clock::now(), config}) {
}

void CounterMetric::inc(double value, const LabelSet& labels, 
                        std::map<std::string, std::string> metadata) {
    if (value < 0) return; // 计数器不能减少
    auto series = get_or_create_series(labels);
    auto last = series->last_point();
    double current = 0.0;
    if (last) {
        if (std::holds_alternative<double>(last->value)) {
            current = std::get<double>(last->value);
        } else if (std::holds_alternative<int64_t>(last->value)) {
            current = static_cast<double>(std::get<int64_t>(last->value));
        } else if (std::holds_alternative<uint64_t>(last->value)) {
            current = static_cast<double>(std::get<uint64_t>(last->value));
        }
    }
    series->record(MetricValue(current + value), metadata);
}

double CounterMetric::get(const LabelSet& labels) const {
    std::shared_lock lock(series_mutex_);
    auto it = time_series_.find(labels);
    if (it == time_series_.end()) {
        return 0.0;
    }
    auto last = it->second->last_point();
    if (!last) return 0.0;
    if (std::holds_alternative<double>(last->value)) {
        return std::get<double>(last->value);
    } else if (std::holds_alternative<int64_t>(last->value)) {
        return static_cast<double>(std::get<int64_t>(last->value));
    } else if (std::holds_alternative<uint64_t>(last->value)) {
        return static_cast<double>(std::get<uint64_t>(last->value));
    }
    return 0.0;
}

void CounterMetric::reset(const LabelSet& labels) {
    std::unique_lock lock(series_mutex_);
    time_series_.erase(labels);
}

std::vector<std::shared_ptr<TimeSeries>> CounterMetric::get_time_series() const {
    std::shared_lock lock(series_mutex_);
    std::vector<std::shared_ptr<TimeSeries>> result;
    for (const auto& pair : time_series_) {
        result.push_back(pair.second);
    }
    return result;
}

std::shared_ptr<TimeSeries> CounterMetric::get_time_series(const LabelSet& labels) {
    return get_or_create_series(labels);
}

std::vector<std::shared_ptr<TimeSeries>> CounterMetric::find_time_series(const LabelMatcher& matcher) const {
    std::shared_lock lock(series_mutex_);
    std::vector<std::shared_ptr<TimeSeries>> result;
    for (const auto& pair : time_series_) {
        if (matcher.matches(pair.first)) {
            result.push_back(pair.second);
        }
    }
    return result;
}

void CounterMetric::record(double value, const LabelSet& labels,
                          std::map<std::string, std::string> metadata) {
    inc(value, labels, metadata);
}

void CounterMetric::record_batch(const std::vector<double>& values,
                                const std::vector<LabelSet>& labels,
                                const std::vector<std::map<std::string, std::string>>& metadata) {
    for (size_t i = 0; i < values.size(); ++i) {
        LabelSet label_set = i < labels.size() ? labels[i] : LabelSet();
        std::map<std::string, std::string> meta = i < metadata.size() ? metadata[i] : std::map<std::string, std::string>();
        inc(values[i], label_set, meta);
    }
}

std::string CounterMetric::to_prometheus() const {
    std::ostringstream oss;
    oss << "# HELP " << descriptor_.name << " " << descriptor_.description << "\n";
    oss << "# TYPE " << descriptor_.name << " counter\n";
    
    std::shared_lock lock(series_mutex_);
    for (const auto& pair : time_series_) {
        oss << pair.second->to_prometheus();
    }
    return oss.str();
}

std::string CounterMetric::to_json() const {
    std::ostringstream oss;
    oss << "{\"name\":\"" << descriptor_.name << "\",\"type\":\"counter\",\"series\":[";
    std::shared_lock lock(series_mutex_);
    bool first = true;
    for (const auto& pair : time_series_) {
        if (!first) oss << ",";
        first = false;
        oss << pair.second->to_json();
    }
    oss << "]}";
    return oss.str();
}

std::string CounterMetric::to_influxdb() const {
    std::ostringstream oss;
    std::shared_lock lock(series_mutex_);
    for (const auto& pair : time_series_) {
        oss << pair.second->to_influxdb();
    }
    return oss.str();
}

size_t CounterMetric::cleanup(std::chrono::system_clock::time_point cutoff) {
    std::unique_lock lock(series_mutex_);
    size_t total = 0;
    for (auto& pair : time_series_) {
        total += pair.second->cleanup(cutoff);
    }
    return total;
}

void CounterMetric::reset() {
    std::unique_lock lock(series_mutex_);
    time_series_.clear();
}

std::shared_ptr<TimeSeries> CounterMetric::get_or_create_series(const LabelSet& labels) {
    std::unique_lock lock(series_mutex_);
    auto it = time_series_.find(labels);
    if (it != time_series_.end()) {
        return it->second;
    }
    auto series = std::make_shared<TimeSeries>(descriptor_.name, labels, descriptor_.config);
    time_series_[labels] = series;
    return series;
}

// ==================== GaugeMetric 实现 ====================

GaugeMetric::GaugeMetric(const std::string& name,
                         const std::string& description,
                         const MetricConfig& config)
    : descriptor_({name, description, MetricType::GAUGE, config.unit, {},
                   std::chrono::system_clock::now(), config}) {
}

void GaugeMetric::set(double value, const LabelSet& labels,
                     std::map<std::string, std::string> metadata) {
    auto series = get_or_create_series(labels);
    series->record(MetricValue(value), metadata);
}

void GaugeMetric::inc(double value, const LabelSet& labels,
                     std::map<std::string, std::string> metadata) {
    auto current = get(labels);
    set(current + value, labels, metadata);
}

void GaugeMetric::dec(double value, const LabelSet& labels,
                     std::map<std::string, std::string> metadata) {
    inc(-value, labels, metadata);
}

double GaugeMetric::get(const LabelSet& labels) const {
    std::shared_lock lock(series_mutex_);
    auto it = time_series_.find(labels);
    if (it == time_series_.end()) {
        return 0.0;
    }
    auto last = it->second->last_point();
    if (!last) return 0.0;
    if (std::holds_alternative<double>(last->value)) {
        return std::get<double>(last->value);
    } else if (std::holds_alternative<int64_t>(last->value)) {
        return static_cast<double>(std::get<int64_t>(last->value));
    } else if (std::holds_alternative<uint64_t>(last->value)) {
        return static_cast<double>(std::get<uint64_t>(last->value));
    }
    return 0.0;
}

std::vector<std::shared_ptr<TimeSeries>> GaugeMetric::get_time_series() const {
    std::shared_lock lock(series_mutex_);
    std::vector<std::shared_ptr<TimeSeries>> result;
    for (const auto& pair : time_series_) {
        result.push_back(pair.second);
    }
    return result;
}

std::shared_ptr<TimeSeries> GaugeMetric::get_time_series(const LabelSet& labels) {
    return get_or_create_series(labels);
}

std::vector<std::shared_ptr<TimeSeries>> GaugeMetric::find_time_series(const LabelMatcher& matcher) const {
    std::shared_lock lock(series_mutex_);
    std::vector<std::shared_ptr<TimeSeries>> result;
    for (const auto& pair : time_series_) {
        if (matcher.matches(pair.first)) {
            result.push_back(pair.second);
        }
    }
    return result;
}

void GaugeMetric::record(double value, const LabelSet& labels,
                        std::map<std::string, std::string> metadata) {
    set(value, labels, metadata);
}

void GaugeMetric::record_batch(const std::vector<double>& values,
                              const std::vector<LabelSet>& labels,
                              const std::vector<std::map<std::string, std::string>>& metadata) {
    for (size_t i = 0; i < values.size(); ++i) {
        LabelSet label_set = i < labels.size() ? labels[i] : LabelSet();
        std::map<std::string, std::string> meta = i < metadata.size() ? metadata[i] : std::map<std::string, std::string>();
        set(values[i], label_set, meta);
    }
}

std::string GaugeMetric::to_prometheus() const {
    std::ostringstream oss;
    oss << "# HELP " << descriptor_.name << " " << descriptor_.description << "\n";
    oss << "# TYPE " << descriptor_.name << " gauge\n";
    
    std::shared_lock lock(series_mutex_);
    for (const auto& pair : time_series_) {
        oss << pair.second->to_prometheus();
    }
    return oss.str();
}

std::string GaugeMetric::to_json() const {
    std::ostringstream oss;
    oss << "{\"name\":\"" << descriptor_.name << "\",\"type\":\"gauge\",\"series\":[";
    std::shared_lock lock(series_mutex_);
    bool first = true;
    for (const auto& pair : time_series_) {
        if (!first) oss << ",";
        first = false;
        oss << pair.second->to_json();
    }
    oss << "]}";
    return oss.str();
}

std::string GaugeMetric::to_influxdb() const {
    std::ostringstream oss;
    std::shared_lock lock(series_mutex_);
    for (const auto& pair : time_series_) {
        oss << pair.second->to_influxdb();
    }
    return oss.str();
}

size_t GaugeMetric::cleanup(std::chrono::system_clock::time_point cutoff) {
    std::unique_lock lock(series_mutex_);
    size_t total = 0;
    for (auto& pair : time_series_) {
        total += pair.second->cleanup(cutoff);
    }
    return total;
}

void GaugeMetric::reset() {
    std::unique_lock lock(series_mutex_);
    time_series_.clear();
}

std::shared_ptr<TimeSeries> GaugeMetric::get_or_create_series(const LabelSet& labels) {
    std::unique_lock lock(series_mutex_);
    auto it = time_series_.find(labels);
    if (it != time_series_.end()) {
        return it->second;
    }
    auto series = std::make_shared<TimeSeries>(descriptor_.name, labels, descriptor_.config);
    time_series_[labels] = series;
    return series;
}

// ==================== HistogramMetric 实现 ====================

HistogramMetric::HistogramMetric(const std::string& name,
                                 const std::string& description,
                                 const MetricConfig& config)
    : descriptor_({name, description, MetricType::HISTOGRAM, config.unit, {},
                   std::chrono::system_clock::now(), config}) {
    if (config.histogram_config) {
        histogram_config_ = *config.histogram_config;
    } else {
        histogram_config_ = HistogramConfig::default_config();
    }
}

void HistogramMetric::observe(double value, const LabelSet& labels,
                             std::map<std::string, std::string> metadata) {
    auto buckets = histogram_config_.get_buckets();
    
    std::unique_lock bucket_lock(bucket_mutex_);
    auto& bucket_counts = bucket_data_[labels];
    if (!bucket_counts) {
        bucket_counts = std::make_shared<BucketCounts>();
        size_t size = buckets.size() + 1; // +1 for +Inf
        // std::atomic 不能移动构造，需要直接构造
        bucket_counts->counts = std::vector<std::atomic<uint64_t>>(size);
        for (auto& count : bucket_counts->counts) {
            count.store(0);
        }
    }
    
    // std::atomic<double> 没有 fetch_add，使用 compare_exchange_weak 实现原子加法
    double old_sum = bucket_counts->sum.load();
    while (!bucket_counts->sum.compare_exchange_weak(old_sum, old_sum + value)) {
        // 如果 compare_exchange_weak 失败，old_sum 会被更新为当前值，重试
    }
    bucket_counts->total.fetch_add(1);
    
    // 更新桶计数
    size_t bucket_index = buckets.size(); // +Inf bucket
    for (size_t i = 0; i < buckets.size(); ++i) {
        if (value <= buckets[i]) {
            bucket_index = i;
            break;
        }
    }
    bucket_counts->counts[bucket_index].fetch_add(1);
    
    // 也记录到时间序列
    auto series = get_or_create_series(labels);
    series->record(MetricValue(value), metadata);
}

HistogramMetric::HistogramStatistics HistogramMetric::get_statistics(const LabelSet& labels) const {
    HistogramStatistics stats;
    auto buckets = histogram_config_.get_buckets();
    
    std::shared_lock bucket_lock(bucket_mutex_);
    auto it = bucket_data_.find(labels);
    if (it == bucket_data_.end()) {
        return stats;
    }
    
    auto& bucket_counts = it->second;
    stats.count = bucket_counts->total.load();
    stats.sum = bucket_counts->sum.load();
    
    if (stats.count > 0) {
        stats.mean = stats.sum / stats.count;
        
        // 从时间序列获取min/max
        // 注意：get_time_series 是非 const 的，但我们在 const 方法中，需要先查找
        std::shared_lock series_lock(series_mutex_);
        auto series_it = time_series_.find(labels);
        if (series_it != time_series_.end()) {
            auto series = series_it->second;
            series_lock.unlock();
            auto series_stats = series->statistics(
                std::chrono::system_clock::time_point::min(),
                std::chrono::system_clock::time_point::max());
            stats.min = series_stats.min;
            stats.max = series_stats.max;
        }
        
        // 填充桶计数
        for (size_t i = 0; i < buckets.size(); ++i) {
            stats.buckets[buckets[i]] = bucket_counts->counts[i].load();
        }
        stats.buckets[std::numeric_limits<double>::infinity()] = 
            bucket_counts->counts[buckets.size()].load();
    }
    
    return stats;
}

std::vector<std::shared_ptr<TimeSeries>> HistogramMetric::get_time_series() const {
    std::shared_lock lock(series_mutex_);
    std::vector<std::shared_ptr<TimeSeries>> result;
    for (const auto& pair : time_series_) {
        result.push_back(pair.second);
    }
    return result;
}

std::shared_ptr<TimeSeries> HistogramMetric::get_time_series(const LabelSet& labels) {
    return get_or_create_series(labels);
}

std::vector<std::shared_ptr<TimeSeries>> HistogramMetric::find_time_series(const LabelMatcher& matcher) const {
    std::shared_lock lock(series_mutex_);
    std::vector<std::shared_ptr<TimeSeries>> result;
    for (const auto& pair : time_series_) {
        if (matcher.matches(pair.first)) {
            result.push_back(pair.second);
        }
    }
    return result;
}

void HistogramMetric::record(double value, const LabelSet& labels,
                            std::map<std::string, std::string> metadata) {
    observe(value, labels, metadata);
}

void HistogramMetric::record_batch(const std::vector<double>& values,
                                  const std::vector<LabelSet>& labels,
                                  const std::vector<std::map<std::string, std::string>>& metadata) {
    for (size_t i = 0; i < values.size(); ++i) {
        LabelSet label_set = i < labels.size() ? labels[i] : LabelSet();
        std::map<std::string, std::string> meta = i < metadata.size() ? metadata[i] : std::map<std::string, std::string>();
        observe(values[i], label_set, meta);
    }
}

std::string HistogramMetric::to_prometheus() const {
    std::ostringstream oss;
    oss << "# HELP " << descriptor_.name << " " << descriptor_.description << "\n";
    oss << "# TYPE " << descriptor_.name << " histogram\n";
    
    auto buckets = histogram_config_.get_buckets();
    std::shared_lock lock(series_mutex_);
    std::shared_lock bucket_lock(bucket_mutex_);
    
    for (const auto& series_pair : time_series_) {
        const auto& labels = series_pair.first;
        auto stats = get_statistics(labels);
        
        // 输出桶
        uint64_t cumulative = 0;
        for (double bucket : buckets) {
            cumulative += stats.buckets[bucket];
            oss << descriptor_.name << "_bucket{";
            bool first = true;
            for (const auto& label : labels.labels()) {
                if (!first) oss << ",";
                first = false;
                oss << label.first << "=\"" << label.second << "\"";
            }
            if (!first) oss << ",";
            oss << "le=\"" << bucket << "\"} " << cumulative << "\n";
        }
        
        // +Inf桶
        cumulative += stats.buckets[std::numeric_limits<double>::infinity()];
        oss << descriptor_.name << "_bucket{";
        bool first = true;
        for (const auto& label : labels.labels()) {
            if (!first) oss << ",";
            first = false;
            oss << label.first << "=\"" << label.second << "\"";
        }
        if (!first) oss << ",";
        oss << "le=\"+Inf\"} " << cumulative << "\n";
        
        // count
        oss << descriptor_.name << "_count{";
        first = true;
        for (const auto& label : labels.labels()) {
            if (!first) oss << ",";
            first = false;
            oss << label.first << "=\"" << label.second << "\"";
        }
        oss << "} " << stats.count << "\n";
        
        // sum
        oss << descriptor_.name << "_sum{";
        first = true;
        for (const auto& label : labels.labels()) {
            if (!first) oss << ",";
            first = false;
            oss << label.first << "=\"" << label.second << "\"";
        }
        oss << "} " << std::fixed << std::setprecision(6) << stats.sum << "\n";
    }
    
    return oss.str();
}

std::string HistogramMetric::to_json() const {
    std::ostringstream oss;
    oss << "{\"name\":\"" << descriptor_.name << "\",\"type\":\"histogram\",\"series\":[";
    std::shared_lock lock(series_mutex_);
    bool first = true;
    for (const auto& pair : time_series_) {
        if (!first) oss << ",";
        first = false;
        oss << pair.second->to_json();
    }
    oss << "]}";
    return oss.str();
}

std::string HistogramMetric::to_influxdb() const {
    std::ostringstream oss;
    std::shared_lock lock(series_mutex_);
    for (const auto& pair : time_series_) {
        oss << pair.second->to_influxdb();
    }
    return oss.str();
}

size_t HistogramMetric::cleanup(std::chrono::system_clock::time_point cutoff) {
    std::unique_lock lock(series_mutex_);
    size_t total = 0;
    for (auto& pair : time_series_) {
        total += pair.second->cleanup(cutoff);
    }
    return total;
}

void HistogramMetric::reset() {
    std::unique_lock lock(series_mutex_);
    std::unique_lock bucket_lock(bucket_mutex_);
    time_series_.clear();
    bucket_data_.clear();
}

std::shared_ptr<TimeSeries> HistogramMetric::get_or_create_series(const LabelSet& labels) {
    std::unique_lock lock(series_mutex_);
    auto it = time_series_.find(labels);
    if (it != time_series_.end()) {
        return it->second;
    }
    auto series = std::make_shared<TimeSeries>(descriptor_.name, labels, descriptor_.config);
    time_series_[labels] = series;
    return series;
}

std::optional<double> HistogramMetric::HistogramStatistics::bucket_for(double value) const {
    for (const auto& pair : buckets) {
        if (value <= pair.first) {
            return pair.first;
        }
    }
    return std::nullopt;
}

// ==================== SummaryMetric 实现 ====================

SummaryMetric::SummaryMetric(const std::string& name,
                             const std::string& description,
                             const MetricConfig& config)
    : descriptor_({name, description, MetricType::SUMMARY, config.unit, {},
                   std::chrono::system_clock::now(), config}) {
    if (config.summary_config) {
        summary_config_ = *config.summary_config;
    } else {
        summary_config_ = SummaryConfig::default_config();
    }
}

void SummaryMetric::observe(double value, const LabelSet& labels,
                           std::map<std::string, std::string> metadata) {
#ifndef METRICS_USE_TDIGEST
    std::unique_lock sample_lock(sample_mutex_);
    auto& sample_data = sample_data_[labels];
    if (!sample_data) {
        sample_data = std::make_shared<SampleData>();
    }
    sample_data->add_sample(value);
    
    // 也记录到时间序列
    auto series = get_or_create_series(labels);
    series->record(MetricValue(value), metadata);
#endif
}

double SummaryMetric::quantile(double q, const LabelSet& labels) const {
#ifndef METRICS_USE_TDIGEST
    std::shared_lock sample_lock(sample_mutex_);
    auto it = sample_data_.find(labels);
    if (it == sample_data_.end()) {
        return 0.0;
    }
    return it->second->get_quantile(q);
#else
    return 0.0;
#endif
}

std::map<double, double> SummaryMetric::quantiles(const std::vector<double>& qs, const LabelSet& labels) const {
    std::map<double, double> result;
    for (double q : qs) {
        result[q] = quantile(q, labels);
    }
    return result;
}

SummaryMetric::SummaryStatistics SummaryMetric::get_statistics(const LabelSet& labels) const {
    SummaryStatistics stats;
#ifndef METRICS_USE_TDIGEST
    std::shared_lock sample_lock(sample_mutex_);
    auto it = sample_data_.find(labels);
    if (it == sample_data_.end()) {
        return stats;
    }
    
    auto& sample_data = it->second;
    stats.count = sample_data->count.load();
    stats.sum = sample_data->sum.load();
    
    if (stats.count > 0) {
        stats.mean = stats.sum / stats.count;
        
        std::lock_guard<std::mutex> lock(sample_data->mutex_);
        if (!sample_data->samples.empty()) {
            std::vector<double> sorted = sample_data->samples;
            std::sort(sorted.begin(), sorted.end());
            stats.min = sorted.front();
            stats.max = sorted.back();
            
            for (double q : summary_config_.quantiles) {
                size_t index = static_cast<size_t>(q * (sorted.size() - 1));
                if (index >= sorted.size()) index = sorted.size() - 1;
                stats.quantiles[q] = sorted[index];
            }
        }
    }
#endif
    return stats;
}

std::vector<std::shared_ptr<TimeSeries>> SummaryMetric::get_time_series() const {
    std::shared_lock lock(series_mutex_);
    std::vector<std::shared_ptr<TimeSeries>> result;
    for (const auto& pair : time_series_) {
        result.push_back(pair.second);
    }
    return result;
}

std::shared_ptr<TimeSeries> SummaryMetric::get_time_series(const LabelSet& labels) {
    return get_or_create_series(labels);
}

std::vector<std::shared_ptr<TimeSeries>> SummaryMetric::find_time_series(const LabelMatcher& matcher) const {
    std::shared_lock lock(series_mutex_);
    std::vector<std::shared_ptr<TimeSeries>> result;
    for (const auto& pair : time_series_) {
        if (matcher.matches(pair.first)) {
            result.push_back(pair.second);
        }
    }
    return result;
}

void SummaryMetric::record(double value, const LabelSet& labels,
                           std::map<std::string, std::string> metadata) {
    observe(value, labels, metadata);
}

void SummaryMetric::record_batch(const std::vector<double>& values,
                                const std::vector<LabelSet>& labels,
                                const std::vector<std::map<std::string, std::string>>& metadata) {
    for (size_t i = 0; i < values.size(); ++i) {
        LabelSet label_set = i < labels.size() ? labels[i] : LabelSet();
        std::map<std::string, std::string> meta = i < metadata.size() ? metadata[i] : std::map<std::string, std::string>();
        observe(values[i], label_set, meta);
    }
}

std::string SummaryMetric::to_prometheus() const {
    std::ostringstream oss;
    oss << "# HELP " << descriptor_.name << " " << descriptor_.description << "\n";
    oss << "# TYPE " << descriptor_.name << " summary\n";
    
    std::shared_lock lock(series_mutex_);
#ifndef METRICS_USE_TDIGEST
    std::shared_lock sample_lock(sample_mutex_);
#endif
    
    for (const auto& series_pair : time_series_) {
        const auto& labels = series_pair.first;
        auto stats = get_statistics(labels);
        
        // 输出分位数
        for (const auto& q_pair : stats.quantiles) {
            oss << descriptor_.name << "{";
            bool first = true;
            for (const auto& label : labels.labels()) {
                if (!first) oss << ",";
                first = false;
                oss << label.first << "=\"" << label.second << "\"";
            }
            if (!first) oss << ",";
            oss << "quantile=\"" << q_pair.first << "\"} " 
                << std::fixed << std::setprecision(6) << q_pair.second << "\n";
        }
        
        // count
        oss << descriptor_.name << "_count{";
        bool first = true;
        for (const auto& label : labels.labels()) {
            if (!first) oss << ",";
            first = false;
            oss << label.first << "=\"" << label.second << "\"";
        }
        oss << "} " << stats.count << "\n";
        
        // sum
        oss << descriptor_.name << "_sum{";
        first = true;
        for (const auto& label : labels.labels()) {
            if (!first) oss << ",";
            first = false;
            oss << label.first << "=\"" << label.second << "\"";
        }
        oss << "} " << std::fixed << std::setprecision(6) << stats.sum << "\n";
    }
    
    return oss.str();
}

std::string SummaryMetric::to_json() const {
    std::ostringstream oss;
    oss << "{\"name\":\"" << descriptor_.name << "\",\"type\":\"summary\",\"series\":[";
    std::shared_lock lock(series_mutex_);
    bool first = true;
    for (const auto& pair : time_series_) {
        if (!first) oss << ",";
        first = false;
        oss << pair.second->to_json();
    }
    oss << "]}";
    return oss.str();
}

std::string SummaryMetric::to_influxdb() const {
    std::ostringstream oss;
    std::shared_lock lock(series_mutex_);
    for (const auto& pair : time_series_) {
        oss << pair.second->to_influxdb();
    }
    return oss.str();
}

size_t SummaryMetric::cleanup(std::chrono::system_clock::time_point cutoff) {
    std::unique_lock lock(series_mutex_);
    size_t total = 0;
    for (auto& pair : time_series_) {
        total += pair.second->cleanup(cutoff);
    }
    return total;
}

void SummaryMetric::reset() {
    std::unique_lock lock(series_mutex_);
#ifndef METRICS_USE_TDIGEST
    std::unique_lock sample_lock(sample_mutex_);
    sample_data_.clear();
#endif
    time_series_.clear();
}

std::shared_ptr<TimeSeries> SummaryMetric::get_or_create_series(const LabelSet& labels) {
    std::unique_lock lock(series_mutex_);
    auto it = time_series_.find(labels);
    if (it != time_series_.end()) {
        return it->second;
    }
    auto series = std::make_shared<TimeSeries>(descriptor_.name, labels, descriptor_.config);
    time_series_[labels] = series;
    return series;
}

#ifndef METRICS_USE_TDIGEST
void SummaryMetric::SampleData::add_sample(double value) {
    std::lock_guard<std::mutex> lock(mutex_);
    samples.push_back(value);
    // std::atomic<double> 没有 fetch_add，使用 load/store 实现原子加法
    double old_sum = sum.load();
    while (!sum.compare_exchange_weak(old_sum, old_sum + value)) {
        // 如果 compare_exchange_weak 失败，old_sum 会被更新为当前值，重试
    }
    count.fetch_add(1);
    
    // 限制样本数量
    if (samples.size() > 1000) {
        samples.erase(samples.begin(), samples.begin() + (samples.size() - 1000));
    }
}

double SummaryMetric::SampleData::get_quantile(double q) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (samples.empty()) {
        return 0.0;
    }
    
    std::vector<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    
    size_t index = static_cast<size_t>(q * (sorted.size() - 1));
    if (index >= sorted.size()) {
        index = sorted.size() - 1;
    }
    
    return sorted[index];
}
#endif

// ==================== MetricRegistry 实现 ====================

std::shared_ptr<MetricRegistry> MetricRegistry::create() {
    return std::shared_ptr<MetricRegistry>(new MetricRegistry());
}

MetricRegistry::MetricRegistry() {
    init_factories();
}

void MetricRegistry::init_factories() {
    metric_factories_[MetricType::COUNTER] = [](const std::string& name, const std::string& desc, const MetricConfig& config) {
        return std::static_pointer_cast<IMetric>(std::make_shared<CounterMetric>(name, desc, config));
    };
    
    metric_factories_[MetricType::GAUGE] = [](const std::string& name, const std::string& desc, const MetricConfig& config) {
        return std::static_pointer_cast<IMetric>(std::make_shared<GaugeMetric>(name, desc, config));
    };
    
    metric_factories_[MetricType::HISTOGRAM] = [](const std::string& name, const std::string& desc, const MetricConfig& config) {
        return std::static_pointer_cast<IMetric>(std::make_shared<HistogramMetric>(name, desc, config));
    };
    
    metric_factories_[MetricType::SUMMARY] = [](const std::string& name, const std::string& desc, const MetricConfig& config) {
        return std::static_pointer_cast<IMetric>(std::make_shared<SummaryMetric>(name, desc, config));
    };
}

std::shared_ptr<MetricNamespace> MetricRegistry::create_namespace(const std::string& name, const std::string& description) {
    std::unique_lock lock(metrics_mutex_);
    auto ns = std::make_shared<MetricNamespace>(name, description);
    namespaces_[name] = ns;
    return ns;
}

std::shared_ptr<IMetric> MetricRegistry::register_metric(const std::string& name,
                                                          const std::string& description,
                                                          MetricType type,
                                                          const MetricConfig& config) {
    std::unique_lock lock(metrics_mutex_);
    auto it = metrics_.find(name);
    if (it != metrics_.end()) {
        return it->second; // 已存在，返回现有指标
    }
    
    auto factory_it = metric_factories_.find(type);
    if (factory_it == metric_factories_.end()) {
        return nullptr;
    }
    
    MetricConfig final_config = config;
    if (final_config.type == MetricType::GAUGE && final_config.unit.empty()) {
        final_config = default_config_;
    }
    
    auto metric = factory_it->second(name, description, final_config);
    metrics_[name] = metric;
    return metric;
}

std::shared_ptr<IMetric> MetricRegistry::get_metric(const std::string& name) const {
    std::shared_lock lock(metrics_mutex_);
    auto it = metrics_.find(name);
    return it != metrics_.end() ? it->second : nullptr;
}

std::vector<std::shared_ptr<IMetric>> MetricRegistry::get_all_metrics() const {
    std::shared_lock lock(metrics_mutex_);
    std::vector<std::shared_ptr<IMetric>> result;
    result.reserve(metrics_.size());
    for (const auto& pair : metrics_) {
        result.push_back(pair.second);
    }
    return result;
}

std::vector<std::shared_ptr<IMetric>> MetricRegistry::find_metrics(const LabelMatcher& matcher) const {
    std::shared_lock lock(metrics_mutex_);
    std::vector<std::shared_ptr<IMetric>> result;
    for (const auto& pair : metrics_) {
        auto series_list = pair.second->find_time_series(matcher);
        if (!series_list.empty()) {
            result.push_back(pair.second);
        }
    }
    return result;
}

bool MetricRegistry::unregister_metric(const std::string& name) {
    std::unique_lock lock(metrics_mutex_);
    return metrics_.erase(name) > 0;
}

std::string MetricRegistry::to_prometheus() const {
    std::shared_lock lock(metrics_mutex_);
    std::ostringstream oss;
    for (const auto& pair : metrics_) {
        oss << pair.second->to_prometheus() << "\n";
    }
    return oss.str();
}

std::string MetricRegistry::to_json() const {
    std::shared_lock lock(metrics_mutex_);
    std::ostringstream oss;
    oss << "{\"metrics\":[";
    bool first = true;
    for (const auto& pair : metrics_) {
        if (!first) oss << ",";
        first = false;
        oss << pair.second->to_json();
    }
    oss << "]}";
    return oss.str();
}

std::string MetricRegistry::to_influxdb() const {
    std::shared_lock lock(metrics_mutex_);
    std::ostringstream oss;
    for (const auto& pair : metrics_) {
        oss << pair.second->to_influxdb();
    }
    return oss.str();
}

size_t MetricRegistry::cleanup(std::chrono::system_clock::time_point cutoff) {
    std::unique_lock lock(metrics_mutex_);
    size_t total = 0;
    for (auto& pair : metrics_) {
        total += pair.second->cleanup(cutoff);
    }
    return total;
}

void MetricRegistry::reset_all() {
    std::unique_lock lock(metrics_mutex_);
    for (auto& pair : metrics_) {
        pair.second->reset();
    }
}

MetricRegistry::RegistryStatistics MetricRegistry::get_statistics() const {
    std::shared_lock lock(metrics_mutex_);
    RegistryStatistics stats;
    stats.total_metrics = metrics_.size();
    
    for (const auto& pair : metrics_) {
        switch (pair.second->type()) {
            case MetricType::COUNTER:
                stats.counters++;
                break;
            case MetricType::GAUGE:
                stats.gauges++;
                break;
            case MetricType::HISTOGRAM:
                stats.histograms++;
                break;
            case MetricType::SUMMARY:
                stats.summaries++;
                break;
            default:
                stats.other_types++;
                break;
        }
        
        auto series_list = pair.second->get_time_series();
        stats.total_time_series += series_list.size();
    }
    
    return stats;
}

void MetricRegistry::set_default_config(const MetricConfig& config) {
    std::unique_lock lock(metrics_mutex_);
    default_config_ = config;
}

void MetricRegistry::set_auto_cleanup_enabled(bool enabled) {
    auto_cleanup_enabled_ = enabled;
}

void MetricRegistry::set_cleanup_interval(std::chrono::seconds interval) {
    cleanup_interval_ = interval;
}

void MetricRegistry::start_auto_cleanup() {
    if (cleanup_running_.exchange(true)) {
        return;
    }
    cleanup_thread_ = std::thread(&MetricRegistry::cleanup_loop, this);
}

void MetricRegistry::stop_auto_cleanup() {
    if (!cleanup_running_.exchange(false)) {
        return;
    }
    if (cleanup_thread_.joinable()) {
        cleanup_thread_.join();
    }
}

void MetricRegistry::cleanup_loop() {
    while (cleanup_running_.load()) {
        if (auto_cleanup_enabled_) {
            auto cutoff = std::chrono::system_clock::now() - std::chrono::hours(1);
            cleanup(cutoff);
        }
        std::this_thread::sleep_for(cleanup_interval_);
    }
}

// ==================== MetricsManager 实现 ====================

std::shared_ptr<MetricRegistry> MetricsManager::global_registry_ = nullptr;
std::shared_ptr<MetricsManager::SystemMetricsCollector> MetricsManager::system_collector_ = nullptr;
std::vector<std::shared_ptr<MetricsManager::MetricsExporter>> MetricsManager::exporters_;
std::once_flag MetricsManager::init_flag_;
std::mutex MetricsManager::exporters_mutex_;

void MetricsManager::initialize() {
    std::call_once(init_flag_, []() {
        global_registry_ = MetricRegistry::create();
    });
}

void MetricsManager::shutdown() {
    stop_all_exporters();
    if (system_collector_) {
        system_collector_->stop();
    }
    global_registry_ = nullptr;
    system_collector_ = nullptr;
}

std::shared_ptr<MetricRegistry> MetricsManager::global_registry() {
    initialize();
    return global_registry_;
}

std::shared_ptr<MetricRegistry> MetricsManager::create_registry() {
    return MetricRegistry::create();
}

std::shared_ptr<CounterMetric> MetricsManager::create_counter(const std::string& name,
                                                               const std::string& description,
                                                               const MetricConfig& config) {
    auto registry = global_registry();
    auto metric = registry->register_metric(name, description, MetricType::COUNTER, config);
    return std::static_pointer_cast<CounterMetric>(metric);
}

std::shared_ptr<GaugeMetric> MetricsManager::create_gauge(const std::string& name,
                                                           const std::string& description,
                                                           const MetricConfig& config) {
    auto registry = global_registry();
    auto metric = registry->register_metric(name, description, MetricType::GAUGE, config);
    return std::static_pointer_cast<GaugeMetric>(metric);
}

std::shared_ptr<HistogramMetric> MetricsManager::create_histogram(const std::string& name,
                                                                  const std::string& description,
                                                                  const MetricConfig& config) {
    auto registry = global_registry();
    auto metric = registry->register_metric(name, description, MetricType::HISTOGRAM, config);
    return std::static_pointer_cast<HistogramMetric>(metric);
}

std::shared_ptr<SummaryMetric> MetricsManager::create_summary(const std::string& name,
                                                               const std::string& description,
                                                               const MetricConfig& config) {
    auto registry = global_registry();
    auto metric = registry->register_metric(name, description, MetricType::SUMMARY, config);
    return std::static_pointer_cast<SummaryMetric>(metric);
}

std::shared_ptr<MetricsManager::SystemMetricsCollector> MetricsManager::system_metrics_collector() {
    initialize();
    if (!system_collector_) {
        system_collector_ = std::make_shared<SystemMetricsCollector>(global_registry_);
    }
    return system_collector_;
}

void MetricsManager::register_exporter(std::shared_ptr<MetricsExporter> exporter) {
    std::lock_guard<std::mutex> lock(exporters_mutex_);
    exporters_.push_back(exporter);
}

void MetricsManager::unregister_exporter(const std::string& name) {
    std::lock_guard<std::mutex> lock(exporters_mutex_);
    exporters_.erase(
        std::remove_if(exporters_.begin(), exporters_.end(),
                      [&name](const auto& exp) { return exp->name() == name; }),
        exporters_.end());
}

void MetricsManager::start_all_exporters() {
    std::lock_guard<std::mutex> lock(exporters_mutex_);
    for (auto& exporter : exporters_) {
        exporter->start();
    }
}

void MetricsManager::stop_all_exporters() {
    std::lock_guard<std::mutex> lock(exporters_mutex_);
    for (auto& exporter : exporters_) {
        exporter->stop();
    }
}

// ==================== SystemMetricsCollector 实现 ====================

MetricsManager::SystemMetricsCollector::SystemMetricsCollector(std::shared_ptr<MetricRegistry> registry)
    : registry_(registry) {
    cpu_usage_ = std::static_pointer_cast<GaugeMetric>(
        registry->register_metric("system_cpu_usage_percent", "CPU usage percentage", MetricType::GAUGE));
    memory_usage_ = std::static_pointer_cast<GaugeMetric>(
        registry->register_metric("system_memory_usage_bytes", "Memory usage in bytes", MetricType::GAUGE));
    open_files_ = std::static_pointer_cast<GaugeMetric>(
        registry->register_metric("system_open_files", "Number of open file descriptors", MetricType::GAUGE));
}

MetricsManager::SystemMetricsCollector::~SystemMetricsCollector() {
    stop();
}

void MetricsManager::SystemMetricsCollector::start() {
    if (running_.exchange(true)) {
        return;
    }
    collection_thread_ = std::thread(&SystemMetricsCollector::collection_loop, this);
}

void MetricsManager::SystemMetricsCollector::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (collection_thread_.joinable()) {
        collection_thread_.join();
    }
}

void MetricsManager::SystemMetricsCollector::collection_loop() {
    while (running_.load()) {
        collect_cpu_usage();
        collect_memory_usage();
        collect_process_stats();
        std::this_thread::sleep_for(interval_);
    }
}

void MetricsManager::SystemMetricsCollector::collect_cpu_usage() {
    // 简化实现
    cpu_usage_->set(0.0, LabelSet{});
}

void MetricsManager::SystemMetricsCollector::collect_memory_usage() {
    // 简化实现
    memory_usage_->set(0.0, LabelSet{});
}

void MetricsManager::SystemMetricsCollector::collect_disk_usage() {
    // 待实现
}

void MetricsManager::SystemMetricsCollector::collect_network_io() {
    // 待实现
}

void MetricsManager::SystemMetricsCollector::collect_process_stats() {
    // 简化实现
    open_files_->set(0.0, LabelSet{});
}

void MetricsManager::SystemMetricsCollector::set_collection_interval(std::chrono::seconds interval) {
    interval_ = interval;
}

void MetricsManager::SystemMetricsCollector::set_enabled_metrics(const std::vector<std::string>& metrics) {
    // 待实现
}

// ==================== PrometheusExporter 实现 ====================

MetricsManager::PrometheusExporter::PrometheusExporter(const std::string& endpoint, uint16_t port)
    : endpoint_(endpoint), port_(port) {
}

void MetricsManager::PrometheusExporter::export_metrics(const std::shared_ptr<MetricRegistry>& registry) {
    // 简化实现：只导出Prometheus格式
    (void)registry;
}

void MetricsManager::PrometheusExporter::start() {
    running_.store(true);
}

void MetricsManager::PrometheusExporter::stop() {
    running_.store(false);
}

// ==================== InfluxDBExporter 实现 ====================

MetricsManager::InfluxDBExporter::InfluxDBExporter(const std::string& url,
                                                    const std::string& database,
                                                    const std::string& username,
                                                    const std::string& password)
    : url_(url), database_(database), username_(username), password_(password) {
}

void MetricsManager::InfluxDBExporter::export_metrics(const std::shared_ptr<MetricRegistry>& registry) {
    // 简化实现
    (void)registry;
}

void MetricsManager::InfluxDBExporter::start() {
    running_.store(true);
}

void MetricsManager::InfluxDBExporter::stop() {
    running_.store(false);
}

void MetricsManager::InfluxDBExporter::set_batch_size(size_t size) {
    batch_size_ = size;
}

void MetricsManager::InfluxDBExporter::set_flush_interval(std::chrono::seconds interval) {
    flush_interval_ = interval;
}

void MetricsManager::InfluxDBExporter::export_loop() {
    while (running_.load()) {
        // 导出逻辑
        std::this_thread::sleep_for(flush_interval_);
    }
}

// ==================== MetricsMiddleware 实现 ====================

MetricsManager::MetricsMiddleware::RequestMetrics 
MetricsManager::MetricsMiddleware::create_http_metrics(const std::string& service_name) {
    RequestMetrics metrics;
    auto registry = MetricsManager::global_registry();
    metrics.request_counter = std::static_pointer_cast<CounterMetric>(
        registry->register_metric(service_name + "_requests_total", "Total HTTP requests", MetricType::COUNTER));
    metrics.request_duration = std::static_pointer_cast<HistogramMetric>(
        registry->register_metric(service_name + "_request_duration_seconds", "HTTP request duration", MetricType::HISTOGRAM));
    metrics.error_counter = std::static_pointer_cast<CounterMetric>(
        registry->register_metric(service_name + "_errors_total", "Total HTTP errors", MetricType::COUNTER));
    return metrics;
}

MetricsManager::MetricsMiddleware::HttpMiddleware::HttpMiddleware(const RequestMetrics& metrics)
    : metrics_(metrics) {
}

std::function<void(const std::string&, const std::string&, uint64_t)> 
MetricsManager::MetricsMiddleware::HttpMiddleware::get_handler() {
    return [this](const std::string& method, const std::string& path, uint64_t duration_ms) {
        LabelSet labels({{"method", method}, {"path", path}});
        metrics_.request_counter->inc(1.0, labels);
        metrics_.request_duration->observe(duration_ms / 1000.0, labels);
    };
}

} // namespace utils
} // namespace mementodb


