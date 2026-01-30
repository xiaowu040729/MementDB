##整体架构##
mementodb/                          # 项目根目录（当前实际目录结构）
├── .gitignore                      # Git 忽略规则
├── build_index/                    # 索引相关的独立构建目录
│   └── CMakeLists.txt
├── CMakeLists.txt                  # 项目主构建文件
├── CMakeUserPresets.json           # 本地 CMake 用户预设
├── conanfile.txt                   # Conan 依赖管理配置
├── config.example.conf             # 服务器配置示例
├── data/                           # 运行时数据目录
│   ├── data/
│   │   └── extent_table.bin        # 示例：主数据/元数据文件
│   └── wal/
│       └── wal_state.bin           # 示例：WAL 状态
├── include/
│   └── mementodb/
│       ├── Client.hpp              # C++ 客户端接口
│       └── Types.h                 # 公共类型/别名定义
├── mementodb.pid                   # 运行中实例的 PID 文件
├── src/                            # 所有 C++ 源代码
│   ├── core/                       # 核心存储引擎
│   │   ├── CMakeLists.txt
│   │   ├── BPlusTree.hpp/.cpp      # B+ 树实现（核心索引结构）
│   │   ├── DiskEngine.hpp/.cpp     # 磁盘引擎，封装对文件/页的访问
│   │   ├── FileManager.hpp/.cpp    # 页的磁盘 IO 管理
│   │   ├── Page.hpp/.cpp           # 页结构
│   │   ├── Record.hpp/.cpp         # 记录/键值对存储格式
│   │   ├── BufferPool.h/.cpp       # 缓冲池实现
│   │   ├── BufferPool_Example.cpp  # 缓冲池示例代码
│   │   ├── BufferPool_Enhanced.cpp # 缓冲池增强版本
│   │   └── README.md
│   ├── index/                      # 多类型索引支持
│   │   ├── CMakeLists.txt
│   │   ├── CMakeLists_standalone.txt
│   │   ├── IIndex.hpp              # 索引抽象接口
│   │   ├── IndexManager.hpp/.cpp   # 索引管理器
│   │   ├── HashIndex.hpp/.cpp      # 哈希索引
│   │   ├── SkipListIndex.hpp/.cpp  # 跳表索引
│   │   ├── BloomFilter.hpp/.cpp    # 布隆过滤器
│   │   ├── test_index.cpp          # 索引模块测试程序
│   │   ├── test_index              # 示例：索引测试可执行文件
│   │   ├── COMPILE_STATUS.md       # 索引模块编译状态说明
│   │   └── TEST_INSTRUCTIONS.md    # 索引测试说明
│   ├── memory/                     # 内存管理模块
│   │   ├── CMakeLists.txt
│   │   ├── Arena.hpp/.cpp          # Arena 内存分配器
│   │   ├── WALBase.hpp             # WAL 相关的基础模板/抽象
│   │   └── Arena说明.md
│   ├── net/                        # 网络服务层
│   │   ├── CMakeLists.txt
│   │   ├── Server.hpp/.cpp         # 主服务器类，事件循环
│   │   ├── Connection.hpp/.cpp     # 单个连接处理
│   │   ├── Protocol.hpp/.cpp       # 协议解析（兼容 Redis 协议）
│   │   ├── ThreadPool.hpp/.cpp     # 业务线程池
│   │   ├── EpollLoop.hpp/.cpp      # 基于 epoll 的 IO 多路复用
│   │   ├── IOLoop.hpp/.cpp         # IO 事件循环抽象
│   │   ├── ServerConfig.hpp/.cpp   # 服务器配置加载
│   │   ├── tests/                  # 网络模块测试
│   │   │   ├── CMakeLists.txt
│   │   │   └── NetModuleTest.cpp
│   │   └── ARCHITECTURE.md         # 网络模块架构说明
│   ├── transaction/                # 事务模块
│   │   ├── CMakeLists.txt
│   │   ├── include/                # 对外事务/WAL 接口
│   │   │   ├── Transaction.hpp
│   │   │   ├── IsolationLevel.hpp
│   │   │   └── WALInterface.hpp
│   │   ├── src/                    # 事务内部实现
│   │   │   ├── TransactionManager.hpp/.cpp
│   │   │   ├── TransactionContext.hpp/.cpp
│   │   │   ├── LockManager.hpp/.cpp
│   │   │   ├── LockTable.hpp/.cpp
│   │   │   ├── LockTableGraphBuilder.hpp/.cpp
│   │   │   ├── DeadlockDetector.hpp/.cpp
│   │   │   ├── FileWAL.hpp/.cpp
│   │   │   ├── RecoveryManager.hpp/.cpp
│   │   │   ├── MVCCEngine.hpp/.cpp
│   │   │   ├── Snapshot.hpp/.cpp
│   │   │   └── VectorCharHash.hpp
│   │   ├── tests/                  # 事务模块单元测试
│   │   │   ├── CMakeLists.txt
│   │   │   ├── LockManagerTest.cpp
│   │   │   ├── TransactionModuleTest.cpp
│   │   │   ├── TransactionTest.cpp
│   │   │   ├── WALTest.cpp
│   │   │   └── README.md
│   ├── utils/                      # 工具与公共组件
│   │   ├── CMakeLists.txt
│   │   ├── Hash.hpp/.cpp           # 哈希函数
│   │   ├── CRC32.hpp/.cpp          # CRC32C 校验
│   │   ├── Coding.hpp/.cpp         # 编码/解码
│   │   ├── Compression.hpp/.cpp    # 压缩工具
│   │   ├── Random.hpp/.cpp         # 随机数生成
│   │   ├── ConfigManager.hpp/.cpp  # 配置管理器
│   │   ├── ConfigManager_Example.cpp
│   │   ├── ConfigManager_Advanced_Example.cpp
│   │   ├── Metrics.h/.cpp          # 性能指标采集
│   │   ├── Metrics_Old.cpp
│   │   ├── Metrics_New.cpp
│   │   ├── Metrics_Example.cpp
│   │   ├── MetricsCollector.h/.cpp
│   │   ├── LoggingSystem/          # 日志子系统
│   │   │   ├── Logger.hpp/.cpp
│   │   │   ├── LogSink.hpp/.cpp
│   │   │   ├── ConsoleSink.hpp/.cpp
│   │   │   ├── FileSink.hpp/.cpp
│   │   │   ├── LogMessage.hpp/.cpp
│   │   │   ├── LogMacros.hpp
│   │   │   ├── Readme.md
│   │   │   └── 如何测试日志系统.md
│   │   ├── 使用指南.md
│   │   └── utils详解.md
│   └── main.cpp                    # 程序入口
├── start.sh                        # 启动脚本
├── stop.sh                         # 停止脚本
├── Readme.md                       # 项目总览
├── 使用说明以帮助快速开始.md      # 快速入门说明
└── 整体架构以及流程.md            # 本文件（项目结构与流程说明）



# MementoDB 快速开始指南

## 一键启动

**在任何 Linux 系统上，只需运行：**

```bash
./start.sh
```

脚本会自动：
1. ✅ 检测系统类型（Ubuntu/Debian/CentOS/Arch等）
2. ✅ 检查必需的依赖（CMake、编译器）
3. ✅ 提供缺失依赖的安装命令
4. ✅ 自动构建项目
5. ✅ 启动数据库服务器

## 系统要求

### 必需依赖

- **CMake** 3.16 或更高版本
- **C++ 编译器** (g++ 或 clang++)
- **make** (通常随编译器一起安装)

### 可选依赖

- **Conan** (用于依赖管理，可选)

## 自动安装依赖

如果缺少依赖，脚本会显示安装命令。根据你的系统类型：

### Ubuntu/Debian
```bash
sudo apt-get update
sudo apt-get install -y cmake build-essential
```

### CentOS/RHEL/Fedora
```bash
sudo dnf install -y cmake gcc-c++ make
# 或使用 yum (旧版本)
sudo yum install -y cmake gcc-c++ make
```

### Arch Linux
```bash
sudo pacman -S --noconfirm cmake base-devel
```

### 安装 Conan (可选)
```bash
pip3 install conan
```

## 使用示例

### 1. 首次启动（自动构建）
```bash
./start.sh
```

### 2. 前台运行（查看实时日志）
```bash
./start.sh --foreground
```

### 3. 只构建，不启动
```bash
./start.sh --build-only
```

### 4. 直接启动（跳过构建）
```bash
./start.sh --no-build
```

## 连接数据库

服务器启动后，监听在 `127.0.0.1:6380`

### 使用 redis-cli
```bash
redis-cli -p 6380
```

### 使用 telnet
```bash
telnet 127.0.0.1 6380
```

### 基本操作
```bash
127.0.0.1:6380> SET mykey "Hello"
OK
127.0.0.1:6380> GET mykey
"Hello"
127.0.0.1:6380> EXISTS mykey
(integer) 1
127.0.0.1:6380> DEL mykey
(integer) 1
```

## 停止服务器

```bash
./stop.sh
```

或如果在前台运行，按 `Ctrl+C`

## 查看日志

```bash
tail -f logs/server.log
```
