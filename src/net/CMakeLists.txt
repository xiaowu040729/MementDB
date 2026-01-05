# Network Module CMakeLists.txt

# 网络模块源文件
set(NET_SOURCES
    IOLoop.cpp
    EpollLoop.cpp
    ThreadPool.cpp
    Protocol.cpp
    Connection.cpp
    Server.cpp
)

# 创建网络库
add_library(net STATIC ${NET_SOURCES})

# 包含目录
target_include_directories(net PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

# 链接依赖
target_link_libraries(net PUBLIC
    core
    utils
    Threads::Threads
)

# 设置C++标准
set_target_properties(net PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

