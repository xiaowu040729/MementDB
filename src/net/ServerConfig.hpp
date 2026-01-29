// File: src/net/ServerConfig.hpp
#ifndef SERVER_CONFIG_H
#define SERVER_CONFIG_H

#include "Protocol.hpp"

#include <string>
#include <vector>

namespace mementodb {
namespace net {

// 服务器配置
struct ServerConfig {
    // 网络配置
    struct ListenConfig {
        std::string host = "0.0.0.0";
        int port = 6379;
        int backlog = 1024;          // 监听队列长度
        bool reuse_addr = true;      // 地址重用
        bool reuse_port = false;     // 端口重用（Linux 3.9+）
        bool ipv6_only = false;      // IPv6 only
        bool tcp_no_delay = true;    // 禁用Nagle算法
        bool tcp_keepalive = true;   // 启用TCP保活
        int keepalive_time = 300;    // 保活时间（秒）
        int keepalive_intvl = 30;    // 保活间隔（秒）
        int keepalive_probes = 3;    // 保活探测次数
        int recv_buffer_size = 0;    // 接收缓冲区大小（0表示系统默认）
        int send_buffer_size = 0;    // 发送缓冲区大小
    };
    
    std::vector<ListenConfig> listen_configs;  // 支持多个监听端点
    
    // 协议配置
    ProtocolConfig protocol_config;
    
    // 连接配置
    struct ConnectionConfig {
        int max_connections = 10000;           // 最大连接数
        int max_idle_time = 3600;              // 最大空闲时间（秒）
        int connection_timeout = 30;           // 连接超时（秒）
        int max_request_size = 1024 * 1024;    // 最大请求大小（1MB）
        int max_response_size = 10 * 1024 * 1024; // 最大响应大小（10MB）
        bool enable_keepalive = true;          // 启用连接保持
        int keepalive_timeout = 60;            // 连接保持超时（秒）
    };
    
    ConnectionConfig connection_config;
    
    // 线程配置
    struct ThreadConfig {
        int io_threads = 1;                    // IO线程数（0表示CPU核心数）
        int worker_threads = 4;                // 工作线程数
        int task_queue_size = 10000;           // 任务队列大小
        bool pin_threads = false;              // 绑定线程到CPU核心
    };
    
    ThreadConfig thread_config;
    
    // 性能配置
    struct PerformanceConfig {
        bool enable_nodelay = true;            // 启用TCP_NODELAY
        bool enable_defer_accept = true;       // 启用TCP_DEFER_ACCEPT
        bool enable_fast_open = false;         // 启用TCP_FASTOPEN
        int tcp_fastopen_queue = 1024;         // TCP_FASTOPEN队列长度
        int epoll_max_events = 4096;           // epoll最大事件数
        int epoll_timeout = 100;               // epoll超时（毫秒）
    };
    
    PerformanceConfig performance_config;
    
    // 安全配置
    struct SecurityConfig {
        bool enable_auth = false;              // 启用认证
        std::string auth_password;             // 认证密码
        std::vector<std::string> allow_ips;    // 允许的IP列表（白名单）
        std::vector<std::string> deny_ips;     // 拒绝的IP列表（黑名单）
        int max_connections_per_ip = 0;        // 每IP最大连接数（0表示无限制）
        bool enable_ratelimit = false;         // 启用速率限制
        int max_requests_per_second = 1000;    // 每秒最大请求数
    };
    
    SecurityConfig security_config;
    
    // 监控配置
    struct MonitorConfig {
        bool enable_stats = true;              // 启用统计
        int stats_interval = 60;               // 统计间隔（秒）
        bool enable_slow_log = true;           // 启用慢查询日志
        int slow_log_threshold = 100;          // 慢查询阈值（毫秒）
        bool enable_access_log = true;         // 启用访问日志
        std::string log_file;                  // 日志文件路径
    };
    
    MonitorConfig monitor_config;
    
    // 服务器标识
    std::string server_name = "MementoDB";
    std::string server_version = "1.0.0";
    std::string instance_id;                   // 实例ID（用于集群）
    
    // 辅助方法
    bool validate() const;
    std::string to_string() const;
};

} // namespace net
} // namespace mementodb

#endif // SERVER_CONFIG_H

