#ifndef LOGGER_HPP // 防止重复包含
#define LOGGER_HPP // 定义宏防止重复包含

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include "LogMessage.hpp"
#include "LogSink.hpp"

namespace LoggingSystem {

/**
 * @brief 日志类
 * @details 提供日志记录功能，支持多种输出方式（控制台/文件），支持日志级别控制，支持日志文件分割
 */
class Logger {
public:
    /// 获取单例实例
    static Logger* GetInstance();

    static void initialize();   // 初始化日志系统
    static void finitalize();   // 清理资源

    /// 添加输出器
    void addSink(const LogSinkPtr& sink);

    /// 移除输出器
    void removeSink(const LogSinkPtr& sink);

    /// 记录日志消息
    void log(LogMessage message);

    /// 设置最小日志级别
    void setMinimumLogLevel(LogLevel level);
    /// 获取最小日志级别
    LogLevel getMinimumLogLevel() const;

    /// 设置是否启用日志
    void setEnabled(bool value);
    bool isEnabled() const;

    /// 启用/关闭异步写入
    void setAsyncEnabled(bool value);
    bool isAsyncEnabled() const;

private:
    Logger();
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    // 同步/异步共用的实际分发逻辑
    void dispatchMessage(const LogMessage& message);
    void workerLoop();
    void startWorker();
    void stopWorker();

private:
    // 单例实例
    static Logger* instance;

    // 互斥锁（允许 const 方法加锁）
    mutable std::mutex m_mutex;
    // 输出器列表
    std::vector<LogSinkPtr> sinks;
    // 最小日志级别
    LogLevel m_minimumLevel = LogLevel::INFO;
    // 是否启用日志
    bool enabled = true;

    // 异步相关
    bool asyncEnabled = false;
    bool stopping = false;
    // 工作线程
    std::thread workerThread;
    // 互斥锁，用于保护队列
    std::mutex queueMutex;
    // 条件变量，用于等待队列中有日志消息
    std::condition_variable queueCond;
    // 队列中的日志消息
    std::deque<LogMessage> messageQueue;
};

} // namespace LoggingSystem

#endif // LOGGER_HPP