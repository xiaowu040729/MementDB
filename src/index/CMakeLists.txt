# Index 模块构建配置

# 创建索引库
add_library(index
    HashIndex.cpp
    SkipListIndex.cpp
    BloomFilter.cpp
    IndexManager.cpp
)

target_include_directories(index PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

# 链接依赖
target_link_libraries(index PUBLIC
    utils
)

# 如果需要 C++17 特性
target_compile_features(index PUBLIC cxx_std_17)

# 测试程序
add_executable(test_index test_index.cpp)
target_link_libraries(test_index PRIVATE index)
target_compile_features(test_index PRIVATE cxx_std_17)

