# 总体架构
mementodb/                      # 项目根目录
├── CMakeLists.txt              # 项目主构建文件
├── CMakePresets.json           # CMake 预设配置
├── conanfile.txt               # 依赖管理 (可选但强烈推荐)
├── Readme.md                   # 项目总览、构建指南、架构图
├── LICENSE                     # 开源许可证 (如 MIT)
├── .gitignore                  # Git忽略文件
│
├── include/mementodb/
│   ├── Client.h
│   ├── Status.h
│   ├── Type.h
│   └── Config.h
│
├── src/                        # 所有源代码
│   ├── core/                    # 核心存储引擎
│   │   ├── CMakeLists.txt
│   │   ├── DiskEngine.hpp/.cpp         # 主存储引擎（基于B+Tree）
│   │   ├── BPlusTree.hpp/.cpp          # B+树实现（核心索引结构）
│   │   ├── Page.hpp/.cpp               # 页结构（4KB/8KB/16KB）
│   │   ├── FileManager.hpp/.cpp        # 文件管理器（页的磁盘IO）
│   │   ├── Record.hpp/.cpp             # 记录/键值对存储格式
│   │   ├── README.md
│   │   ├── StorageEngine.h/.cpp        # ★ 计划：存储引擎抽象基类
│   │   ├── BufferPool.h/.cpp           # ★ 计划：缓冲池管理器
│   │   ├── LRUReplacer.h/.cpp          # ★ 计划：LRU淘汰算法
│   │   └── ClockReplacer.h/.cpp        # ★ 计划：Clock淘汰算法
│   │
│   ├── memory/                  # 独立的内存管理模块
│   │   ├── CMakeLists.txt
│   │   ├── Arena.hpp/.cpp              # 内存分配器
│   │   ├── WALBase.hpp                 # WAL基础类模板
│   │   ├── Arena说明.md
│   │   └── MemoryPool.hpp/.cpp         # ★ 计划：通用的内存池
│   │
│   │
│   ├── net/                     # 网络服务层
│   │   ├── CMakeLists.txt
│   │   ├── Server.h/.cpp        # 主服务器类，事件循环
│   │   ├── Connection.h/.cpp    # 单个连接处理
│   │   ├── Protocol.h/.cpp      # 协议解析 (Redis协议兼容)
│   │   ├── ThreadPool.h/.cpp    # 业务线程池
│   │   ├── EpollLoop.h/.cpp     # ★ 新增：Linux epoll实现
│   │   └── IOLoop.h/.cpp        # ★ 新增：IO事件循环抽象
│   │
│   ├── utils/                   # 基础工具与公共类
│   │   ├── CMakeLists.txt
│   │   ├── Hash.hpp/.cpp               # 哈希函数（支持多种算法）
│   │   ├── CRC32.hpp/.cpp              # CRC32C校验（硬件加速）
│   │   ├── Coding.hpp/.cpp             # 基础编码/解码函数
│   │   ├── Random.hpp/.cpp              # 随机数生成
│   │   ├── Compression.hpp/.cpp        # 压缩工具
│   │   ├── LoggingSystem/              # 日志系统
│   │   │   ├── Logger.hpp/.cpp         # 日志管理器
│   │   │   ├── LogSink.hpp/.cpp        # 日志输出接口
│   │   │   ├── ConsoleSink.hpp/.cpp    # 控制台输出
│   │   │   ├── FileSink.hpp/.cpp       # 文件输出
│   │   │   ├── LogMessage.hpp/.cpp     # 日志消息
│   │   │   ├── LogMacros.hpp           # 日志宏定义
│   │   │   ├── Readme.md
│   │   │   └── 如何测试日志系统.md
│   │   ├── 使用指南.md
│   │   ├── utils详解.md
│   │   ├── ConfigManager.h/.cpp        # ★ 计划：配置管理器
│   │   └── Metrics.h/.cpp              # ★ 计划：性能监控指标
│   │
│   ├── transaction/             # 事务模块
│   │   ├── CMakeLists.txt
│   │   ├── include/                    # 公开接口
│   │   │   ├── Transaction.hpp         # 事务接口定义
│   │   │   ├── IsolationLevel.hpp      # 隔离级别定义
│   │   │   └── WALInterface.hpp        # WAL抽象接口定义
│   │   ├── src/                        # 实现文件
│   │   │   ├── TransactionManager.hpp  # 事务管理器（仅头文件）
│   │   │   ├── TransactionContext.hpp/.cpp  # 事务上下文
│   │   │   ├── LockManager.hpp         # 锁管理器（仅头文件）
│   │   │   ├── LockTable.hpp           # 锁表（仅头文件）
│   │   │   ├── LockTableGraphBuilder.hpp/.cpp  # 死锁检测图构建器
│   │   │   ├── DeadlockDetector.hpp/.cpp      # 死锁检测器
│   │   │   ├── FileWAL.hpp/.cpp                # 文件WAL实现
│   │   │   ├── RecoveryManager.hpp/.cpp       # 恢复管理器
│   │   │   ├── MVCCEngine.hpp/.cpp             # MVCC引擎
│   │   │   ├── Snapshot.hpp/.cpp               # 快照管理
│   │   │   └── VectorCharHash.hpp              # 字节序列哈希工具
│   │   └── tests/                     # 测试文件
│   │       ├── CMakeLists.txt
│   │       ├── TransactionTest.cpp
│   │       ├── LockManagerTest.cpp
│   │       └── WALTest.cpp
│   │
│   ├── index/                   # ★ 新增：多类型索引支持
│   │   ├── CMakeLists.txt
│   │   ├── IndexManager.h/.cpp          # 索引管理器
│   │   ├── BPlusTreeIndex.h/.cpp        # B+树索引（复用core的BPlusTree）
│   │   ├── HashIndex.h/.cpp             # ★ 哈希索引（点查询快）
│   │   ├── SkipListIndex.h/.cpp         # ★ 跳表索引（内存索引）
│   │   └── BloomFilter.h/.cpp           # ★ 布隆过滤器（减少磁盘IO）
│   │
│   ├── cluster/                 # ★ 新增：分布式模块（未来扩展）
│   │   ├── CMakeLists.txt
│   │   ├── ShardManager.h/.cpp          # 分片管理
│   │   ├── ReplicationManager.h/.cpp    # 复制管理
│   │   └── Membership.h/.cpp            # 集群成员管理
│   └── main.cpp                # 程序入口点
│
├── build/                       # 构建输出目录（gitignore，Conan生成文件也在此）
│
├── logs/                        # 日志目录（gitignore）
│
├── thirdparty/                  # ★ 建议使用第三方库（可选）
│   ├── catch2/                  # 单元测试框架
│   ├── spdlog/                  # 日志库（可选）
│   └── benchmark/               # 性能测试框架
│
├── tests/
│   ├── CMakeLists.txt
│   ├── test_runner.cpp
│   ├── unit/
│   │   ├── test_b_plus_tree.cpp
│   │   ├── test_buffer_pool.cpp   # ★ 新增：缓冲池测试
│   │   ├── test_wal.cpp
│   │   ├── test_index.cpp         # ★ 新增：索引测试
│   │   └── ...
│   └── integration/
│       └── test_server_client.cpp
│
├── benchmarks/
│   ├── CMakeLists.txt
│   ├── benchmark_runner.cpp
│   ├── benchmark_set_get.cpp
│   ├── benchmark_transaction.cpp  # ★ 新增：事务性能测试
│   └── benchmark_buffer_pool.cpp  # ★ 新增：缓冲池性能测试
│
├── examples/
│   ├── CMakeLists.txt
│   ├── simple_client.cpp
│   ├── transaction_example.cpp     # ★ 新增：事务使用示例
│   └── example_usage.c
│
└── docs/
    ├── ARCHITECTURE.md
    ├── STORAGE_ENGINE.md
    ├── TRANSACTION.md
    ├── NETWORK_PROTOCOL.md
    ├── BUILD_GUIDE.md
    ├── BUFFER_POOL_DESIGN.md       # ★ 新增：缓冲池设计文档
    ├── MVCC_DESIGN.md              # ★ 新增：MVCC设计文档
    └── BENCHMARK_RESULT.md

## 快速构建指南

### 前置要求
- CMake 3.16+
- Conan 2.x
- C++20 编译器

### 构建步骤

1. **安装依赖（使用 Conan）**
   ```bash
   # 重要：使用 --output-folder 参数将生成的文件放到 build 目录
   conan install . --output-folder=build --build=missing
   ```

2. **配置 CMake**
   ```bash
   cmake -S . -B build
   ```

3. **编译**
   ```bash
   cmake --build build -j4
   ```

**注意**：Conan 生成的文件（`*.cmake`、`conanbuild.sh` 等）会自动输出到 `build/` 目录，不会污染项目根目录。