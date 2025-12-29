#include "Logger.hpp"

namespace LoggingSystem {

Logger* Logger::instance = nullptr;

Logger::Logger()
    : enabled(true)
    , m_minimumLevel(LogLevel::INFO)
{
}

Logger::~Logger()
{
    stopWorker();
}

Logger* Logger::GetInstance()
{
    if (instance == nullptr)
    {
        instance = new Logger();
    }
    return instance;
}

void Logger::initialize()
{
    //  初始化日志系统
    if (instance == nullptr)
    {
        instance = new Logger();
    }
    // 设置默认输出器 ..(待完成)
    // 注意：enabled 和 m_minimumLevel 在构造函数中已初始化
}

void Logger::finitalize()
{
    if (instance != nullptr)
    {
        instance->stopWorker();
        delete instance;
        instance = nullptr;
    }
}

void Logger::log(LogMessage message)
{
    // 检查是否启用日志
    if(!enabled || message.level < m_minimumLevel)
    {
        return;
    }

    // 异步模式：入队并唤醒工作线程
    if (asyncEnabled)
    {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            messageQueue.push_back(std::move(message));
        }
        queueCond.notify_one();
        return;
    }

    // 同步模式直接分发
    dispatchMessage(message);
}

void Logger::addSink(const LogSinkPtr& sink)
{
    if (!sink) {
        return;
    }

    std::lock_guard<std::mutex> locker(m_mutex);
    auto it = std::find(sinks.begin(), sinks.end(), sink);
    if (it == sinks.end()) {
        sinks.push_back(sink);
    }
}

void Logger::removeSink(const LogSinkPtr& sink)
{
    std::lock_guard<std::mutex> locker(m_mutex);
    sinks.erase(std::remove(sinks.begin(), sinks.end(), sink), sinks.end());
}

void Logger::setEnabled(bool enabled)
{
    std::lock_guard<std::mutex> locker(m_mutex);
    this->enabled = enabled;
}

bool Logger::isEnabled() const
{
    std::lock_guard<std::mutex> locker(m_mutex);
    return enabled;
}

void Logger::setMinimumLogLevel(LogLevel level)
{
    std::lock_guard<std::mutex> locker(m_mutex);
    this->m_minimumLevel = level;
}

LogLevel Logger::getMinimumLogLevel() const
{
    std::lock_guard<std::mutex> locker(m_mutex);
    return m_minimumLevel;
}

void Logger::dispatchMessage(const LogMessage& message)
{
    std::lock_guard<std::mutex> locker(m_mutex);
    // 所有输出器都输出消息
    for(const auto& sink : sinks)
    {
        if (sink && sink->isEnabled() && message.level >= sink->getMinimumLogLevel()) {
            sink->write(message);
        }
    }
}

void Logger::workerLoop()
{
    // 循环处理队列中的日志消息
    for (;;)
    {
        // 等待队列中有日志消息
        LogMessage msg;
        {
            // 加锁，防止多个线程同时访问队列
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCond.wait(lock, [&] { return stopping || !messageQueue.empty(); });

            if (stopping && messageQueue.empty()) {
                break;
            }
            // 获取队列中的第一个日志消息
            msg = std::move(messageQueue.front());
            messageQueue.pop_front();
        }

        dispatchMessage(msg);
    }
}

void Logger::startWorker()
{
    if (workerThread.joinable()) {
        return;
    }
    stopping = false;
    workerThread = std::thread(&Logger::workerLoop, this);
}

void Logger::stopWorker()
{
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        stopping = true;
    }
    queueCond.notify_all();
    if (workerThread.joinable()) {
        workerThread.join();
    }
    // 清空残留队列
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        messageQueue.clear();
    }
}

void Logger::setAsyncEnabled(bool enabledValue)
{
    if (asyncEnabled == enabledValue) {
        return;
    }

    asyncEnabled = enabledValue;
    if (asyncEnabled) {
        startWorker();
    } else {
        stopWorker();
    }
}

bool Logger::isAsyncEnabled() const
{
    return asyncEnabled;
}


}   // namespace LoggingSystem