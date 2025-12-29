#ifndef LOG_MACROS_HPP // 防止重复包含
#define LOG_MACROS_HPP // 定义宏防止重复包含

#include "Logger.hpp"
#include <string>
#include "LogMessage.hpp"
#include <chrono>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <vector>
#include <map>
#include <set>
#include <unordered_map>

namespace LoggingSystem {

namespace Internal {
// 创建日志消息
inline LogMessage createLogMessage(const std::string& message,
                                   LogLevel level,
                                   const std::string& module,
                                   const std::string& file = "",
                                   int line = 0,
                                   const std::string& function = "") {
    return LogMessage(message, level, module, file, line, function);
}
}

#define LOGGER LoggingSystem::Logger::GetInstance()

#define LOG(level, module, message) \
    LOGGER->log(LoggingSystem::Internal::createLogMessage( \
        (message), (level), (module), std::string(__FILE__), __LINE__, std::string(__FUNCTION__)))

#define LOG_DEBUG(moudle, message) \
    LOG(LoggingSystem::LogLevel::DEBUG, moudle, message)

#define LOG_INFO(moudle, message) \
    LOG(LoggingSystem::LogLevel::INFO, moudle, message)

#define LOG_WARN(moudle, message) \
    LOG(LoggingSystem::LogLevel::WARN, moudle, message)

#define LOG_ERROR(moudle, message) \
    LOG(LoggingSystem::LogLevel::ERROR, moudle, message)

#define LOG_FATAL(moudle, message) \
    LOG(LoggingSystem::LogLevel::FATAL, moudle, message)

//条件日志
#define LOG_IF(condition,moudle,message) \
    if(condition) \
    { \
        LOG(moudle, message); \
    }

#define LOG_DEBUG_IF(condition,moudle,message) \
    if(condition) \
    { \
        LOG_DEBUG(moudle, message); \
    }

#define LOG_ERROR_IF(condition,moudle,message) \
    if(condition) \
    { \
        LOG_ERROR(moudle, message); \
    }


// 带上下文的日志宏（可以扩展）
#define LOG_WITH_CONTEXT(level, module, message, ctx) \
    do { \
        LoggingSystem::LogMessage msg = LoggingSystem::Internal::createLogMessage( \
            message, level, module, std::string(__FILE__), __LINE__, std::string(__FUNCTION__)); \
        msg.context = ctx; \
        LOGGER->log(msg); \
    } while(0)


// 范围进入/退出日志宏
#define LOG_SCOPE_ENTER(module, scopeName) \
    LOG_DEBUG(module, std::string("Entering: ") + std::string(scopeName))

#define LOG_SCOPE_EXIT(module, scopeName) \
    LOG_DEBUG(module, std::string("Exiting: ") + std::string(scopeName))



// 性能日志宏
#define LOG_PERFORMANCE_START(module, operation) \
    do { \
        std::chrono::system_clock::time_point startTime = std::chrono::system_clock::now(); \
        LOG_DEBUG(module, std::string("Starting: ") + std::string(operation)); \
    } while(0)

#define LOG_PERFORMANCE_END(module, operation) \
    do { \
        std::chrono::system_clock::time_point endTime = std::chrono::system_clock::now(); \
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count(); \
        LOG_DEBUG(module, std::string("Ending: ") + std::string(operation) + std::string(" took ") + std::to_string(duration) + std::string(" ms")); \
    } while(0)





} // namespace LoggingSystem

#endif // LOG_MACROS_HPP // 防止重复包含