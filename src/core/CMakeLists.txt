# Core 模块构建配置

add_library(core
    Page.cpp
    FileManager.cpp
    BPlusTree.cpp
    DiskEngine.cpp
    Record.cpp
)

target_include_directories(core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

# 链接依赖
target_link_libraries(core PRIVATE
    Threads::Threads
)

# 测试可执行文件已移除

