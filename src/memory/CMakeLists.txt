# Memory 模块 CMakeLists.txt

cmake_minimum_required(VERSION 3.15)
project(memory)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 包含目录
include_directories(
    ${CMAKE_SOURCE_DIR}/src/utils
)

# 源文件
set(MEMORY_SOURCES
    Arena.cpp
    WALBase.hpp  # 模板类，只有头文件
)

# 头文件
set(MEMORY_HEADERS
    Arena.hpp
    WALBase.hpp
)

# 创建库（WALBase.hpp 是模板类，只需要头文件）
add_library(memory STATIC
    ${MEMORY_SOURCES}
)

# 链接依赖
target_link_libraries(memory
    PUBLIC
        utils
)

target_include_directories(memory PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

