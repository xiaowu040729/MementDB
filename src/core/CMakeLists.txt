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

# 功能测试可执行文件
add_executable(test_core_functional
    test_core_functional.cpp
    ${CMAKE_SOURCE_DIR}/src/memory/Arena.cpp
)

target_link_libraries(test_core_functional PRIVATE
    core
    utils
    Threads::Threads
)

target_include_directories(test_core_functional PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

# DiskEngine put/get 调试可执行文件
add_executable(test_diskengine_put_debug
    test_diskengine_put_debug.cpp
    ${CMAKE_SOURCE_DIR}/src/memory/Arena.cpp
)

target_link_libraries(test_diskengine_put_debug PRIVATE
    core
    utils
    Threads::Threads
)

target_include_directories(test_diskengine_put_debug PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

