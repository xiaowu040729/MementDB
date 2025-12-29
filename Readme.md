# 总体架构
mementodb/                      # 项目根目录
├── CMakeLists.txt              # 项目主构建文件
├── conanfile.txt               # 依赖管理 (可选但强烈推荐)
├── README.md                   # 项目总览、构建指南、架构图
├── LICENSE                     # 开源许可证 (如 MIT)
├── .gitignore                  # Git忽略文件
│
├── include/                    # 公共头文件 (对外暴露的API)
│   └── mementodb/
│       ├── Client.h            # 客户端连接API
│       ├── Status.h            # 统一返回状态对象
│       └── Type.h              # 基础类型定义 (Key, Value, Slice等)
│
├── src/                        # 所有源代码
│   ├── core/                   # 最核心的引擎实现
│   │   ├── CMakeLists.txt                 # 构建配置
│   │   ├── DiskEngine.h/.cpp              # 磁盘存储引擎主类（对外接口）
│   │   ├── Page.h/.cpp                    # 页结构（磁盘/内存基本单元）
│   │   ├── BPlusTree.h/.cpp               # B+树实现（核心索引结构）
│   │   ├── FileManager.h/.cpp             # 文件管理器（页的磁盘IO）
│   │   └── Record.h/.cpp                  # 记录/键值对存储格式（可选，后续添加）
│   │
│   ├── memory/                 # 独立的内存管理模块
│   │   ├── CMakeLists.txt
│   │   └── Arena.h/.cpp        # 内存分配器，减少碎片
│   │
│   │
│   ├── net/                    # 网络服务层
│   │   ├── CMakeLists.txt
│   │   ├── Server.h/.cpp       # 主服务器类，事件循环
│   │   ├── Connection.h/.cpp   # 单个连接处理
│   │   ├── Protocol.h/.cpp     # 协议解析 (Redis协议兼容)
│   │   └── ThreadPool.h/.cpp   # 业务线程池
│   │
│   ├── utils/                  # 基础工具与公共类
│   │   ├── CMakeLists.txt
│   │   ├── LoggingSystem       #日志系统
│   │   ├── Hash.h              # 哈希函数
│   │   ├── Coding.h/.cpp       # 基础编码/解码函数
│   │   └── Random.h/.cpp       # 随机数
│   │
│   └── main.cpp                # 程序入口点
│
├── lib/                        # 可能依赖的第三方库源码 (如用于测试的catch2)
│
├── tests/                      # 单元测试与集成测试
│   ├── CMakeLists.txt
│   ├── test_runner.cpp         # 测试主入口
│   ├── unit/                   # 单元测试
│   │   ├── test_b_plus_tree.cpp
│   │   ├── test_wal.cpp
│   │   └── ...
│   └── integration/            # 集成测试
│       └── test_server_client.cpp
│
├── benchmarks/                 # 性能基准测试 (使用google benchmark)
│   ├── CMakeLists.txt
│   ├── benchmark_runner.cpp
│   ├── benchmark_set_get.cpp
│   └── ...
│
├── examples/                   # 使用示例
│   ├── CMakeLists.txt
│   ├── simple_client.cpp       # 简单C++客户端示例
│   └── example_usage.c
│
└── docs/                       # 设计文档 (至关重要!)
    ├── ARCHITECTURE.md         # 详细架构说明，可放入之前的架构图
    ├── STORAGE_ENGINE.md       # B+树与WAL设计细节
    ├── TRANSACTION.md          # 事务与锁设计细节
    ├── NETWORK_PROTOCOL.md     # 网络协议定义
    ├── BUILD_GUIDE.md          # 详细的构建指南
    └── BENCHMARK_RESULT.md     # 性能测试结果与分析