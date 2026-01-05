# Transaction 模块 CMakeLists.txt

cmake_minimum_required(VERSION 3.15)
project(transaction)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 包含目录
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/utils
    ${CMAKE_SOURCE_DIR}/src/memory
    ${CMAKE_SOURCE_DIR}/src/core
)

# 源文件（只包含实际存在的 .cpp 文件）
set(TRANSACTION_SOURCES
    src/FileWAL.cpp
    src/MVCCEngine.cpp
    src/Snapshot.cpp
    src/DeadlockDetector.cpp
    src/LockTableGraphBuilder.cpp
)

# 检查其他源文件是否存在，如果存在则添加
foreach(SOURCE_FILE 
    src/TransactionManager.cpp
    src/TransactionContext.cpp
    src/LockManager.cpp
    src/LockTable.cpp
    src/RecoveryManager.cpp
    src/DeadlockDetector.cpp
    src/LockTableGraphBuilder.cpp
)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE_FILE})
        list(APPEND TRANSACTION_SOURCES ${SOURCE_FILE})
    endif()
endforeach()

# 头文件
set(TRANSACTION_HEADERS
    include/Transaction.hpp
    include/IsolationLevel.hpp
    include/WALInterface.hpp
    src/TransactionManager.hpp
    src/TransactionContext.hpp
    src/LockManager.hpp
    src/LockTable.hpp
    src/FileWAL.hpp
    src/RecoveryManager.hpp
    src/DeadlockDetector.hpp
    src/LockTableGraphBuilder.hpp
    src/MVCCEngine.hpp
    src/Snapshot.hpp
)

# 创建库
add_library(transaction STATIC
    ${TRANSACTION_SOURCES}
    ${TRANSACTION_HEADERS}
)

# 链接依赖
target_link_libraries(transaction
    PUBLIC
        memory
        utils
        core
)

# 测试（可选）
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

