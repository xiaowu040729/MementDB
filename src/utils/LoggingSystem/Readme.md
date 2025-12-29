## LoggingSystem 模块说明

### 1. 模块概述

`LoggingSystem` 是一个基于 Qt 的轻量级日志模块，提供：

- **多级别日志**：DEBUG / INFO / WARN / ERROR / FATAL
- **多输出端**：控制台输出（`ConsoleSink`）、文件输出（`FileSink`），可扩展自定义输出端
- **线程安全**：内部使用 `QMutex` / `QMutexLocker` 保护共享数据
- **格式化输出**：统一的文本格式，可扩展为 JSON / CSV 等

主要组成文件：

- `Logger.hpp / Logger.cpp`：日志核心单例
- `LogSink.hpp`：输出器抽象基类
- `ConsoleSink.hpp / ConsoleSink.cpp`：控制台输出器
- `FileSink.hpp / FileSink.cpp`：文件输出器
- `LogMessage.hpp / LogMessage.cpp`：日志消息结构与格式化
- `LogMacros.hpp`：日志快捷宏
- `test_logging.cpp`：日志系统自测程序

---

### 2. 核心类与职责

#### 2.1 LogLevel & LogMessage

- `LogLevel`：日志级别枚举，定义为：
  - `DEBUG`, `INFO`, `WARN`, `ERROR`, `FATAL`, `UNKNOWN`
- `LogMessage`：表示一条日志记录，包含：
  - 消息文本：`QString message`
  - 级别：`LogLevel level`
  - 时间：`QDateTime timestamp`
  - 模块：`QString moudle`
  - 线程 ID：`qint64 threadId`
  - 源信息：`file / line / function`
  - 扩展上下文：`QVariantMap context`

常用方法：

- `QString toString(bool verbose = false) const`  
  - `false`：`[时间][级别][模块]: 消息`
  - `true`：附带文件、行号、函数等详细信息
- `QString toColorString() const`：带 ANSI 颜色码的字符串，用于彩色控制台
- `QJsonObject toJson() const`：转换为 JSON 对象

#### 2.2 LogSink（抽象输出器）

- 抽象基类，继承自 `QObject`：
  - 纯虚接口：
    - `void initial()`
    - `void write(const LogMessage& message)`
    - `void flush()`
    - `QString name() const`
  - 通用配置：
    - `void setEnabled(bool enabled) / bool isEnabled() const`
    - `void setMinimumLogLevel(LogLevel level) / LogLevel getMinimumLogLevel() const`
  - 内部状态：
    - `bool enabled`（默认 `true`）
    - `LogLevel minimumLevel`（默认 `DEBUG`）
    - `QString m_name`
    - `mutable QMutex mutex`

提供别名：

- `using LogSinkPtr = QSharedPointer<LogSink>;`

#### 2.3 ConsoleSink（控制台输出器）

职责：将日志输出到标准输出/错误输出。

特性：

- 支持按级别使用不同颜色（DEBUG 蓝色、INFO 白色、WARN 黄色、ERROR 红色、FATAL 紫色）
- 可选择将 ERROR / FATAL 输出到 `stderr`
- 使用 `QTextStream(stdout/stderr)` + UTF‑8 编码输出

主要接口：

- `void initial() override`
- `void write(const LogMessage& message) override`
- `void flush() override`
- `QString name() const override`
- 配置方法：
  - `void SetuseColor(bool useColor) / bool isColorUsed() const`
  - `void setErrorToStdErr(bool enable) / bool errorToStdErr() const`

> **提示（Windows 中文控制台）**：为避免中文乱码，建议在 VS 开发者命令行中先执行 `chcp 65001`，再运行程序。

#### 2.4 FileSink（文件输出器）

职责：将日志写入文件。

主要成员：

- `QString filePath`：日志文件路径
- `QFile file`：文件句柄
- `QMutex fileMutex`：文件写入锁
- `bool m_isFileOpen`：文件是否已打开
- 轮转相关配置（当前接口已预留，逻辑可按需扩展）：
  - `int rotationsize`
  - `int maxBackupCount`
  - `bool needToSplit`
  - `int splitSize`
  - `int splitCount`
  - `QDateTime splitTime`
- 格式配置：
  - `enum class Format { TEXT, JSON, CSV };`
  - `Format m_format`

主要接口：

- `void initial() override`：初始化默认配置
- `bool isFileOpen()`
- `bool openFile(const QString& filePath)`
- `void setFilePath(const QString& filePath)`
- `QString getFilePath() const`
- `void write(const LogMessage& message) override`
- `void flush() override`
- `void closeFile()`
- `void setRotationSize(int size) / int rotationSize() const`
- `void SetMaxBackupCount(int count) / int MaxBackupCount() const`
- `void setFormat(Format format) / Format getFormat() const`
- `QString name() const override`

当前实现要点：

- 打开文件时使用：
  - `QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text`
- 文本编码：
  - Qt5：`QTextStream::setCodec("UTF-8")`
  - Qt6：`QTextStream::setEncoding(QStringConverter::Utf8)`
- 若是新文件，会写入一个简单的文件头：
  - TEXT 模式：`=== Log Started at yyyy-MM-dd hh:mm:ss ===`
- 写入时每条日志一行，避免追加时黏在一起：
  - `message.toString(false) + "\n"`

#### 2.5 Logger（日志核心）

单例类，负责：

- 管理输出器列表：`QVector<LogSinkPtr> sinks`
- 根据全局/局部级别过滤日志
- 向所有符合条件的 Sink 分发 `LogMessage`

关键接口：

- 静态生命周期：
  - `static Logger* GetInstance()`
  - `static void initialize()`
  - `static void finitalize()`
- 日志记录：
  - `void log(LogMessage message)`
  - `void debug / info / warn / error / fatal(...)`
- 输出器管理：
  - `void addSink(const LogSinkPtr& sink)`
  - `void removeSink(const LogSinkPtr& sink)`
- 总体控制：
  - `void setMinimumLogLevel(LogLevel level) / LogLevel getMinimumLogLevel() const`
  - `void setEnabled(bool enabled) / bool isEnabled() const`

内部通过 `QMutex m_mutex` 保证多线程安全。

---

### 3. 日志宏（LogMacros）

为了方便在业务代码中快速记录日志，`LogMacros.hpp` 提供了一系列宏，典型用法：

- `LOG_DEBUG(module, message)`
- `LOG_INFO(module, message)`
- `LOG_WARN(module, message)`
- `LOG_ERROR(module, message)`
- `LOG_FATAL(module, message)`
- 条件日志：
  - `LOG_DEBUG_IF(condition, module, message)`

这些宏会自动捕获：

- 模块名（你传入的 `module`）
- 源文件名、行号、函数名（通过 `__FILE__` / `__LINE__` / `__FUNCTION__`）

---

### 4. 初始化与使用示例

#### 4.1 在测试程序中使用（`test_logging.cpp`）

1. **初始化日志系统与控制台输出器**：

```cpp
QCoreApplication app(argc, argv);

Logger::initialize();
Logger* logger = Logger::GetInstance();

QSharedPointer<ConsoleSink> consoleSink(new ConsoleSink());
consoleSink->setMinimumLogLevel(LogLevel::DEBUG);
logger->addSink(consoleSink);
```

2. **文件日志测试（写入 `Log/test_log.txt`）**：

```cpp
Logger* logger = Logger::GetInstance();
QSharedPointer<FileSink> fileSink(new FileSink());
fileSink->setFilePath("Log/test_log.txt");
fileSink->setRotationSize(1024 * 1024); // 1 MB
fileSink->SetMaxBackupCount(5);
fileSink->setFormat(FileSink::Format::TEXT);
fileSink->setMinimumLogLevel(LogLevel::DEBUG);

if (fileSink->openFile("Log/test_log.txt")) {
    logger->addSink(fileSink);
    LOG_INFO("FileTest", "文件日志测试开始");

    for (int i = 0; i < 10; ++i) {
        LOG_DEBUG("FileTest", QString("这是第 %1 条日志").arg(i + 1));
    }

    LOG_INFO("FileTest", "文件日志测试完成");

    fileSink->flush();
    fileSink->closeFile();
    logger->removeSink(fileSink);
}
```

3. **在业务代码中输出日志**：

```cpp
LOG_INFO("Game", "游戏初始化完成");
LOG_ERROR("Network", "连接服务器失败");
```

4. **结束时清理**：

```cpp
Logger::finitalize();
```

---

### 5. 集成到其他项目的步骤

1. 将 `Infrastructure/LoggingSystem` 目录加入你的工程：
   - 在 `.pro` 中添加：
     - `SOURCES`：`Logger.cpp`, `LogMessage.cpp`, `ConsoleSink.cpp`, `FileSink.cpp`, `test_logging.cpp`（按需）
     - `HEADERS`：对应的头文件
2. 在应用入口处：
   - 调用 `Logger::initialize()`
   - 创建并注册至少一个输出器（一般是 `ConsoleSink`，可选加 `FileSink`）
3. 在业务代码中通过 `LOG_xxx` 宏记录日志。
4. 程序退出前调用 `Logger::finitalize()` 释放资源。

---

### 6. 后续可扩展方向

- 实现 `FileSink` 的日志轮转（按大小 / 按日期分割）
- 增加 JSON / CSV 格式完整支持（按行输出结构化日志）
- 增加异步日志（使用队列 + 后台线程写文件）
- 增加远程日志 Sink（如通过网络发送到日志服务器）
