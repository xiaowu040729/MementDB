// File: src/utils/Metrics_Example.cpp
// Metrics使用示例

#include "Metrics.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <random>

using namespace mementodb::utils;

void ExampleBasicUsage() {
    std::cout << "=== Metrics 基本使用示例 ===\n\n";
    
    auto& metrics = Metrics::GetInstance();
    
    // 1. 注册计数器
    std::cout << "1. 注册计数器:\n";
    auto requests_total = metrics.register_counter("http_requests_total", 
                                                   "Total number of HTTP requests");
    requests_total->inc();
    requests_total->inc(5);
    std::cout << "   requests_total = " << requests_total->get_value() << "\n";
    
    // 2. 注册带标签的计数器
    auto requests_by_method = metrics.register_counter("http_requests_total",
                                                       "Total HTTP requests by method",
                                                       {{"method", "GET"}});
    requests_by_method->inc(10);
    std::cout << "   requests_total{method=\"GET\"} = " 
              << requests_by_method->get_value() << "\n";
    
    // 3. 注册仪表盘
    std::cout << "\n2. 注册仪表盘:\n";
    auto active_connections = metrics.register_gauge("connections_active",
                                                     "Number of active connections");
    active_connections->set(42);
    active_connections->inc(5);
    active_connections->dec(2);
    std::cout << "   active_connections = " << active_connections->get_value() << "\n";
    
    // 4. 注册直方图
    std::cout << "\n3. 注册直方图:\n";
    auto request_duration = metrics.register_histogram("http_request_duration_seconds",
                                                       {0.1, 0.5, 1.0, 2.5, 5.0, 10.0},
                                                       "HTTP request duration");
    
    // 模拟一些请求延迟
    std::random_device rd;
    std::mt19937 gen(rd());
    std::normal_distribution<> dis(1.0, 0.5);
    
    for (int i = 0; i < 100; ++i) {
        double duration = std::max(0.0, dis(gen));
        request_duration->observe(duration);
    }
    
    std::cout << "   记录了100个请求延迟样本\n";
    std::cout << "   总请求数: " << request_duration->get_count() << "\n";
    std::cout << "   总延迟: " << request_duration->get_sum() << "秒\n";
    
    // 5. 注册摘要
    std::cout << "\n4. 注册摘要:\n";
    auto response_size = metrics.register_summary("http_response_size_bytes",
                                                  {0.5, 0.9, 0.95, 0.99},
                                                  "HTTP response size");
    
    for (int i = 0; i < 50; ++i) {
        double size = 1000 + (i * 100);
        response_size->observe(size);
    }
    
    auto quantiles = response_size->get_quantiles();
    std::cout << "   分位数:\n";
    for (const auto& q : quantiles) {
        std::cout << "     " << (q.quantile * 100) << "%: " << q.value << "\n";
    }
}

void ExamplePrometheusExport() {
    std::cout << "\n=== Prometheus格式导出示例 ===\n\n";
    
    auto& metrics = Metrics::GetInstance();
    
    // 创建一些指标
    auto counter = metrics.register_counter("test_counter", "A test counter");
    counter->inc(42);
    
    auto gauge = metrics.register_gauge("test_gauge", "A test gauge");
    gauge->set(3.14);
    
    // 导出为Prometheus格式
    std::string prometheus_output = metrics.to_prometheus();
    std::cout << "Prometheus格式输出:\n";
    std::cout << prometheus_output << "\n";
}

void ExampleSystemMetrics() {
    std::cout << "\n=== 系统指标收集示例 ===\n\n";
    
    auto& metrics = Metrics::GetInstance();
    
    // 启用系统指标收集
    metrics.set_system_metrics_enabled(true);
    metrics.set_system_metrics_interval(std::chrono::seconds(5));
    
    std::cout << "启动系统指标收集...\n";
    metrics.start_system_metrics_collection();
    
    // 收集一次系统指标
    metrics.collect_system_metrics();
    
    // 获取系统指标
    auto cpu_usage = metrics.get_metric("system_cpu_usage_percent");
    auto memory_usage = metrics.get_metric("system_memory_usage_bytes");
    auto open_files = metrics.get_metric("system_open_files");
    
    if (cpu_usage) {
        std::cout << "CPU使用率: " << cpu_usage->get_value() << "%\n";
    }
    if (memory_usage) {
        std::cout << "内存使用: " << (memory_usage->get_value() / 1024 / 1024) << " MB\n";
    }
    if (open_files) {
        std::cout << "打开文件数: " << open_files->get_value() << "\n";
    }
    
    // 等待一段时间
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    // 停止收集
    metrics.stop_system_metrics_collection();
    std::cout << "系统指标收集已停止\n";
}

void ExampleStatistics() {
    std::cout << "\n=== Metrics统计信息示例 ===\n\n";
    
    auto& metrics = Metrics::GetInstance();
    
    // 创建各种类型的指标
    metrics.register_counter("counter1", "Counter 1");
    metrics.register_counter("counter2", "Counter 2");
    metrics.register_gauge("gauge1", "Gauge 1");
    metrics.register_histogram("histogram1", {}, "Histogram 1");
    metrics.register_summary("summary1", {}, "Summary 1");
    
    // 获取统计信息
    auto stats = metrics.get_statistics();
    std::cout << "Metrics统计信息:\n";
    std::cout << "  总指标数: " << stats.total_metrics << "\n";
    std::cout << "  计数器数: " << stats.counters << "\n";
    std::cout << "  仪表盘数: " << stats.gauges << "\n";
    std::cout << "  直方图数: " << stats.histograms << "\n";
    std::cout << "  摘要数: " << stats.summaries << "\n";
    std::cout << "  系统指标启用: " << (stats.system_metrics_enabled ? "是" : "否") << "\n";
}

void ExampleServerIntegration() {
    std::cout << "\n=== 服务器集成示例 ===\n\n";
    
    auto& metrics = Metrics::GetInstance();
    
    // 模拟服务器指标
    auto total_connections = metrics.register_counter("server_connections_total",
                                                       "Total number of connections");
    auto active_connections = metrics.register_gauge("server_connections_active",
                                                      "Number of active connections");
    auto total_requests = metrics.register_counter("server_requests_total",
                                                     "Total number of requests");
    auto request_duration = metrics.register_histogram("server_request_duration_seconds",
                                                        {0.01, 0.05, 0.1, 0.5, 1.0, 5.0},
                                                        "Request processing duration");
    
    // 模拟一些活动
    total_connections->inc(10);
    active_connections->set(5);
    total_requests->inc(100);
    
    // 模拟请求处理时间
    std::random_device rd;
    std::mt19937 gen(rd());
    std::exponential_distribution<> dis(2.0); // 平均0.5秒
    
    for (int i = 0; i < 50; ++i) {
        double duration = dis(gen);
        request_duration->observe(duration);
    }
    
    std::cout << "服务器指标:\n";
    std::cout << "  总连接数: " << total_connections->get_value() << "\n";
    std::cout << "  活跃连接数: " << active_connections->get_value() << "\n";
    std::cout << "  总请求数: " << total_requests->get_value() << "\n";
    std::cout << "  请求处理时间统计:\n";
    std::cout << "    样本数: " << request_duration->get_count() << "\n";
    std::cout << "    总时间: " << request_duration->get_sum() << "秒\n";
    
    // 导出为Prometheus格式
    std::cout << "\nPrometheus格式:\n";
    std::string prometheus = metrics.to_prometheus();
    std::cout << prometheus << "\n";
}

int main() {
    try {
        ExampleBasicUsage();
        ExamplePrometheusExport();
        ExampleSystemMetrics();
        ExampleStatistics();
        ExampleServerIntegration();
    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}

