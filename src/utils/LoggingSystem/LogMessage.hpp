#ifndef LOG_MESSAGE_HPP // 防止重复包含
#define LOG_MESSAGE_HPP // 定义宏防止重复包含

#include <string>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

/**
 * @brief 
 * @details 日志系统命名空间，用于封装日志系统相关的类和函数
 */
namespace LoggingSystem {




/**
 * @brief 日志级别
 * @details 日志级别，用于控制日志的输出
 */
enum class LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR,
    FATAL,
    UNKNOWN,
};

enum class LogModule {
    ENGINE,
    GAME,
    UI,
    NETWORK,
    DATABASE,
    OTHER,
};




/**
 * @brief 日志消息类
 * @details 日志消息类，用于存储日志消息
 */
class LogMessage {
public:  
    LogMessage() = default;
    ~LogMessage() = default;

    /**
    * @brief 构造函数
    * @param message 日志消息
    * @param level 日志级别
    * @param moudle 日志模块
    * @param file 日志文件
    * @param line 日志行号
    * @param function 日志函数
    */
    LogMessage(const std::string& message, LogLevel level, const std::string& moudle, const std::string& file = "", int line = 0, const std::string& function = "");
    
    // 转换为字符串
    std::string toString(bool verbose = false) const;
    // 转换为JSON   
    std::unordered_map<std::string, std::string> toJson() const;
    // 转换为彩色字符串
    std::string toColorString() const;

    // 转换为日志级别字符串
    std::string LogLevelToString(LogLevel level) const;
    // 转换为日志级别
    LogLevel StringToLogLevel(std::string string) const;

    // 成员变量
    std::string message; // 日志消息
    LogLevel level; // 日志级别
    std::chrono::system_clock::time_point timestamp; // 日志时间
    std::string moudle; // 日志模块
    std::thread::id threadId; // 日志线程
    std::string file; // 日志文件
    int line; // 日志行号
    std::string function; // 日志函数
    std::unordered_map<std::string, std::string> context; // 日志上下文
};


}   // namespace LoggingSystem
#endif  // LOG_MESSAGE_HPP // 防止重复包含