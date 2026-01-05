#ifndef IO_LOOP_H
#define IO_LOOP_H

#include <functional>
#include <memory>

namespace mementodb {
namespace net {

// IO事件类型
enum class IOEvent {
    READ,
    WRITE,
    ERROR
};

// IO事件回调函数类型
using IOEventCallback = std::function<void(int fd, IOEvent event)>;

/**
 * IOLoop - IO事件循环抽象基类
 * 
 * 提供统一的IO事件循环接口，支持不同的实现（epoll, kqueue, select等）
 */
class IOLoop {
public:
    virtual ~IOLoop() = default;
    
    /**
     * 初始化IO循环
     * @return 是否成功
     */
    virtual bool init() = 0;
    
    /**
     * 添加文件描述符到事件循环
     * @param fd 文件描述符
     * @param events 关注的事件类型（READ, WRITE等）
     * @param callback 事件回调函数
     * @return 是否成功
     */
    virtual bool add_fd(int fd, IOEvent events, IOEventCallback callback) = 0;
    
    /**
     * 修改文件描述符的关注事件
     * @param fd 文件描述符
     * @param events 新的事件类型
     * @return 是否成功
     */
    virtual bool modify_fd(int fd, IOEvent events) = 0;
    
    /**
     * 从事件循环中移除文件描述符
     * @param fd 文件描述符
     * @return 是否成功
     */
    virtual bool remove_fd(int fd) = 0;
    
    /**
     * 运行事件循环（阻塞）
     */
    virtual void run() = 0;
    
    /**
     * 停止事件循环
     */
    virtual void stop() = 0;
    
    /**
     * 检查事件循环是否正在运行
     * @return 是否正在运行
     */
    virtual bool is_running() const = 0;
};

} // namespace net
} // namespace mementodb

#endif // IO_LOOP_H

