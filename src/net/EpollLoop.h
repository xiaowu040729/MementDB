#ifndef EPOLL_LOOP_H
#define EPOLL_LOOP_H

#include "IOLoop.h"
#include <unordered_map>
#include <atomic>
#include <mutex>

namespace mementodb {
namespace net {

/**
 * EpollLoop - Linux epoll实现的IO事件循环
 * 
 * 使用Linux epoll机制实现高性能IO事件循环
 */
class EpollLoop : public IOLoop {
public:
    EpollLoop();
    ~EpollLoop();
    
    bool init() override;
    bool add_fd(int fd, IOEvent events, IOEventCallback callback) override;
    bool modify_fd(int fd, IOEvent events) override;
    bool remove_fd(int fd) override;
    void run() override;
    void stop() override;
    bool is_running() const override;

private:
    int epoll_fd_;
    std::atomic<bool> running_;
    std::unordered_map<int, IOEventCallback> callbacks_;
    std::mutex callbacks_mutex_;
    
    // 将IOEvent转换为epoll事件标志
    uint32_t to_epoll_events(IOEvent events) const;
    
    // 处理epoll事件
    void handle_events(int num_events, struct epoll_event* events);
};

} // namespace net
} // namespace mementodb

#endif // EPOLL_LOOP_H

