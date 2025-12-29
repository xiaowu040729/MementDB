#include "FileSink.hpp"
#include "LogMacros.hpp"

namespace LoggingSystem {

FileSink::FileSink(LogSink* parent)
    : LogSink(parent)
{
    m_name = "FileSink";
}

FileSink::~FileSink()
{
    flush();
}


void FileSink::initial()
{
    // 初始化输出器
    needToSplit = false;
    // 文件分割大小
    splitSize = 1024 * 1024 * 10;
    // 文件分割数量
    splitCount = 0;
    // 文件分割时间
    splitTime = std::chrono::system_clock::now();
    // 是否启用日志
    enabled = true;
    // 最小日志级别
    minimumLevel = LogLevel::DEBUG;
}

bool FileSink::isFileOpen()
{
    std::lock_guard<std::mutex> locker(fileMutex);
    return file.is_open();
}

bool FileSink::openFile(const std::string& filePath)
{
    // 注意：此函数内部不要调用 LOG_XXX 宏，否则会导致递归锁死
    // 因为 LOG_XXX -> Logger::log -> FileSink::write -> 再次锁 fileMutex

    bool ok = true;
    bool createdDir = false;
    std::string errorMsg;

    {
        std::lock_guard<std::mutex> locker(fileMutex);
        
        // 如果文件已经打开，先关闭
        if (file.is_open()) {
            file.close();
            m_isFileOpen = false;
        }
        
        // 设置文件路径
        this->filePath = filePath;
        
        // 获取文件信息，用于创建目录
        std::filesystem::path fileInfo(filePath);
        std::filesystem::path dir = fileInfo.parent_path();
        
        // 如果目录不存在，创建目录
        if (!dir.empty() && !std::filesystem::exists(dir)) {
            if (!std::filesystem::create_directories(dir)) {
                ok = false;
                errorMsg = "Failed to create directory: " + dir.string();
            } else {
                createdDir = true;
            }
        }

        if (ok) {
            // 设置文件对象
            file.open(filePath, std::ios::out | std::ios::app | std::ios::binary);
            
            // 以追加模式打开文件（如果文件不存在则创建）
            if (!file.is_open()) {
                ok = false;
                errorMsg = "Failed to open log file: " + filePath;
                m_isFileOpen = false;
            } else {
                m_isFileOpen = true;
            }
        }
    }

    if (!ok) {
        // 这里使用标准错误输出，避免再次进入日志系统造成死锁
        std::cerr << "[FileSink] " << errorMsg << std::endl;
        return false;
    }
    
    std::stringstream stream;

    // 如果是新文件，写入文件头
    if (file.tellp() == 0) {
        switch (m_format) {
        case Format::JSON:
            stream << "[\n";
            break;
        case Format::CSV:
            stream << "timestamp,level,module,message,threadId,file,line,function\n";
            break;
        case Format::TEXT:
        default: {
            auto now = std::chrono::system_clock::now();
            std::time_t tt = std::chrono::system_clock::to_time_t(now);
            std::tm tm{};
        #if defined(_WIN32)
            localtime_s(&tm, &tt);
        #else
            localtime_r(&tt, &tm);
        #endif
            stream << "=== Log Started at "
                   << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
                   << " ===\n";
            break;
        }
        }
        stream.flush();
    }

    return true;
}


void FileSink::setFilePath(const std::string& filePath)
{
    this->filePath = filePath;
}

std::string FileSink::getFilePath() const
{
    return filePath;
}

void FileSink::write(const LogMessage& message)
{
    // 检查是否启用日志
    if(!enabled || message.level < minimumLevel)
    {
        return;
    }
    
   // 检查文件是否打开，如果没打开则尝试打开
   if(!isFileOpen())
   {
       openFile(filePath);
   }

   // 写入日志消息（再次加锁，保证线程安全）
   std::lock_guard<std::mutex> locker(fileMutex);
   if (file.is_open()) {
       // 每条日志一行，避免多条日志黏在一起
       const std::string line = message.toString(false) + "\n";
       file << line;
       file.flush();
   }
}



void FileSink::closeFile()
{
    std::lock_guard<std::mutex> locker(fileMutex);
    file.close();
    m_isFileOpen = false;
}


void FileSink::setRotationSize(int size)
{
    this->rotationsize = size;
}

int FileSink::rotationSize() const
{
    return rotationsize;
}

void FileSink::SetMaxBackupCount(int count)
{
    this->maxBackupCount = count;
}

int FileSink::MaxBackupCount() const
{
    return maxBackupCount;
}

void FileSink::setFormat(Format format)
{
    this->m_format = format;
}

FileSink::Format FileSink::getFormat() const
{
    return m_format;
}

std::string FileSink::name() const
{
    return m_name;
}

void FileSink::flush()
{
    std::lock_guard<std::mutex> locker(fileMutex);
    if (file.is_open()) {
        file.flush();
    }
}

} // namespace LoggingSystem