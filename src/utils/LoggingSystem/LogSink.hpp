#ifndef SINK_HPP // 防止重复包含
#define SINK_HPP // 定义宏防止重复包含

#include <string>
#include <mutex>
#include "LogMessage.hpp"

namespace LoggingSystem {

/**
 * @brief 日志输出器
 * @details 日志输出器，用于将日志消息输出到指定位置
 */
class LogSink
{
public:
    explicit LogSink(LogSink* parent = nullptr);   // 不允许隐式转换
    virtual ~LogSink() = default;   // 虚析构函数

    /// 初始化输出器
    virtual void initial() = 0;

    /// 写入日志消息
    virtual void write(const LogMessage& message) = 0;

    /// 刷新缓冲区
    virtual void flush() = 0;

    /// 设置是否启用日志
    void setEnabled(bool value) { enabled = value; }

    /// 获取是否启用日志
    bool isEnabled() const { return enabled; }

    /// 设置最小日志级别
    void setMinimumLogLevel(LogLevel level) { minimumLevel = level; }

    /// 获取最小日志级别
    LogLevel getMinimumLogLevel() const { return minimumLevel; }

    /// 获取输出器名称
    virtual std::string name() const = 0;

protected:
    // 是否启用日志
    bool enabled = true;
    // 最小日志级别
    LogLevel minimumLevel = LogLevel::DEBUG;
    // 输出器名称（成员变量，与 name() 函数区分）
    std::string m_name;
    // 互斥锁（允许 const 方法加锁）
    mutable std::mutex mutex;
};

// 智能指针类型定义    用LogSinkPtr代替LogSink*
using LogSinkPtr = std::shared_ptr<LogSink>;

}   // namespace LoggingSystem

#endif  // LOG_SINK_HPP // 防止重复包含