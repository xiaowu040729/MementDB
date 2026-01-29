#ifndef SERVER_H
#define SERVER_H

#include "IOLoop.hpp"
#include "Connection.hpp"
#include "ThreadPool.hpp"
#include "Protocol.hpp"
#include "ServerConfig.hpp"
#include <memory>
#include <string>
#include <unordered_map>
#include <atomic>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <chrono>
#include <functional>
#include <system_error>

namespace mementodb {

namespace core {
    class DiskEngineV2;  // 前向声明
}

namespace net {

// 服务器事件回调
using ServerEventCallback = std::function<void(const std::string& event, const std::string& data)>;

// 连接过滤器
using ConnectionFilter = std::function<bool(const std::string& client_ip, int client_port)>;

/**
 * Server - 主服务器类
 * 
 * 整合IO事件循环、连接管理、协议解析等组件，提供完整的数据库服务器功能
 * 支持多协议、多监听端口、集群、监控等高级特性
 */
class Server : public std::enable_shared_from_this<Server> {
public:
    using ConnectionFactory = std::function<std::shared_ptr<Connection>(
        int fd, 
        const std::string& client_ip,
        int client_port,
        std::shared_ptr<core::DiskEngineV2> engine,
        const ProtocolConfig& protocol_config
    )>;
    
    // 服务器状态
    enum class State {
        STOPPED,        // 已停止
        STARTING,       // 启动中
        RUNNING,        // 运行中
        STOPPING,       // 停止中
        RESTARTING,     // 重启中
        ERROR           // 错误状态
    };
    
    // 服务器统计信息
    struct Statistics {
        // 连接统计
        uint64_t total_connections = 0;
        uint64_t active_connections = 0;
        uint64_t peak_connections = 0;
        uint64_t rejected_connections = 0;
        
        // 请求统计
        uint64_t total_requests = 0;
        uint64_t requests_per_second = 0;
        uint64_t peak_requests_per_second = 0;
        uint64_t failed_requests = 0;
        
        // 网络统计
        uint64_t bytes_received = 0;
        uint64_t bytes_sent = 0;
        uint64_t network_errors = 0;
        
        // 命令统计
        std::unordered_map<std::string, uint64_t> command_stats;
        uint64_t slow_commands = 0;
        uint64_t protocol_errors = 0;
        
        // 服务器统计
        uint64_t uptime = 0;                    // 运行时间（秒）
        uint64_t start_time = 0;                // 启动时间戳
        uint64_t last_stats_time = 0;           // 上次统计时间
        
        // 线程池统计
        uint64_t pending_tasks = 0;
        uint64_t completed_tasks = 0;
        uint64_t task_queue_overflows = 0;
        
        // 系统资源
        double cpu_usage = 0.0;                 // CPU使用率
        uint64_t memory_usage = 0;              // 内存使用量（字节）
        uint64_t open_files = 0;                // 打开文件数
        
        std::string to_string() const;
    };
    
    Server(std::shared_ptr<core::DiskEngineV2> engine, 
           const ServerConfig& config = ServerConfig());
    
    ~Server();
    
    // 禁止拷贝和移动
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;
    
    /**
     * 启动服务器（非阻塞）
     * @param wait_for_ready 是否等待服务器完全启动
     * @param timeout_ms 等待超时时间（毫秒）
     * @return 是否成功启动
     */
    bool start(bool wait_for_ready = true, uint64_t timeout_ms = 5000);
    
    /**
     * 停止服务器
     * @param graceful 是否优雅停止（等待连接处理完成）
     * @param timeout_ms 等待超时时间（毫秒）
     */
    void stop(bool graceful = true, uint64_t timeout_ms = 30000);
    
    /**
     * 重启服务器
     */
    bool restart(uint64_t timeout_ms = 10000);
    
    /**
     * 运行服务器（阻塞调用）
     * @param foreground 是否在前台运行（true:阻塞，false:后台运行）
     */
    void run(bool foreground = true);
    
    /**
     * 获取服务器状态
     */
    State get_state() const { return state_.load(); }
    
    /**
     * 检查服务器是否正在运行
     */
    bool is_running() const { return state_.load() == State::RUNNING; }
    
    /**
     * 等待服务器达到指定状态
     */
    bool wait_for_state(State target_state, uint64_t timeout_ms = 5000);
    
    /**
     * 获取服务器配置
     */
    const ServerConfig& get_config() const { return config_; }
    
    /**
     * 更新服务器配置（某些配置需要重启才能生效）
     */
    bool update_config(const ServerConfig& new_config);
    
    /**
     * 获取服务器统计信息
     */
    Statistics get_statistics() const;
    
    /**
     * 重置统计信息
     */
    void reset_statistics();
    
    /**
     * 获取监听端点信息
     */
    struct EndpointInfo {
        std::string host;
        int port;
        int fd;
        bool listening;
        std::string protocol;
    };
    
    std::vector<EndpointInfo> get_listen_endpoints() const;
    
    /**
     * 添加新的监听端点
     */
    bool add_listen_endpoint(const std::string& host, int port);
    
    /**
     * 移除监听端点
     */
    bool remove_listen_endpoint(const std::string& host, int port);
    
    /**
     * 获取活跃连接信息
     */
    struct ConnectionInfo {
        int fd;
        std::string client_ip;
        int client_port;
        std::string server_ip;
        int server_port;
        uint64_t connection_time;
        uint64_t last_activity;
        uint64_t bytes_received;
        uint64_t bytes_sent;
        std::string state;
        std::string protocol_version;
    };
    
    std::vector<ConnectionInfo> get_connections() const;
    
    /**
     * 断开指定连接
     */
    bool disconnect(int fd, bool graceful = true);
    
    /**
     * 断开所有连接
     */
    void disconnect_all(bool graceful = true);
    
    /**
     * 设置连接工厂（用于自定义连接类型）
     */
    void set_connection_factory(ConnectionFactory factory);
    
    /**
     * 设置连接过滤器
     */
    void set_connection_filter(ConnectionFilter filter);
    
    /**
     * 设置服务器事件回调
     */
    void set_event_callback(ServerEventCallback callback);
    
    /**
     * 设置异常处理器
     */
    using ExceptionHandler = std::function<void(const std::exception&)>;
    void set_exception_handler(ExceptionHandler handler);
    
    /**
     * 设置信号处理器
     */
    void setup_signal_handlers();
    
    /**
     * 优雅关闭信号处理
     */
    static void handle_signal(int signal);
    
    /**
     * 保存当前状态（用于持久化）
     */
    bool save_state(const std::string& path);
    
    /**
     * 加载状态（用于恢复）
     */
    bool load_state(const std::string& path);
    
    /**
     * 执行管理命令
     */
    struct CommandResult {
        bool success;
        std::string message;
        std::string data;
    };
    
    CommandResult execute_admin_command(const std::string& command, 
                                       const std::vector<std::string>& args);
    
    /**
     * 健康检查
     */
    struct HealthStatus {
        bool healthy;
        std::string status;
        std::unordered_map<std::string, std::string> details;
        uint64_t timestamp;
    };
    
    HealthStatus health_check() const;
    
    /**
     * 创建服务器实例（工厂方法）
     */
    static std::shared_ptr<Server> create(std::shared_ptr<core::DiskEngineV2> engine,
                                         const ServerConfig& config = ServerConfig());

private:
    // 内部监听器类
    class Listener;
    
    // 连接管理器类
    class ConnectionManager;
    
    // 监控器类
    class Monitor;
    
    // 主要组件
    std::shared_ptr<core::DiskEngineV2> engine_;
    std::unique_ptr<IOLoop> io_loop_;
    std::unique_ptr<ThreadPool> thread_pool_;
    std::unique_ptr<ConnectionManager> connection_manager_;
    std::unique_ptr<Monitor> monitor_;
    
    // 配置和状态
    ServerConfig config_;
    std::atomic<State> state_;
    std::atomic<bool> stopping_;
    
    // 监听器管理
    std::vector<std::unique_ptr<Listener>> listeners_;
    mutable std::shared_mutex listeners_mutex_;
    
    // 服务器ID和元数据
    const std::string server_id_;
    std::chrono::steady_clock::time_point start_time_;
    
    // 回调函数
    ConnectionFactory connection_factory_;
    ConnectionFilter connection_filter_;
    ServerEventCallback event_callback_;
    ExceptionHandler exception_handler_;
    
    // 同步原语
    mutable std::mutex state_mutex_;
    std::condition_variable state_cv_;
    
    // 统计信息
    mutable std::mutex stats_mutex_;
    Statistics stats_;
    std::thread stats_thread_;
    std::atomic<bool> stats_running_;
    
    // 速率限制
    struct RateLimitEntry {
        uint64_t request_count = 0;
        std::chrono::steady_clock::time_point window_start;
    };
    mutable std::mutex rate_limit_mutex_;
    mutable std::unordered_map<std::string, RateLimitEntry> rate_limit_map_;
    
    // 连接计数（每IP）
    mutable std::mutex ip_connections_mutex_;
    std::unordered_map<std::string, size_t> ip_connections_;
    
    // 私有方法
    bool init();
    bool init_listeners();
    bool init_io_loop();
    bool init_thread_pool();
    bool init_monitor();
    
    void cleanup();
    void notify_event(const std::string& event, const std::string& data = "");
    
    // 事件处理
    void on_accept(Listener* listener, int fd, const std::string& client_ip, int client_port);
    void on_connection_closed(int fd, uint64_t connection_id);
    void on_connection_error(int fd, const std::error_code& ec);
    void on_request_completed(int fd, const std::string& command, 
                             uint64_t duration_us, bool success);
    
    // 统计更新
    void update_connection_stats();
    void update_request_stats();
    void update_system_stats();
    void stats_collection_loop();
    
    // 安全验证
    bool validate_connection(const std::string& client_ip, int client_port) const;
    bool check_rate_limit(const std::string& client_ip) const;
    
    // 管理命令实现
    CommandResult admin_info(const std::vector<std::string>& args);
    CommandResult admin_clients(const std::vector<std::string>& args);
    CommandResult admin_stats(const std::vector<std::string>& args);
    CommandResult admin_config(const std::vector<std::string>& args);
    CommandResult admin_shutdown(const std::vector<std::string>& args);
    CommandResult admin_reload(const std::vector<std::string>& args);
    CommandResult admin_diagnostics(const std::vector<std::string>& args);
    
    // 辅助方法
    std::string generate_server_id() const;
    void set_state(State new_state);
    void safe_invoke_callback(const std::function<void()>& callback);
};

// 监听器实现（内部类）
class Server::Listener {
public:
    Listener(Server* server, const ServerConfig::ListenConfig& config);
    ~Listener();
    
    bool start();
    void stop();
    
    const ServerConfig::ListenConfig& config() const { return config_; }
    int fd() const { return fd_; }
    bool is_listening() const { return listening_; }
    
    std::string endpoint() const;
    
private:
    Server* server_;
    ServerConfig::ListenConfig config_;
    int fd_;
    std::atomic<bool> listening_;
    
    bool create_socket();
    bool setup_socket_options();
    bool bind_and_listen();
};

// 连接管理器实现（内部类）
class Server::ConnectionManager {
public:
    ConnectionManager(Server* server, 
                     const ServerConfig::ConnectionConfig& config,
                     std::unique_ptr<IOLoop> io_loop);
    ~ConnectionManager();
    
    bool add_connection(int fd, const std::string& client_ip, int client_port,
                       const std::string& server_ip, int server_port);
    
    // 添加连接（使用已创建的 Connection 对象）
    bool add_connection(int fd, std::shared_ptr<Connection> conn,
                       const std::string& client_ip, int client_port,
                       const std::string& server_ip, int server_port);
    bool remove_connection(int fd);
    void remove_all_connections(bool graceful);
    
    std::vector<ConnectionInfo> get_connections() const;
    size_t connection_count() const;
    
    void update_connection_stats();
    
    // 获取 IOLoop（供 Listener 使用）
    IOLoop* io_loop() { return io_loop_.get(); }
    
private:
    struct ConnectionContext {
        std::shared_ptr<Connection> connection;
        uint64_t id;
        std::string client_ip;
        int client_port;
        std::string server_ip;
        int server_port;
        std::chrono::steady_clock::time_point create_time;
        std::chrono::steady_clock::time_point last_activity;
        uint64_t bytes_received;
        uint64_t bytes_sent;
        std::atomic<bool> closing;
    };
    
    Server* server_;
    ServerConfig::ConnectionConfig config_;
    std::unique_ptr<IOLoop> io_loop_;
    std::thread io_thread_;
    
    mutable std::shared_mutex connections_mutex_;
    std::unordered_map<int, std::shared_ptr<ConnectionContext>> connections_;
    std::unordered_map<uint64_t, int> connection_id_map_;
    
    std::atomic<uint64_t> next_connection_id_;
    
    void cleanup_idle_connections();
    void check_connection_timeouts();
    void schedule_cleanup();
    
    // 连接事件处理
    void on_connection_data(int fd, IOEvent event, void* user_data);
    void on_connection_error(int fd, IOEvent event, void* user_data);
    void on_connection_timeout(int fd);
};

} // namespace net
} // namespace mementodb

#endif // SERVER_H

