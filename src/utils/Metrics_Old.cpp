// File: src/utils/Metrics.cpp

#include "Metrics.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <fstream>

#ifdef _WIN32
    #include <windows.h>
    #include <psapi.h>
#else
    #include <unistd.h>
    #include <sys/resource.h>
    #include <sys/stat.h>
    #include <dirent.h>
#endif

namespace mementodb {
namespace utils {

// ==================== Counter 实现 ====================

Counter::Counter(const std::string& name, const std::string& help, const Labels& labels)
    : name_(name), help_(help), labels_(labels), value_(0.0) {
}

void Counter::inc(double value) {
    if (value < 0) {
        return; // 计数器不能减少
    }
    double old = value_.load();
    double expected;
    do {
        expected = old;
        old = value_.compare_exchange_weak(expected, expected + value);
    } while (old != expected);
}

MetricValue Counter::get_value() const {
    return value_.load();
}

std::vector<MetricSample> Counter::get_samples() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {{get_value(), std::chrono::system_clock::now(), labels_}};
}

std::string Counter::to_prometheus() const {
    std::ostringstream oss;
    
    // HELP行
    if (!help_.empty()) {
        oss << "# HELP " << name_ << " " << help_ << "\n";
    }
    
    // TYPE行
    oss << "# TYPE " << name_ << " counter\n";
    
    // 指标行
    oss << name_;
    if (!labels_.empty()) {
        oss << "{";
        bool first = true;
        for (const auto& label : labels_) {
            if (!first) oss << ",";
            first = false;
            oss << label.first << "=\"" << label.second << "\"";
        }
        oss << "}";
    }
    oss << " " << std::fixed << std::setprecision(6) << get_value() << "\n";
    
    return oss.str();
}

void Counter::reset() {
    value_.store(0.0);
}

// ==================== Gauge 实现 ====================

Gauge::Gauge(const std::string& name, const std::string& help, const Labels& labels)
    : name_(name), help_(help), labels_(labels), value_(0.0) {
}

void Gauge::set(double value) {
    value_.store(value);
}

void Gauge::inc(double value) {
    double old = value_.load();
    double expected;
    do {
        expected = old;
        old = value_.compare_exchange_weak(expected, expected + value);
    } while (old != expected);
}

void Gauge::dec(double value) {
    inc(-value);
}

MetricValue Gauge::get_value() const {
    return value_.load();
}

std::vector<MetricSample> Gauge::get_samples() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {{get_value(), std::chrono::system_clock::now(), labels_}};
}

std::string Gauge::to_prometheus() const {
    std::ostringstream oss;
    
    if (!help_.empty()) {
        oss << "# HELP " << name_ << " " << help_ << "\n";
    }
    oss << "# TYPE " << name_ << " gauge\n";
    
    oss << name_;
    if (!labels_.empty()) {
        oss << "{";
        bool first = true;
        for (const auto& label : labels_) {
            if (!first) oss << ",";
            first = false;
            oss << label.first << "=\"" << label.second << "\"";
        }
        oss << "}";
    }
    oss << " " << std::fixed << std::setprecision(6) << get_value() << "\n";
    
    return oss.str();
}

void Gauge::reset() {
    value_.store(0.0);
}

// ==================== Histogram 实现 ====================

Histogram::Histogram(const std::string& name, 
                     const std::vector<double>& buckets,
                     const std::string& help,
                     const Labels& labels)
    : name_(name), help_(help), labels_(labels), buckets_(buckets),
      count_(0), sum_(0.0) {
    // 确保桶是排序的
    std::sort(buckets_.begin(), buckets_.end());
    // 初始化桶计数器
    bucket_counts_.resize(buckets_.size() + 1); // +1 for +Inf bucket
    for (auto& count : bucket_counts_) {
        count.store(0);
    }
}

void Histogram::observe(double value) {
    count_.fetch_add(1);
    
    double old_sum = sum_.load();
    double expected;
    do {
        expected = old_sum;
        old_sum = sum_.compare_exchange_weak(expected, expected + value);
    } while (old_sum != expected);
    
    // 更新桶计数
    size_t bucket_index = buckets_.size(); // +Inf bucket
    for (size_t i = 0; i < buckets_.size(); ++i) {
        if (value <= buckets_[i]) {
            bucket_index = i;
            break;
        }
    }
    bucket_counts_[bucket_index].fetch_add(1);
}

MetricValue Histogram::get_value() const {
    return sum_.load();
}

std::vector<HistogramBucket> Histogram::get_buckets() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<HistogramBucket> result;
    
    for (size_t i = 0; i < buckets_.size(); ++i) {
        result.push_back({buckets_[i], bucket_counts_[i].load()});
    }
    // +Inf bucket
    result.push_back({std::numeric_limits<double>::infinity(), 
                     bucket_counts_[buckets_.size()].load()});
    
    return result;
}

uint64_t Histogram::get_count() const {
    return count_.load();
}

double Histogram::get_sum() const {
    return sum_.load();
}

std::vector<MetricSample> Histogram::get_samples() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MetricSample> samples;
    
    auto buckets = get_buckets();
    for (const auto& bucket : buckets) {
        Labels bucket_labels = labels_;
        bucket_labels["le"] = std::isinf(bucket.upper_bound) ? "+Inf" : 
                              std::to_string(bucket.upper_bound);
        samples.push_back({static_cast<double>(bucket.count), 
                          std::chrono::system_clock::now(), bucket_labels});
    }
    
    // 添加count和sum
    Labels count_labels = labels_;
    count_labels["le"] = "+Inf";
    samples.push_back({static_cast<double>(get_count()), 
                      std::chrono::system_clock::now(), count_labels});
    
    Labels sum_labels = labels_;
    samples.push_back({get_sum(), std::chrono::system_clock::now(), sum_labels});
    
    return samples;
}

std::string Histogram::to_prometheus() const {
    std::ostringstream oss;
    
    if (!help_.empty()) {
        oss << "# HELP " << name_ << " " << help_ << "\n";
    }
    oss << "# TYPE " << name_ << " histogram\n";
    
    // 桶指标
    uint64_t cumulative_count = 0;
    for (size_t i = 0; i < buckets_.size(); ++i) {
        cumulative_count += bucket_counts_[i].load();
        oss << name_ << "_bucket{";
        bool first = true;
        for (const auto& label : labels_) {
            if (!first) oss << ",";
            first = false;
            oss << label.first << "=\"" << label.second << "\"";
        }
        if (!first) oss << ",";
        oss << "le=\"" << buckets_[i] << "\"} " << cumulative_count << "\n";
    }
    
    // +Inf bucket
    cumulative_count += bucket_counts_[buckets_.size()].load();
    oss << name_ << "_bucket{";
    bool first = true;
    for (const auto& label : labels_) {
        if (!first) oss << ",";
        first = false;
        oss << label.first << "=\"" << label.second << "\"";
    }
    if (!first) oss << ",";
    oss << "le=\"+Inf\"} " << cumulative_count << "\n";
    
    // count
    oss << name_ << "_count{";
    first = true;
    for (const auto& label : labels_) {
        if (!first) oss << ",";
        first = false;
        oss << label.first << "=\"" << label.second << "\"";
    }
    oss << "} " << get_count() << "\n";
    
    // sum
    oss << name_ << "_sum{";
    first = true;
    for (const auto& label : labels_) {
        if (!first) oss << ",";
        first = false;
        oss << label.first << "=\"" << label.second << "\"";
    }
    oss << "} " << std::fixed << std::setprecision(6) << get_sum() << "\n";
    
    return oss.str();
}

void Histogram::reset() {
    count_.store(0);
    sum_.store(0.0);
    for (auto& bucket_count : bucket_counts_) {
        bucket_count.store(0);
    }
}

// ==================== Summary 实现 ====================

Summary::Summary(const std::string& name,
                 const std::vector<double>& quantiles,
                 const std::string& help,
                 const Labels& labels)
    : name_(name), help_(help), labels_(labels), quantiles_(quantiles),
      count_(0), sum_(0.0) {
    std::sort(quantiles_.begin(), quantiles_.end());
}

void Summary::observe(double value) {
    std::lock_guard<std::mutex> lock(mutex_);
    count_.fetch_add(1);
    
    double old_sum = sum_.load();
    double expected;
    do {
        expected = old_sum;
        old_sum = sum_.compare_exchange_weak(expected, expected + value);
    } while (old_sum != expected);
    
    samples_.push_back(value);
    
    // 限制样本数量（避免内存无限增长）
    const size_t max_samples = 10000;
    if (samples_.size() > max_samples) {
        // 保留最近的样本
        samples_.erase(samples_.begin(), samples_.begin() + (samples_.size() - max_samples));
    }
}

MetricValue Summary::get_value() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_.load() == 0) {
        return 0.0;
    }
    return sum_.load() / count_.load();
}

std::vector<SummaryQuantile> Summary::get_quantiles() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SummaryQuantile> result;
    
    if (samples_.empty()) {
        for (double q : quantiles_) {
            result.push_back({q, 0.0});
        }
        return result;
    }
    
    std::vector<double> sorted_samples = samples_;
    std::sort(sorted_samples.begin(), sorted_samples.end());
    
    for (double q : quantiles_) {
        size_t index = static_cast<size_t>(q * (sorted_samples.size() - 1));
        if (index >= sorted_samples.size()) {
            index = sorted_samples.size() - 1;
        }
        result.push_back({q, sorted_samples[index]});
    }
    
    return result;
}

uint64_t Summary::get_count() const {
    return count_.load();
}

double Summary::get_sum() const {
    return sum_.load();
}

std::vector<MetricSample> Summary::get_samples() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MetricSample> samples;
    
    auto quantiles = get_quantiles();
    for (const auto& q : quantiles) {
        Labels q_labels = labels_;
        q_labels["quantile"] = std::to_string(q.quantile);
        samples.push_back({q.value, std::chrono::system_clock::now(), q_labels});
    }
    
    // 添加count和sum
    Labels count_labels = labels_;
    samples.push_back({static_cast<double>(get_count()), 
                      std::chrono::system_clock::now(), count_labels});
    
    Labels sum_labels = labels_;
    samples.push_back({get_sum(), std::chrono::system_clock::now(), sum_labels});
    
    return samples;
}

std::string Summary::to_prometheus() const {
    std::ostringstream oss;
    
    if (!help_.empty()) {
        oss << "# HELP " << name_ << " " << help_ << "\n";
    }
    oss << "# TYPE " << name_ << " summary\n";
    
    // 分位数指标
    auto quantiles = get_quantiles();
    for (const auto& q : quantiles) {
        oss << name_ << "{";
        bool first = true;
        for (const auto& label : labels_) {
            if (!first) oss << ",";
            first = false;
            oss << label.first << "=\"" << label.second << "\"";
        }
        if (!first) oss << ",";
        oss << "quantile=\"" << q.quantile << "\"} " 
            << std::fixed << std::setprecision(6) << q.value << "\n";
    }
    
    // count
    oss << name_ << "_count{";
    bool first = true;
    for (const auto& label : labels_) {
        if (!first) oss << ",";
        first = false;
        oss << label.first << "=\"" << label.second << "\"";
    }
    oss << "} " << get_count() << "\n";
    
    // sum
    oss << name_ << "_sum{";
    first = true;
    for (const auto& label : labels_) {
        if (!first) oss << ",";
        first = false;
        oss << label.first << "=\"" << label.second << "\"";
    }
    oss << "} " << std::fixed << std::setprecision(6) << get_sum() << "\n";
    
    return oss.str();
}

void Summary::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    count_.store(0);
    sum_.store(0.0);
    samples_.clear();
}

// ==================== Metrics 实现 ====================

std::unique_ptr<Metrics> Metrics::instance_ = nullptr;
std::once_flag Metrics::once_flag_;

Metrics& Metrics::GetInstance() {
    std::call_once(once_flag_, []() {
        instance_ = std::unique_ptr<Metrics>(new Metrics());
    });
    return *instance_;
}

Metrics::Metrics() {
    // 注册系统指标
    cpu_usage_ = register_gauge("system_cpu_usage_percent", "CPU usage percentage");
    memory_usage_ = register_gauge("system_memory_usage_bytes", "Memory usage in bytes");
    memory_available_ = register_gauge("system_memory_available_bytes", "Available memory in bytes");
    open_files_ = register_gauge("system_open_files", "Number of open file descriptors");
    cpu_time_user_ = register_counter("system_cpu_time_user_seconds", "CPU time spent in user mode");
    cpu_time_system_ = register_counter("system_cpu_time_system_seconds", "CPU time spent in system mode");
}

Metrics::~Metrics() {
    stop_system_metrics_collection();
}

std::shared_ptr<Counter> Metrics::register_counter(const std::string& name,
                                                   const std::string& help,
                                                   const Labels& labels) {
    std::unique_lock lock(metrics_mutex_);
    std::string key = make_metric_key(name, labels);
    
    auto it = metrics_.find(key);
    if (it != metrics_.end()) {
        // 如果已存在，检查类型
        if (it->second->get_type() == MetricType::COUNTER) {
            return std::static_pointer_cast<Counter>(it->second);
        }
        // 类型不匹配，返回nullptr
        return nullptr;
    }
    
    auto counter = std::make_shared<Counter>(name, help, labels);
    metrics_[key] = counter;
    return counter;
}

std::shared_ptr<Gauge> Metrics::register_gauge(const std::string& name,
                                                const std::string& help,
                                                const Labels& labels) {
    std::unique_lock lock(metrics_mutex_);
    std::string key = make_metric_key(name, labels);
    
    auto it = metrics_.find(key);
    if (it != metrics_.end()) {
        if (it->second->get_type() == MetricType::GAUGE) {
            return std::static_pointer_cast<Gauge>(it->second);
        }
        return nullptr;
    }
    
    auto gauge = std::make_shared<Gauge>(name, help, labels);
    metrics_[key] = gauge;
    return gauge;
}

std::shared_ptr<Histogram> Metrics::register_histogram(const std::string& name,
                                                        const std::vector<double>& buckets,
                                                        const std::string& help,
                                                        const Labels& labels) {
    std::unique_lock lock(metrics_mutex_);
    std::string key = make_metric_key(name, labels);
    
    auto it = metrics_.find(key);
    if (it != metrics_.end()) {
        if (it->second->get_type() == MetricType::HISTOGRAM) {
            return std::static_pointer_cast<Histogram>(it->second);
        }
        return nullptr;
    }
    
    auto histogram = std::make_shared<Histogram>(name, buckets, help, labels);
    metrics_[key] = histogram;
    return histogram;
}

std::shared_ptr<Summary> Metrics::register_summary(const std::string& name,
                                                     const std::vector<double>& quantiles,
                                                     const std::string& help,
                                                     const Labels& labels) {
    std::unique_lock lock(metrics_mutex_);
    std::string key = make_metric_key(name, labels);
    
    auto it = metrics_.find(key);
    if (it != metrics_.end()) {
        if (it->second->get_type() == MetricType::SUMMARY) {
            return std::static_pointer_cast<Summary>(it->second);
        }
        return nullptr;
    }
    
    auto summary = std::make_shared<Summary>(name, quantiles, help, labels);
    metrics_[key] = summary;
    return summary;
}

std::shared_ptr<IMetric> Metrics::get_metric(const std::string& name, const Labels& labels) const {
    std::shared_lock lock(metrics_mutex_);
    std::string key = make_metric_key(name, labels);
    auto it = metrics_.find(key);
    if (it != metrics_.end()) {
        return it->second;
    }
    return nullptr;
}

std::vector<std::shared_ptr<IMetric>> Metrics::get_all_metrics() const {
    std::shared_lock lock(metrics_mutex_);
    std::vector<std::shared_ptr<IMetric>> result;
    result.reserve(metrics_.size());
    for (const auto& pair : metrics_) {
        result.push_back(pair.second);
    }
    return result;
}

std::string Metrics::to_prometheus() const {
    std::shared_lock lock(metrics_mutex_);
    std::ostringstream oss;
    
    // 按名称分组指标
    std::map<std::string, std::vector<std::shared_ptr<IMetric>>> grouped;
    for (const auto& pair : metrics_) {
        grouped[pair.second->get_name()].push_back(pair.second);
    }
    
    for (const auto& group : grouped) {
        // 对于每个指标名称，输出所有标签变体
        for (const auto& metric : group.second) {
            oss << metric->to_prometheus();
        }
        oss << "\n";
    }
    
    return oss.str();
}

std::string Metrics::to_json() const {
    std::shared_lock lock(metrics_mutex_);
    std::ostringstream oss;
    
    oss << "{\n";
    oss << "  \"metrics\": [\n";
    
    bool first_metric = true;
    for (const auto& pair : metrics_) {
        if (!first_metric) oss << ",\n";
        first_metric = false;
        
        auto metric = pair.second;
        oss << "    {\n";
        oss << "      \"name\": \"" << metric->get_name() << "\",\n";
        oss << "      \"type\": \"";
        switch (metric->get_type()) {
            case MetricType::COUNTER: oss << "counter"; break;
            case MetricType::GAUGE: oss << "gauge"; break;
            case MetricType::HISTOGRAM: oss << "histogram"; break;
            case MetricType::SUMMARY: oss << "summary"; break;
        }
        oss << "\",\n";
        oss << "      \"help\": \"" << metric->get_help() << "\",\n";
        oss << "      \"value\": " << metric->get_value() << ",\n";
        oss << "      \"labels\": {";
        bool first_label = true;
        for (const auto& label : metric->get_labels()) {
            if (!first_label) oss << ", ";
            first_label = false;
            oss << "\"" << label.first << "\": \"" << label.second << "\"";
        }
        oss << "}\n";
        oss << "    }";
    }
    
    oss << "\n  ]\n";
    oss << "}\n";
    
    return oss.str();
}

void Metrics::reset_all() {
    std::unique_lock lock(metrics_mutex_);
    for (const auto& pair : metrics_) {
        pair.second->reset();
    }
}

bool Metrics::remove_metric(const std::string& name, const Labels& labels) {
    std::unique_lock lock(metrics_mutex_);
    std::string key = make_metric_key(name, labels);
    return metrics_.erase(key) > 0;
}

void Metrics::collect_system_metrics() {
#ifdef _WIN32
    // Windows系统指标收集
    FILETIME idle_time, kernel_time, user_time;
    if (GetSystemTimes(&idle_time, &kernel_time, &user_time)) {
        ULARGE_INTEGER kernel, user;
        kernel.LowPart = kernel_time.dwLowDateTime;
        kernel.HighPart = kernel_time.dwHighDateTime;
        user.LowPart = user_time.dwLowDateTime;
        user.HighPart = user_time.dwHighDateTime;
        
        cpu_time_system_->inc(static_cast<double>(kernel.QuadPart) / 10000000.0);
        cpu_time_user_->inc(static_cast<double>(user.QuadPart) / 10000000.0);
    }
    
    MEMORYSTATUSEX mem_info;
    mem_info.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&mem_info)) {
        memory_usage_->set(static_cast<double>(mem_info.ullTotalPhys - mem_info.ullAvailPhys));
        memory_available_->set(static_cast<double>(mem_info.ullAvailPhys));
    }
    
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        // 可以添加进程特定的内存指标
    }
#else
    // Linux/Unix系统指标收集
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        cpu_time_user_->inc(static_cast<double>(usage.ru_utime.tv_sec) + 
                           usage.ru_utime.tv_usec / 1000000.0);
        cpu_time_system_->inc(static_cast<double>(usage.ru_stime.tv_sec) + 
                              usage.ru_stime.tv_usec / 1000000.0);
    }
    
    // 内存使用（简化版，读取/proc/meminfo）
    std::ifstream meminfo("/proc/meminfo");
    if (meminfo.is_open()) {
        std::string line;
        uint64_t mem_total = 0, mem_available = 0, mem_free = 0;
        while (std::getline(meminfo, line)) {
            if (line.find("MemTotal:") == 0) {
                sscanf(line.c_str(), "MemTotal: %lu kB", &mem_total);
            } else if (line.find("MemAvailable:") == 0) {
                sscanf(line.c_str(), "MemAvailable: %lu kB", &mem_available);
            } else if (line.find("MemFree:") == 0) {
                sscanf(line.c_str(), "MemFree: %lu kB", &mem_free);
            }
        }
        if (mem_available > 0) {
            memory_available_->set(static_cast<double>(mem_available * 1024));
            memory_usage_->set(static_cast<double>((mem_total - mem_available) * 1024));
        } else if (mem_free > 0) {
            memory_available_->set(static_cast<double>(mem_free * 1024));
            memory_usage_->set(static_cast<double>((mem_total - mem_free) * 1024));
        }
    }
    
    // 打开文件数
    int fd_count = 0;
    DIR* dir = opendir("/proc/self/fd");
    if (dir != nullptr) {
        while (readdir(dir) != nullptr) {
            fd_count++;
        }
        closedir(dir);
        open_files_->set(static_cast<double>(fd_count));
    }
#endif
}

void Metrics::set_system_metrics_enabled(bool enabled) {
    system_metrics_enabled_.store(enabled);
}

void Metrics::set_system_metrics_interval(std::chrono::seconds interval) {
    collection_interval_ = interval;
}

void Metrics::start_system_metrics_collection() {
    if (collection_running_.exchange(true)) {
        return; // 已经在运行
    }
    
    system_metrics_enabled_.store(true);
    collection_thread_ = std::thread(&Metrics::system_metrics_collection_loop, this);
}

void Metrics::stop_system_metrics_collection() {
    if (!collection_running_.exchange(false)) {
        return; // 没有运行
    }
    
    system_metrics_enabled_.store(false);
    if (collection_thread_.joinable()) {
        collection_thread_.join();
    }
}

void Metrics::system_metrics_collection_loop() {
    while (system_metrics_enabled_.load()) {
        collect_system_metrics();
        std::this_thread::sleep_for(collection_interval_);
    }
}

Metrics::Statistics Metrics::get_statistics() const {
    std::shared_lock lock(metrics_mutex_);
    Statistics stats;
    stats.total_metrics = metrics_.size();
    stats.system_metrics_enabled = system_metrics_enabled_.load();
    stats.collection_interval = collection_interval_;
    
    for (const auto& pair : metrics_) {
        switch (pair.second->get_type()) {
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
        }
    }
    
    return stats;
}

std::string Metrics::make_metric_key(const std::string& name, const Labels& labels) const {
    std::ostringstream oss;
    oss << name;
    if (!labels.empty()) {
        oss << "{";
        bool first = true;
        for (const auto& label : labels) {
            if (!first) oss << ",";
            first = false;
            oss << label.first << "=" << label.second;
        }
        oss << "}";
    }
    return oss.str();
}

std::string Metrics::labels_to_string(const Labels& labels) const {
    if (labels.empty()) {
        return "";
    }
    
    std::ostringstream oss;
    oss << "{";
    bool first = true;
    for (const auto& label : labels) {
        if (!first) oss << ",";
        first = false;
        oss << label.first << "=\"" << label.second << "\"";
    }
    oss << "}";
    return oss.str();
}

} // namespace utils
} // namespace mementodb

