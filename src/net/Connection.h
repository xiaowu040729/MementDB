#ifndef CONNECTION_H
#define CONNECTION_H

#include "Protocol.h"
#include <string>
#include <memory>
#include <functional>

namespace mementodb {
namespace core {
    class DiskEngineV2;  // 前向声明
}

namespace net {

/**
 * Connection - 单个客户端连接处理
 * 
 * 管理单个客户端连接的生命周期，处理请求和响应
 */
class Connection {
public:
    using RequestHandler = std::function<void(const Protocol::ParsedCommand&, std::string&)>;
    
    Connection(int fd, std::shared_ptr<core::DiskEngineV2> engine);
    ~Connection();
    
    /**
     * 处理接收到的数据
     * @param data 接收到的数据
     */
    void on_data_received(const std::string& data);
    
    /**
     * 获取文件描述符
     * @return 文件描述符
     */
    int fd() const { return fd_; }
    
    /**
     * 检查连接是否已关闭
     * @return 是否已关闭
     */
    bool is_closed() const { return closed_; }
    
    /**
     * 关闭连接
     */
    void close();
    
    /**
     * 发送响应数据
     * @param data 要发送的数据
     * @return 是否发送成功
     */
    bool send(const std::string& data);

private:
    void process_command(const Protocol::ParsedCommand& cmd);
    void handle_get(const std::vector<std::string>& args, std::string& response);
    void handle_set(const std::vector<std::string>& args, std::string& response);
    void handle_del(const std::vector<std::string>& args, std::string& response);
    void handle_exists(const std::vector<std::string>& args, std::string& response);
    void handle_ping(std::string& response);
    void handle_info(std::string& response);
    
    int fd_;
    bool closed_;
    std::string read_buffer_;
    std::string write_buffer_;
    std::shared_ptr<core::DiskEngineV2> engine_;
};

} // namespace net
} // namespace mementodb

#endif // CONNECTION_H

