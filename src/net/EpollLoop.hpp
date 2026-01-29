#ifndef EPOLL_LOOP_H
#define EPOLL_LOOP_H

#include "IOLoop.hpp"
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <memory>
#include <vector>
#include <functional>
#include <chrono>
#include <list>
#include <queue>
#include <condition_variable>
#include <string>
#include <thread>
#include <shared_mutex>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <signal.h>

// 前向声明 epoll_event 结构体（避免在头文件中直接包含 <sys/epoll.h>）
struct epoll_event;

namespace mementodb {
namespace net {

// 前向声明
class TimerManager;
class SignalHandler;

/**
 * EpollLoop - Linux epoll实现的IO事件循环
 * 
 * 使用Linux epoll机制实现高性能IO事件循环
 * 支持定时器、信号处理、异步任务等高级功能
 */
class EpollLoop : public IOLoop {
public:
    EpollLoop();
    
    // 禁止拷贝和移动
    EpollLoop(const EpollLoop&) = delete;
    EpollLoop& operator=(const EpollLoop&) = delete;
    EpollLoop(EpollLoop&&) = delete;
    EpollLoop& operator=(EpollLoop&&) = delete;
    
    ~EpollLoop() override;
    
    // IOLoop接口实现
    bool init(const Options& options) override;
    // 添加文件描述符到事件循环
    bool add_fd(int fd, IOEvent events, IOEventCallback callback, void* user_data = nullptr) override;
    // 修改文件描述符的关注事件
    bool modify_fd(int fd, IOEvent events) override;
    // 从事件循环中移除文件描述符
    bool remove_fd(int fd) override;
    
    // 添加定时器
    TimerId add_timer(uint64_t delay_ms,
                      uint64_t interval_ms,
                      TimerCallback callback,
                      const TimerOptions& options) override;
    // 取消定时器
    bool cancel_timer(TimerId timer_id) override;
    // 添加信号处理
    
    SignalId add_signal(int signum,
                        std::function<void(int, void*)> callback,
                        const SignalOptions& options) override;
    // 移除信号处理
    bool remove_signal(SignalId signal_id) override;
    
    // 在事件循环线程中执行任务
    bool run_in_loop(TaskCallback task) override;
    // 在事件循环线程中延迟执行任务
    bool run_after(TaskCallback task, uint64_t delay_ms) override;
    // 在事件循环线程中间隔执行任务
    TimerId run_every(TaskCallback task, uint64_t interval_ms) override;
    // 运行事件循环
    bool run(int timeout_ms = 0) override;
    // 运行事件循环一次
    int run_once(int timeout_ms = 0) override;
    // 停止事件循环

    void stop(bool graceful = true) override;
    // 是否在事件循环线程中运行
    bool is_running() const override;
    // 是否在事件循环线程中
    bool is_in_loop_thread() const override;
    
    // 获取事件循环统计信息
    Statistics get_statistics() const override;
    // 重置事件循环统计信息
    void reset_statistics() override;
    // 设置异常处理器
    
    void set_exception_handler(ExceptionHandler handler) override;
    
    std::string get_name() const override;
    // 唤醒事件循环
    void wakeup() override;
    // 获取当前时间戳
    
    uint64_t now_ms() const override;
    // 是否支持指定的事件类型
    bool supports(IOEvent events) const override;
    // 获取实现类型名称
    const char* implementation_name() const override;
    // 获取最后错误信息
    std::error_code last_error() const override;
    // 设置调试日志回调
    void set_debug_logger(DebugLogCallback callback) override;
    
    // 设置优先级
    bool set_priority(int priority);
    // 设置最大事件数
    void set_max_events_per_cycle(size_t max_events);
    // 设置epoll超时时间
    void set_epoll_timeout(int timeout_ms);
    
    // 获取文件描述符事件
    IOEvent get_fd_events(int fd) const override;
    // 设置文件描述符用户数据
    bool set_fd_user_data(int fd, void* user_data) override;
    // 获取文件描述符用户数据
    void* get_fd_user_data(int fd) const override;
    
    // 清除所有文件描述符
    void clear_all_fds() override;
    // 清除所有定时器
    void clear_all_timers() override;
    // 等待异步任务
    bool wait_for_pending_tasks(uint64_t timeout_ms) override;
    
private:
    struct FdContext {
        IOEvent events{IOEvent::NONE};
        IOEventCallback callback;
        void* user_data{nullptr};
        bool removed{false};
        uint64_t last_active{0};
    };
    
public: // 公开给 TimerManager 使用
    struct TimerInfo {
        // 定时器ID
        TimerId id;
        // 定时器回调
        TimerCallback callback;
        // 到期时间
        uint64_t expire_time;
        // 间隔时间
        uint64_t interval;
        // 是否重复
        bool repeated;
        // 用户数据
        void* user_data;
        // 是否取消
        bool cancelled = false;
    };
    
    struct SignalInfo {
        // 信号编号
        int signum;
        // 信号处理回调
        std::function<void(int)> callback;
        // 信号处理选项
        SignalOptions options;
    };
    
    // 成员变量
    // epoll文件描述符
    int epoll_fd_;
    // 唤醒文件描述符
    int wakeup_fd_;  // 用于唤醒epoll_wait的eventfd
    // 是否运行
    std::atomic<bool> running_;
    // 是否停止
    std::atomic<bool> stopping_;
    
    // 线程相关
    // 事件循环线程ID
    std::thread::id loop_thread_id_;
    // 事件循环线程互斥锁
    mutable std::mutex loop_mutex_;
    
    // 文件描述符管理（使用读写锁优化）
    mutable std::shared_mutex fd_mutex_;
    // 文件描述符上下文
    std::unordered_map<int, std::shared_ptr<FdContext>> fd_contexts_;
    
    // 定时器管理
    std::unique_ptr<TimerManager> timer_manager_;
    int timer_fd_;  // timerfd用于定时器
    std::unordered_map<TimerId, std::shared_ptr<TimerInfo>> timers_;
    std::atomic<TimerId> next_timer_id_;
    mutable std::mutex timer_mutex_;
    
    // 信号管理
    int signal_fd_;  // signalfd用于信号处理
    std::unordered_map<int, SignalInfo> signal_handlers_;
    mutable std::mutex signal_mutex_;
    
    // 异步任务队列
    std::queue<TaskCallback> pending_tasks_;
    mutable std::mutex task_mutex_;
    std::condition_variable task_cv_;
    
    // 统计信息
    mutable std::mutex stats_mutex_;
    Statistics stats_;
    std::chrono::steady_clock::time_point last_cycle_time_;
    
    // 配置
    std::string name_;
    size_t max_events_per_cycle_;
    int epoll_timeout_ms_;
    int priority_;
    
    // 错误与日志
    std::error_code last_error_;
    DebugLogCallback debug_logger_;
    
    // 异常处理
    ExceptionHandler exception_handler_;
    
    // 私有方法
    bool init_wakeup_fd();
    bool init_timer_fd();
    bool init_signal_fd();
    
    void process_events();
    void handle_events(int num_events, struct epoll_event* events);
    void handle_fd_event(int fd, uint32_t events);
    void handle_wakeup();
    void handle_timer();
    void handle_signal();
    
    void process_pending_tasks();
    void execute_task(TaskCallback task);
    
    void update_cycle_statistics(std::chrono::microseconds cycle_time);
    void cleanup_removed_fds();
    
    uint32_t to_epoll_events(IOEvent events) const;
    IOEvent from_epoll_events(uint32_t events) const;
    
    template<typename Func>
    void safe_execute(Func&& func);

    // IOLoop 保护接口实现
    void notify_stop() override;
    void update_statistics(const Statistics& delta) override;
};

// 定时器管理器
class TimerManager {
public:
    explicit TimerManager(EpollLoop* loop);
    ~TimerManager();
    
    EpollLoop::TimerId add_timer(const EpollLoop::TimerInfo& info);
    bool cancel_timer(EpollLoop::TimerId timer_id);
    void process_timers();
    
    // 获取下一个定时器到期时间
    uint64_t get_next_timeout() const;
    
private:
    struct TimerCompare {
        bool operator()(const std::shared_ptr<EpollLoop::TimerInfo>& a,
                        const std::shared_ptr<EpollLoop::TimerInfo>& b) const {
            return a->expire_time > b->expire_time;
        }
    };
    
    EpollLoop* loop_;
    std::priority_queue<std::shared_ptr<EpollLoop::TimerInfo>,
                        std::vector<std::shared_ptr<EpollLoop::TimerInfo>>,
                        TimerCompare> timer_queue_;
    mutable std::mutex timer_mutex_;
};

// 信号处理器
class SignalHandler {
public:
    explicit SignalHandler(EpollLoop* loop);
    ~SignalHandler();
    
    bool add_signal(int signum, 
                    std::function<void(int)> callback,
                    const EpollLoop::SignalOptions& options);
    bool remove_signal(int signum);
    void process_signals();
    
private:
    EpollLoop* loop_;
    sigset_t signal_mask_;
    mutable std::mutex signal_mutex_;
};

} // namespace net
} // namespace mementodb

#endif // EPOLL_LOOP_H

