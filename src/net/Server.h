#ifndef SERVER_H
#define SERVER_H

#include "IOLoop.h"
#include "Connection.h"
#include "ThreadPool.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <atomic>

namespace mementodb {
namespace core {
    class DiskEngineV2;  // 前向声明
}

namespace net {

/**
 * Server - 主服务器类
 * 
 * 整合IO事件循环、连接管理、协议解析等组件，提供完整的数据库服务器功能
 */
class Server {
public:
    Server(std::shared_ptr<core::DiskEngineV2> engine, 
           const std::string& host = "0.0.0.0", 
           int port = 6379);
    ~Server();
    
    /**
     * 启动服务器
     * @return 是否成功启动
     */
    bool start();
    
    /**
     * 停止服务器
     */
    void stop();
    
    /**
     * 运行服务器（阻塞调用）
     */
    void run();
    
    /**
     * 检查服务器是否正在运行
     * @return 是否正在运行
     */
    bool is_running() const { return running_.load(); }
    
    /**
     * 获取服务器监听地址
     * @return 地址字符串
     */
    std::string get_host() const { return host_; }
    
    /**
     * 获取服务器监听端口
     * @return 端口号
     */
    int get_port() const { return port_; }

private:
    void on_accept(int fd, IOEvent event);
    void on_client_data(int fd, IOEvent event);
    void on_client_error(int fd, IOEvent event);
    void remove_connection(int fd);
    
    bool create_listen_socket();
    void setup_socket_options(int fd);
    
    std::shared_ptr<core::DiskEngineV2> engine_;
    std::unique_ptr<IOLoop> io_loop_;
    std::unique_ptr<ThreadPool> thread_pool_;
    
    std::string host_;
    int port_;
    int listen_fd_;
    
    std::atomic<bool> running_;
    std::unordered_map<int, std::shared_ptr<Connection>> connections_;
    std::mutex connections_mutex_;
};

} // namespace net
} // namespace mementodb

#endif // SERVER_H

