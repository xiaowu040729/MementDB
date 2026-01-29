#ifndef IO_LOOP_H
#define IO_LOOP_H

#include <functional>
#include <memory>
#include <chrono>
#include <cstdint>
#include <string>
#include <system_error>

namespace mementodb {
namespace net {

// IO事件标志位，使用位掩码支持组合事件
enum class IOEvent : uint32_t {
    NONE     = 0x00,
    READ     = 0x01,      // 可读事件
    WRITE    = 0x02,      // 可写事件
    ERROR    = 0x04,      // 错误事件
    HUP      = 0x08,      // 挂起事件（对端关闭）
    PRI      = 0x10,      // 紧急数据（带外数据）
    RDHUP    = 0x20,      // 流套接字对端关闭或半关闭
    
    // 常用组合
    READ_WRITE = READ | WRITE,
    ALL_EVENTS = READ | WRITE | ERROR | HUP | PRI | RDHUP,
    
    // 触发模式标志（平台相关，使用时需要检查支持）
    ET        = 0x80000000, // 边缘触发（Edge Triggered）
};

// 支持位运算
inline IOEvent operator|(IOEvent a, IOEvent b) {
    return static_cast<IOEvent>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline IOEvent operator&(IOEvent a, IOEvent b) {
    return static_cast<IOEvent>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
inline IOEvent operator^(IOEvent a, IOEvent b) {
    return static_cast<IOEvent>(static_cast<uint32_t>(a) ^ static_cast<uint32_t>(b));
}
inline IOEvent& operator|=(IOEvent& a, IOEvent b) {
    a = a | b;
    return a;
}
inline IOEvent& operator&=(IOEvent& a, IOEvent b) {
    a = a & b;
    return a;
}

// IO事件回调函数类型
using IOEventCallback = std::function<void(int fd, IOEvent event, void* user_data)>;
// 定时器回调函数类型
using TimerCallback = std::function<void(void* user_data)>;
// 异步任务回调函数类型
using TaskCallback = std::function<void()>;
// 异常处理器类型
using ExceptionHandler = std::function<void(const std::exception&)>;

/**
 * IOLoop - IO事件循环抽象基类
 * 
 * 提供统一的IO事件循环接口，支持不同的实现（epoll, kqueue, select, IOCP等）
 * 支持文件描述符事件、定时器、异步任务等功能
 */
class IOLoop {
public:
    using TimerId = uint64_t;
    using SignalId = uint32_t;
    
    // 配置选项
    struct Options {
        bool enable_timers = true;          // 启用定时器
        bool enable_signals = false;        // 启用信号处理
        bool enable_async_tasks = true;     // 启用异步任务
        size_t max_events_per_cycle = 1024; // 每次循环处理的最大事件数
        int default_timeout_ms = 10000;     // 默认超时时间（毫秒）
        std::string name;                   // 循环名称（用于调试）
    };
    
    // 定时器选项
    struct TimerOptions {
        bool repeated = false;      // 是否重复
        bool immediate = false;     // 是否立即执行（忽略首次延迟）
        void* user_data = nullptr;  // 用户数据
        std::string name;           // 定时器名称（用于调试）
    };
    
    // 信号处理选项
    struct SignalOptions {
        bool restart_syscalls = true;  // 信号处理后是否重启系统调用
        bool handler_in_loop = true;   // 是否在事件循环线程中处理
        void* user_data = nullptr;     // 用户数据
    };
    
    // 统计信息
    struct Statistics {
        uint64_t fd_count = 0;          // 注册的文件描述符数量
        uint64_t timer_count = 0;       // 活跃定时器数量
        uint64_t signal_count = 0;      // 注册的信号数量
        uint64_t processed_events = 0;  // 已处理的事件总数
        uint64_t pending_tasks = 0;     // 待处理任务数量
        uint64_t cycles = 0;            // 循环次数
        std::chrono::microseconds total_cycle_time{0};  // 总循环时间
        std::chrono::microseconds max_cycle_time{0};    // 最长单次循环时间
        std::chrono::steady_clock::time_point start_time; // 启动时间
    };
    
    virtual ~IOLoop() = default;
    
    // 禁止拷贝和移动
    IOLoop(const IOLoop&) = delete;
    IOLoop& operator=(const IOLoop&) = delete;
    IOLoop(IOLoop&&) = delete;
    IOLoop& operator=(IOLoop&&) = delete;
    
    /**
     * 初始化IO循环
     * @param options 配置选项
     * @return 是否成功
     */
    virtual bool init(const Options& options) = 0;
    
    /**
     * 添加文件描述符到事件循环
     * @param fd 文件描述符
     * @param events 关注的事件类型（使用位掩码组合）
     * @param callback 事件回调函数
     * @param user_data 用户数据（可选）
     * @return 是否成功
     */
    virtual bool add_fd(int fd, IOEvent events, 
                        IOEventCallback callback, 
                        void* user_data = nullptr) = 0;
    
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
     * 添加定时器
     * @param delay_ms 延迟时间（毫秒）
     * @param interval_ms 间隔时间（毫秒，0表示单次）
     * @param callback 定时器回调
     * @param options 定时器选项
     * @return 定时器ID，0表示失败
     */
    virtual TimerId add_timer(uint64_t delay_ms, 
                             uint64_t interval_ms,
                             TimerCallback callback,
                             const TimerOptions& options) = 0;
    
    /**
     * 取消定时器
     * @param timer_id 定时器ID
     * @return 是否成功
     */
    virtual bool cancel_timer(TimerId timer_id) = 0;
    
    /**
     * 注册信号处理器
     * @param signum 信号编号
     * @param callback 信号处理回调（参数：信号编号，用户数据）
     * @param options 信号处理选项
     * @return 信号ID，0表示失败
     */
    virtual SignalId add_signal(int signum,
                               std::function<void(int, void*)> callback,
                               const SignalOptions& options) = 0;
    
    /**
     * 移除信号处理器
     * @param signal_id 信号ID
     * @return 是否成功
     */
    virtual bool remove_signal(SignalId signal_id) = 0;
    
    /**
     * 在事件循环线程中执行任务
     * @param task 要执行的任务
     * @return 是否成功提交任务
     */
    virtual bool run_in_loop(TaskCallback task) = 0;
    
    /**
     * 在事件循环线程中延迟执行任务
     * @param task 要执行的任务
     * @param delay_ms 延迟时间（毫秒）
     * @return 是否成功提交任务
     */
    virtual bool run_after(TaskCallback task, uint64_t delay_ms) = 0;
    
    /**
     * 在事件循环线程中间隔执行任务
     * @param task 要执行的任务
     * @param interval_ms 间隔时间（毫秒）
     * @return 定时器ID
     */
    virtual TimerId run_every(TaskCallback task, uint64_t interval_ms) = 0;
    
    /**
     * 运行事件循环（阻塞）
     * @param timeout_ms 超时时间（毫秒，0表示无限等待）
     * @return 是否成功运行（false通常表示发生错误）
     */
    virtual bool run(int timeout_ms = 0) = 0;
    
    /**
     * 运行一次事件循环（非阻塞）
     * @param timeout_ms 超时时间（毫秒）
     * @return 处理的事件数量，负值表示错误
     */
    virtual int run_once(int timeout_ms = 0) = 0;
    
    /**
     * 停止事件循环
     * @param graceful 是否优雅停止（等待当前事件处理完成）
     */
    virtual void stop(bool graceful = true) = 0;
    
    /**
     * 检查事件循环是否正在运行
     * @return 是否正在运行
     */
    virtual bool is_running() const = 0;
    
    /**
     * 检查是否在当前事件循环线程中
     * @return 是否在事件循环线程中
     */
    virtual bool is_in_loop_thread() const = 0;
    
    /**
     * 获取事件循环统计信息
     */
    virtual Statistics get_statistics() const = 0;
    
    /**
     * 重置统计信息
     */
    virtual void reset_statistics() = 0;
    
    /**
     * 设置异常处理器
     */
    virtual void set_exception_handler(ExceptionHandler handler) = 0;
    
    /**
     * 获取事件循环名称
     */
    virtual std::string get_name() const = 0;
    
    /**
     * 唤醒事件循环（用于跨线程通知）
     */
    virtual void wakeup() = 0;
    
    /**
     * 获取当前时间戳（毫秒）
     */
    virtual uint64_t now_ms() const = 0;
    
    /**
     * 检查是否支持指定的事件类型
     * @param events 事件类型
     * @return 是否支持
     */
    virtual bool supports(IOEvent events) const = 0;
    
    /**
     * 获取实现类型名称
     */
    virtual const char* implementation_name() const = 0;
    
    /**
     * 获取最后错误信息
     */
    virtual std::error_code last_error() const = 0;
    
    /**
     * 设置调试日志回调
     */
    using DebugLogCallback = std::function<void(const std::string&)>;
    virtual void set_debug_logger(DebugLogCallback callback) = 0;
    
    /**
     * 获取文件描述符当前关注的事件
     * @param fd 文件描述符
     * @return 关注的事件类型，如果未注册返回IOEvent::NONE
     */
    virtual IOEvent get_fd_events(int fd) const = 0;
    
    /**
     * 设置文件描述符的用户数据
     * @param fd 文件描述符
     * @param user_data 用户数据
     * @return 是否成功
     */
    virtual bool set_fd_user_data(int fd, void* user_data) = 0;
    
    /**
     * 获取文件描述符的用户数据
     * @param fd 文件描述符
     * @return 用户数据，如果没有返回nullptr
     */
    virtual void* get_fd_user_data(int fd) const = 0;
    
    /**
     * 清空所有注册的文件描述符
     */
    virtual void clear_all_fds() = 0;
    
    /**
     * 清空所有定时器
     */
    virtual void clear_all_timers() = 0;
    
    /**
     * 等待所有任务完成（用于优雅停止）
     * @param timeout_ms 超时时间（毫秒）
     * @return 是否在超时前完成
     */
    virtual bool wait_for_pending_tasks(uint64_t timeout_ms) = 0;
    
    /**
     * 静态方法：创建平台默认的IOLoop实例
     */
    static std::unique_ptr<IOLoop> create_default();
    
    /**
     * 静态方法：创建指定类型的IOLoop实例
     * @param type 实现类型（"epoll", "kqueue", "select", "iocp"等）
     */
    static std::unique_ptr<IOLoop> create(const std::string& type);
    
protected:
    IOLoop() = default;
    
    // 保护方法，供子类实现使用
    virtual void notify_stop() = 0;
    virtual void update_statistics(const Statistics& delta) = 0;
};

// 辅助函数
/**
 * IOEvent转字符串
 */
std::string io_event_to_string(IOEvent events);
/**
 * 检查IOEvent是否包含特定事件
 */
bool io_event_contains(IOEvent events, IOEvent target);

} // namespace net
} // namespace mementodb

#endif // IO_LOOP_H

