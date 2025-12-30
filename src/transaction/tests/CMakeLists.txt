# Transaction 模块测试 CMakeLists.txt

cmake_minimum_required(VERSION 3.15)

# 测试可执行文件
add_executable(transaction_test TransactionTest.cpp)
target_link_libraries(transaction_test
    transaction
    utils
    memory
    core
)

add_executable(lock_manager_test LockManagerTest.cpp)
target_link_libraries(lock_manager_test
    transaction
    utils
    memory
    core
)

add_executable(wal_test WALTest.cpp)
target_link_libraries(wal_test
    transaction
    utils
    memory
    core
)

# 如果使用 C++17 的 filesystem，需要链接 filesystem 库
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "9.0")
    target_link_libraries(transaction_test stdc++fs)
    target_link_libraries(lock_manager_test stdc++fs)
    target_link_libraries(wal_test stdc++fs)
endif()

