# Index 模块独立构建配置（不依赖 Conan）
cmake_minimum_required(VERSION 3.16)
project(IndexTest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 添加 include 目录
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

# 创建索引库
add_library(index_lib
    HashIndex.cpp
    SkipListIndex.cpp
    BloomFilter.cpp
    IndexManager.cpp
)

target_include_directories(index_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

# 链接依赖（只链接 utils 中的 Hash，不依赖其他）
# 我们需要编译 Hash.cpp
add_library(utils_hash STATIC
    ${CMAKE_SOURCE_DIR}/src/utils/Hash.cpp
)

target_include_directories(utils_hash PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(index_lib PUBLIC utils_hash)

# 测试程序
add_executable(test_index test_index.cpp)
target_link_libraries(test_index PRIVATE index_lib)
target_compile_features(test_index PRIVATE cxx_std_17)

