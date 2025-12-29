#ifndef FILE_SINK_HPP // 防止重复包含
#define FILE_SINK_HPP // 定义宏防止重复包含

#include "LogSink.hpp"
#include <fstream>
#include <mutex>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <filesystem>
#include "LogMessage.hpp"


namespace LoggingSystem {

class FileSink : public LogSink
{
public:
    explicit FileSink(LogSink* parent = nullptr);
    ~FileSink();
    
    virtual void initial() override;
    /**
     * @brief 写入日志消息
     * @param message 日志消息
     */
    virtual void write(const LogMessage& message) override;
    /**
     * @brief 设置文件路径
     * @param filePath 文件路径
     */
    void setFilePath(const std::string& filePath);
    /**
     * @brief 获取文件路径
     * @return 文件路径
     */
    std::string getFilePath() const;
    /**
     * @brief 刷新缓冲区
     * @details 刷新缓冲区，将缓冲区中的数据写入到目标位置
     */
    virtual void flush() override;
    /**
     * @brief 获取输出器名称
     * @return 输出器名称
     */
    virtual std::string name() const override;

    /**
     * @brief 检查文件是否打开
     * @return 是否打开
     *
     * 注意：内部会加锁，所以这里不是 const 成员函数。
     */
    bool isFileOpen();
    /**
     * @brief 打开文件  
     * @details 打开文件，如果文件不存在，则创建文件
     * @param filePath 文件路径
     */
    bool openFile(const std::string& filePath);
    /**
     * @brief 关闭文件
     * @details 关闭文件，如果文件打开，则关闭文件
     */
    void closeFile();

    /**
     * @brief 设置轮转大小
     * @param size 轮转大小
     */
    void setRotationSize(int size);
    int rotationSize() const;

    /**
     * @brief 设置最大备份数量
     * @param count 最大备份数量
     */
    void SetMaxBackupCount(int count);
    int  MaxBackupCount() const;

    // 格式类型
    enum class Format{
        TEXT,
        JSON,
        CSV,
    };

    /**
     * @brief 获取格式类型
     * @param format 格式类型
     */
    Format getFormat() const;
    void setFormat(Format format);


private:
void rotateIfNeeded();
void backupFile(const std::string& sourcePath, int index);

private:
    // 是否启用日志
    bool enabled = true;
    // 最小日志级别
    LogLevel minimumLevel;
    // 文件路径
    std::string filePath;
    // 文件句柄
    std::fstream file;
    // 文件锁
    std::mutex fileMutex;
    // 文件是否打开（成员变量，与 isFileOpen() 函数区分）
    bool m_isFileOpen = false;
    // 文件是否需要分割
    bool needToSplit = false;
    // 文件分割大小
    int splitSize = 1024 * 1024 * 10;
    // 文件分割数量
    int splitCount = 0;
    // 文件分割时间
    std::chrono::system_clock::time_point splitTime;
    // 轮转大小
    int rotationsize;
    // 最大备份数量
    int maxBackupCount;
    // 格式
    Format m_format;
};

}   // namespace LoggingSystem

#endif  // FILE_SINK_HPP // 防止重复包含