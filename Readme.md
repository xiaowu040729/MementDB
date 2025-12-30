# 总体架构
mementodb/                      # 项目根目录
├── CMakeLists.txt              # 项目主构建文件
├── conanfile.txt               # 依赖管理 (可选但强烈推荐)
├── README.md                   # 项目总览、构建指南、架构图
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
│   │   ├── DiskEngine.h/.cpp           # 主存储引擎（基于B+Tree）
│   │   ├── StorageEngine.h/.cpp        # ★ 存储引擎抽象基类
│   │   ├── Page.h/.cpp                 # 页结构（4KB/8KB/16KB）
│   │   ├── BPlusTree.h/.cpp            # B+树实现（核心索引结构）
│   │   ├── FileManager.h/.cpp          # 文件管理器（页的磁盘IO）
│   │   ├── Record.h/.cpp               # 记录/键值对存储格式
│   │   ├── BufferPool.h/.cpp           # ★ 新增：缓冲池管理器
│   │   ├── LRUReplacer.h/.cpp          # ★ 新增：LRU淘汰算法
│   │   └── ClockReplacer.h/.cpp        # ★ 新增：Clock淘汰算法
│   │
│   ├── memory/                  # 独立的内存管理模块
│   │   ├── CMakeLists.txt
│   │   ├── Arena.hpp/.cpp       # 内存分配器
│   │   ├── MemoryPool.hpp/.cpp  # ★ 增强：通用的内存池
│   │   └── WALBase.hpp          # WAL基础类模板
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
│   │   ├── Logger.h/.cpp        # ★ 改为正式类
│   │   ├── ConfigManager.h/.cpp # ★ 新增：配置管理器
│   │   ├── Hash.h/.cpp          # 哈希函数
│   │   ├── Coding.h/.cpp        # 基础编码/解码函数
│   │   ├── Random.h/.cpp        # 随机数
│   │   ├── CRC32.h/.cpp         # ★ 新增：CRC校验
│   │   └── Metrics.h/.cpp       # ★ 新增：性能监控指标
│   │
│   ├── transaction/             # 事务模块
│   │   ├── CMakeLists.txt
│   │   ├── include/                    # 公开接口
│   │   │   ├── Transaction.hpp         # 事务接口定义
│   │   │   ├── IsolationLevel.hpp       # 隔离级别定义
│   │   │   └── WALInterface.hpp         # WAL抽象接口定义
│   │   ├── src/
│   │   │   ├── TransactionManager.hpp/.cpp
│   │   │   ├── TransactionContext.hpp/.cpp
│   │   │   ├── LockManager.hpp/.cpp
│   │   │   ├── LockTable.hpp/.cpp
│   │   │   ├── FileWAL.hpp/.cpp
│   │   │   ├── RecoveryManager.hpp/.cpp
│   │   │   ├── DeadlockDetector.hpp/.cpp
│   │   │   ├── MVCCEngine.hpp/.cpp      # ★ 新增：MVCC实现
│   │   │   └── Snapshot.hpp/.cpp        # ★ 新增：快照管理
│   │   └── tests/
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