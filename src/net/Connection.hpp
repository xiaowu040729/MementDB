#ifndef CONNECTION_H
#define CONNECTION_H

#include "Protocol.hpp"
#include <string>
#include <memory>
#include <functional>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <vector>
#include <optional>
#include <chrono>

namespace mementodb {
namespace core {
    class DiskEngineV2;  // 前向声明
}

namespace net {

// 前向声明
class ConnectionManager;

// 类型别名
using ParsedCommand = ::mementodb::net::ParsedCommand;

/**
 * Connection - 单个客户端连接处理
 * 
 * 管理单个客户端连接的生命周期，处理请求和响应
 * 线程安全设计，支持多线程环境
 */
class Connection : public std::enable_shared_from_this<Connection> {
public:
    using RequestHandler = std::function<bool(const ParsedCommand&, std::string&)>;
    using CommandHandler = std::function<void(Connection&, const std::vector<std::string>&, std::string&)>;
    
    // 连接状态
    enum class State {
        CONNECTED,
        AUTHENTICATING,
        PROCESSING,
        CLOSING,
        CLOSED
    };
    
    Connection(int fd, 
               std::shared_ptr<core::DiskEngineV2> engine,
               std::weak_ptr<ConnectionManager> manager = {});
    ~Connection();
    
    // 禁止拷贝和移动
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) = delete;
    Connection& operator=(Connection&&) = delete;
    
    /**
     * 异步处理接收到的数据
     * @param data 接收到的数据
     * @param size 数据大小
     * @return 处理成功返回true，连接应关闭返回false
     */
    bool on_data_received(const char* data, size_t size);
    
    /**
     * 获取文件描述符
     * @return 文件描述符
     */
    int fd() const { return fd_; }
    
    /**
     * 获取连接ID（用于日志和追踪）
     */
    uint64_t id() const { return connection_id_; }
    
    /**
     * 检查连接是否已关闭
     * @return 是否已关闭
     */
    bool is_closed() const { return state_ == State::CLOSED; }
    
    /**
     * 获取连接状态
     */
    State state() const { return state_; }
    
    /**
     * 获取客户端地址信息
     */
    std::optional<std::string> client_address() const;
    
    /**
     * 获取连接统计信息
     */
    struct Statistics {
        uint64_t bytes_received = 0;
        uint64_t bytes_sent = 0;
        uint64_t commands_processed = 0;
        uint64_t errors = 0;
        std::chrono::steady_clock::time_point connected_time;
    };
    
    Statistics get_statistics() const;
    
    /**
     * 优雅关闭连接
     * @param immediate 是否立即关闭（false表示等待发送缓冲区清空）
     */
    void close(bool immediate = false);
    
    /**
     * 异步发送响应数据
     * @param data 要发送的数据
     * @return 队列成功返回true，失败返回false
     */
    bool send(std::string data);
    
    /**
     * 批量发送数据
     */
    bool send_batch(const std::vector<std::string>& responses);
    
    /**
     * 设置命令处理器（支持自定义命令扩展）
     */
    void set_command_handler(const std::string& command, CommandHandler handler);
    
    /**
     * 设置默认处理器
     */
    void set_default_handler(CommandHandler handler);
    
    /**
     * 认证相关
     */
    bool authenticate(const std::string& password);
    bool is_authenticated() const { return authenticated_; }
    
    /**
     * 设置连接超时
     */
    void set_timeout(std::chrono::milliseconds timeout);
    
    /**
     * 设置认证密码（从Server配置获取）
     */
    void set_auth_password(const std::string& password);

private:
    // 核心处理方法
    void process_buffer();
    bool parse_commands();
    void process_command(const ParsedCommand& cmd);
    
    // 内置命令处理器
    void handle_get(const std::vector<std::string>& args, std::string& response);
    void handle_set(const std::vector<std::string>& args, std::string& response);
    void handle_del(const std::vector<std::string>& args, std::string& response);
    void handle_exists(const std::vector<std::string>& args, std::string& response);
    void handle_ping(std::string& response);
    void handle_info(std::string& response);
    void handle_auth(const std::vector<std::string>& args, std::string& response);
    void handle_select(const std::vector<std::string>& args, std::string& response);
    void handle_multi(const std::vector<std::string>& args, std::string& response);
    void handle_exec(std::string& response);
    void handle_discard(std::string& response);
    
    // 事务支持
    struct Transaction {
        std::vector<ParsedCommand> commands;
        bool active = false;
    };
    
    // 连接成员变量
    const int fd_;
    const uint64_t connection_id_;
    std::atomic<State> state_;
    std::atomic<bool> authenticated_;
    std::atomic<bool> in_transaction_;
    
    // 客户端地址信息
    mutable std::optional<std::string> cached_client_address_;
    mutable std::mutex address_mutex_;
    
    // 认证密码（从Server配置获取）
    std::string auth_password_;
    
    // 当前数据库索引（Redis兼容，默认0）
    std::atomic<int> current_db_index_;
    
    // 缓冲区和同步
    mutable std::mutex read_buffer_mutex_;
    std::string read_buffer_;
    mutable std::mutex write_buffer_mutex_;
    std::vector<std::string> write_buffer_;
    
    // 统计信息
    mutable std::mutex stats_mutex_;
    Statistics stats_;
    
    // 依赖组件
    std::shared_ptr<core::DiskEngineV2> engine_;
    std::weak_ptr<ConnectionManager> manager_;
    
    // 命令处理器映射
    std::unordered_map<std::string, CommandHandler> command_handlers_;
    CommandHandler default_handler_;
    
    // 事务状态
    Transaction current_transaction_;
    
    // 连接超时
    std::chrono::milliseconds timeout_;
    std::chrono::steady_clock::time_point last_activity_;
    
    // 私有辅助方法
    void update_activity();
    void check_timeout();
    void flush_write_buffer();
    void send_error(const std::string& error_msg);
    void send_ok();
};

// 连接管理器接口
class ConnectionManager {
public:
    virtual void on_connection_closed(uint64_t connection_id) = 0;
    virtual ~ConnectionManager() = default;
};

} // namespace net
} // namespace mementodb

#endif // CONNECTION_H

