# Network Module Tests CMakeLists.txt

# 测试可执行文件 - 网络模块综合测试
add_executable(net_module_test
    NetModuleTest.cpp
)

# 包含目录
target_include_directories(net_module_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

# 链接依赖
target_link_libraries(net_module_test PRIVATE
    net
    core
    utils
    memory
    Threads::Threads
)

# 设置C++标准
set_target_properties(net_module_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

# 如果存在 ServerTest.cpp，也添加它
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ServerTest.cpp")
    add_executable(server_test
        ServerTest.cpp
    )
    
    target_include_directories(server_test PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
    )
    
    target_link_libraries(server_test PRIVATE
        net
        core
        utils
        memory
        Threads::Threads
    )
    
    set_target_properties(server_test PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
    )
endif()

