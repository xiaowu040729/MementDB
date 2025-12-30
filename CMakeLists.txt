cmake_minimum_required(VERSION 3.16)

project(MementoDB LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 支持 Conan 依赖管理（如果使用 Conan）
# 注意：toolchain 文件应该在项目根目录，而不是 build 目录
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/conan_toolchain.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/conan_toolchain.cmake)
    # 添加 Conan 生成的配置文件路径
    list(APPEND CMAKE_PREFIX_PATH ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# 需要线程支持（std::thread），在 Linux 下要显式链接 pthread
find_package(Threads REQUIRED)

# 可执行文件名称：outDebug（对应 launch.json 里的 program）
# 注意：源文件在 src 目录下
add_executable(outDebug
    src/main.cpp
    src/core/Page.cpp
    src/core/Record.cpp
    src/utils/Coding.cpp
    src/utils/Hash.cpp
    src/utils/Random.cpp
    src/memory/Arena.cpp
    src/utils/LoggingSystem/Logger.cpp
    src/utils/LoggingSystem/ConsoleSink.cpp
    src/utils/LoggingSystem/FileSink.cpp
    src/utils/LoggingSystem/LogSink.cpp
    src/utils/LoggingSystem/LogMessage.cpp
)

# 添加 include 目录
target_include_directories(outDebug PRIVATE 
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(outDebug PRIVATE 
    Threads::Threads
    utils
    core
    memory
    transaction
)

# 添加子目录（core、utils、memory 和 transaction 库）
add_subdirectory(src/core)
add_subdirectory(src/utils)
add_subdirectory(src/memory)
add_subdirectory(src/transaction)

# B+树测试可执行文件（如果文件存在）
# if(EXISTS ${CMAKE_SOURCE_DIR}/src/test_bplustree.cpp)
#     add_executable(test_bplustree
#         src/test_bplustree.cpp
#     )
#     target_link_libraries(test_bplustree PRIVATE core utils Threads::Threads)
# endif()

# 让可执行文件输出到 build/Debug 目录
if(NOT CMAKE_CONFIGURATION_TYPES)
    # 单配置生成器（MinGW Makefiles 等）
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Debug")
else()
    # 多配置生成器（Visual Studio 等）
    foreach(CONFIG_TYPE ${CMAKE_CONFIGURATION_TYPES})
        string(TOUPPER "${CONFIG_TYPE}" CONFIG_TYPE_UPPER)
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_TYPE_UPPER}
            "${CMAKE_BINARY_DIR}/${CONFIG_TYPE}")
    endforeach()
endif()


