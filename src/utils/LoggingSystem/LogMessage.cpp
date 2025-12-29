#include "LogMessage.hpp"
#include <string>
#include <sstream>
#include <iomanip>
#include <ctime>

namespace LoggingSystem {

LogMessage::LogMessage(const std::string& message, LogLevel level, const std::string& moudle, const std::string& file, int line, const std::string& function)
    : message(message), 
      level(level), 
      moudle(moudle), 
      file(file), 
      line(line), 
      function(function),
      timestamp(std::chrono::system_clock::now()),
      threadId(std::this_thread::get_id())
{}


std::string LogMessage::toString(bool verbose) const
{
   // 转换时间戳为字符串
   auto time_t = std::chrono::system_clock::to_time_t(timestamp);
   std::stringstream ss;
   ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
   std::string timeStr = ss.str();
   
   std::string levelStr = LogLevelToString(level);
   
   // 转换线程ID为字符串
   std::stringstream threadStream;
   threadStream << threadId;
   std::string threadIdStr = threadStream.str();

   std::string safeMessage = message;
   // 如果不需要详细信息，则去除换行符
   if(!verbose)
   {
    // 去除换行符
    size_t pos = 0;
    while ((pos = safeMessage.find("\n", pos)) != std::string::npos) {
        safeMessage.replace(pos, 1, " ");
        pos += 1;
    }
   }

   std::stringstream result;
   if(verbose)
   {
    // 返回详细信息 [时间][级别][模块][线程]: 消息
    //              -> 文件:行号 函数名
    result << "[" << timeStr << "][" << levelStr << "][" << moudle << "][" << threadIdStr << "]: " << safeMessage;
    if (!file.empty()) {
        result << "\n      -> " << file << ":" << line << " " << function;
    }
   }
   else{
    // 返回简洁信息 [时间][级别][模块]: 消息
    result << "[" << timeStr << "][" << levelStr << "][" << moudle << "]: " << safeMessage;
   }
   
   return result.str();
}


// 核心作用：将日志（或其他业务对象）的文本内容转换为JSON格式
std::unordered_map<std::string, std::string> LogMessage::toJson() const
{
    std::unordered_map<std::string, std::string> json;
    
    // 转换时间戳
    auto time_t = std::chrono::system_clock::to_time_t(timestamp);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
    json["timestamp"] = ss.str();
    
    // 转换线程ID
    std::stringstream threadStream;
    threadStream << threadId;
    json["threadId"] = threadStream.str();
    
    json["level"] = LogLevelToString(level);
    json["moudle"] = moudle;
    json["file"] = file;
    json["line"] = std::to_string(line);
    json["function"] = function;
    json["message"] = message;
    return json;
}


// 将日志消息转换为彩色字符串
std::string LogMessage::toColorString() const
{
    // 转换时间戳
    auto time_t = std::chrono::system_clock::to_time_t(timestamp);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
    std::string timeStr = ss.str();
    
    std::string levelStr = LogLevelToString(level);
    std::string resetCode = "\033[0m";
    
    // 确保字符串里没有换行符
    std::string safeMessage = message;
    size_t pos = 0;
    while ((pos = safeMessage.find("\n", pos)) != std::string::npos) {
        safeMessage.replace(pos, 1, " ");
        pos += 1;
    }
   
    // 根据日志级别设置颜色
    std::string colorCode;
    switch(level)
    {
        case LogLevel::DEBUG:
        {
            colorCode = "\033[34m"; // 蓝色
            break;
        }
        case LogLevel::INFO:
        {
            colorCode = "\033[37m"; // 白色
            break;
        }
        case LogLevel::WARN:
        {
            colorCode = "\033[33m"; // 黄色
            break;
        }
        case LogLevel::ERROR:
        {
            colorCode = "\033[31m"; // 红色
            break;
        }
        case LogLevel::FATAL:
        {
            colorCode = "\033[35m"; // 紫色
            break;
        }
        default:
        {
            colorCode = "\033[0m"; // 默认颜色
            break;
        }
    }

    std::stringstream result;
    result << colorCode << "[" << timeStr << "][" << levelStr << "][" << moudle << "]: " << safeMessage << resetCode;
    return result.str();
}


std::string LogMessage::LogLevelToString(LogLevel level) const
{
    switch(level)
    {
        case LogLevel::DEBUG:
        {
            return "DEBUG";
        }
        case LogLevel::INFO:
        {
            return "INFO";
        }
        case LogLevel::WARN:
        {
            return "WARN";
        }
        case LogLevel::ERROR:
        {
            return "ERROR";
        }
        case LogLevel::FATAL:
        {
            return "FATAL";
        }
        default:
        {
            return "UNKNOWN";
        }
    }

}

LogLevel LogMessage::StringToLogLevel(std::string string) const
{
    if(string == "DEBUG")
    {
        return LogLevel::DEBUG;
    }
    else if(string == "INFO")
    {
        return LogLevel::INFO;
    }
    else if(string == "WARN")
    {
        return LogLevel::WARN;
    }
    else if(string == "ERROR")
    {
        return LogLevel::ERROR;
    }
    else if(string == "FATAL")
    {
        return LogLevel::FATAL;
    }
    else
    {
        return LogLevel::UNKNOWN; // 默认返回 UNKNOWN
    }
}

}


