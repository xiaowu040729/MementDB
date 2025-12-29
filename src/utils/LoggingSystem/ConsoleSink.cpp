#include "ConsoleSink.hpp"


namespace LoggingSystem {

ConsoleSink::ConsoleSink(LogSink* parent)
    : LogSink(parent)
    , useColor(true)
    , m_errorToStdErr(false)
{
    m_name = "ConsoleSink";
}

void ConsoleSink::initial()
{
    // 初始化输出器
    useColor = true;
    m_errorToStdErr = false;
}


ConsoleSink::~ConsoleSink()
{
    
    flush();
}

void ConsoleSink::SetuseColor(bool useColor)
{
    std::lock_guard<std::mutex> locker(mutex);
    this->useColor = useColor;
}

bool ConsoleSink::isColorUsed() const
{
    std::lock_guard<std::mutex> locker(mutex);
    return useColor;
}

void ConsoleSink::setErrorToStdErr(bool enable)
{
    std::lock_guard<std::mutex> locker(mutex);
    this->m_errorToStdErr = enable;
}

bool ConsoleSink::errorToStdErr() const
{
    std::lock_guard<std::mutex> locker(mutex);
    return m_errorToStdErr;
}

void ConsoleSink::setOutputToStdout(bool enable)
{
    std::lock_guard<std::mutex> locker(mutex);
    m_outputToStdout = enable;
}

bool ConsoleSink::outputToStdout() const
{
    std::lock_guard<std::mutex> locker(mutex);
    return m_outputToStdout;
}

void ConsoleSink::setOutputToStderr(bool enable)
{
    std::lock_guard<std::mutex> locker(mutex);
    m_outputToStderr = enable;
}

bool ConsoleSink::outputToStderr() const
{
    std::lock_guard<std::mutex> locker(mutex);
    return m_outputToStderr;
}

void ConsoleSink::write(const LogMessage& message)
{
    // 快速路径：提前检查，避免不必要的加锁
    if (!enabled || message.level < minimumLevel) {
        return;
    }

    // 读取配置（需要加锁）
    bool shouldUseColor;
    bool shouldUseStdErr;
    {
        std::lock_guard<std::mutex> locker(mutex);
        
        // 双重检查：防止在检查后、加锁前状态改变
        if (!enabled || message.level < minimumLevel) {
            return;
        }
        
        shouldUseColor = useColor;
        shouldUseStdErr = m_errorToStdErr && 
                         (message.level == LogLevel::ERROR || 
                          message.level == LogLevel::FATAL);
    }

    // 格式化消息（在锁外进行，减少锁持有时间）
    const std::string output = shouldUseColor 
                          ? message.toColorString() 
                          : message.toString(false);

    // 输出到控制台
    if (shouldUseStdErr && m_outputToStderr) {
        std::cerr << output << std::endl;
        std::cerr.flush();
    } else if (m_outputToStdout) {
        std::cout << output << std::endl;
        std::cout.flush();
    }
}

void ConsoleSink::flush()
{
    std::lock_guard<std::mutex> locker(mutex);
    std::cout.flush();
    std::cerr.flush();
}

std::string ConsoleSink::name() const
{
    return m_name;
}
}   // namespace LoggingSystem
