#ifndef CONSOLE_SINK_HPP // 防止重复包含
#define CONSOLE_SINK_HPP // 定义宏防止重复包含

#include "LogSink.hpp"
#include <iostream>

namespace LoggingSystem {

class ConsoleSink : public LogSink
{
public:
    explicit ConsoleSink(LogSink* parent = nullptr);
    ~ConsoleSink();

    virtual void initial() override;

    /// 设置是否使用颜色
    void SetuseColor(bool useColor);
    /// 获取是否使用颜色
    bool isColorUsed() const;

    /// 写入日志消息
    virtual void write(const LogMessage& message) override;
    /// 刷新缓冲区
    virtual void flush() override;
    /// 获取输出器名称
    virtual std::string name() const override;

    /// 设置错误级别及以上输出到 stderr
    void setErrorToStdErr(bool enable);
    /// 获取是否输出到 stderr
    bool errorToStdErr() const;

    /// 设置是否输出到 stdout
    void setOutputToStdout(bool enable);
    /// 获取是否输出到 stdout
    bool outputToStdout() const;
    /// 设置是否输出到 stderr
    void setOutputToStderr(bool enable);
    /// 获取是否输出到 stderr
    bool outputToStderr() const;

private:
    // 是否使用颜色
    bool useColor = true;
    // 是否输出到 stderr（成员变量，与 errorToStdErr() 函数区分）
    bool m_errorToStdErr = false;
    // 输出到 stdout/stderr 的开关
    bool m_outputToStdout = true;
    bool m_outputToStderr = true;
};

}   // namespace LoggingSystem

#endif  // CONSOLE_SINK_HPP // 防止重复包含