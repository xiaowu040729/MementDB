cmake_minimum_required(VERSION 3.16)

# 支持 Conan 依赖管理（如果使用 Conan）
# 必须在 project() 之前设置 CMAKE_TOOLCHAIN_FILE
# 优先从 build 目录查找 Conan 生成的文件，如果不存在则从根目录查找（向后兼容）
# 注意：只有在文件存在时才设置 CMAKE_TOOLCHAIN_FILE
set(CONAN_TOOLCHAIN_FILE "")
if(EXISTS ${CMAKE_CURRENT_BINARY_DIR}/conan_toolchain.cmake)
    set(CONAN_TOOLCHAIN_FILE ${CMAKE_CURRENT_BINARY_DIR}/conan_toolchain.cmake)
    # 添加 Conan 生成的配置文件路径（build 目录）
    list(APPEND CMAKE_PREFIX_PATH ${CMAKE_CURRENT_BINARY_DIR})
    message(STATUS "Using Conan toolchain from build directory")
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/conan_toolchain.cmake)
    # 向后兼容：如果 build 目录没有，尝试根目录
    set(CONAN_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/conan_toolchain.cmake)
    list(APPEND CMAKE_PREFIX_PATH ${CMAKE_CURRENT_SOURCE_DIR})
    message(STATUS "Using Conan toolchain from source directory")
else()
    message(STATUS "Conan toolchain not found, building without Conan dependencies")
endif()

# 只有在找到工具链文件时才设置（避免空字符串导致错误）
if(CONAN_TOOLCHAIN_FILE AND NOT CONAN_TOOLCHAIN_FILE STREQUAL "")
    set(CMAKE_TOOLCHAIN_FILE ${CONAN_TOOLCHAIN_FILE})
else()
    # 确保 CMAKE_TOOLCHAIN_FILE 未设置（如果之前被设置过）
    unset(CMAKE_TOOLCHAIN_FILE CACHE)
endif()

project(MementoDB LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 需要线程支持（std::thread），在 Linux 下要显式链接 pthread
find_package(Threads REQUIRED)

# 可选依赖 zstd：优先 Conan/系统，否则自动下载并编译（供 core 与 utils 使用）
find_package(zstd QUIET)
if(NOT zstd_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(ZSTD QUIET libzstd)
        if(ZSTD_FOUND)
            set(zstd_FOUND TRUE)
        endif()
    endif()
endif()
if(NOT zstd_FOUND)
    include(FetchContent)
    set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        zstd
        GIT_REPOSITORY https://github.com/facebook/zstd.git
        GIT_TAG v1.5.5
        SOURCE_SUBDIR build/cmake
    )
    FetchContent_MakeAvailable(zstd)
    if(TARGET libzstd_static)
        set(zstd_FOUND TRUE)
        message(STATUS "zstd: 使用 FetchContent 自动下载并编译 (v1.5.5)")
    endif()
endif()

# 让可执行文件输出到 build/Debug 目录
# 必须在 add_executable() 之前设置
if(NOT CMAKE_CONFIGURATION_TYPES)
    # 单配置生成器（Unix Makefiles 等）
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Debug")
else()
    # 多配置生成器（Visual Studio 等）
    foreach(CONFIG_TYPE ${CMAKE_CONFIGURATION_TYPES})
        string(TOUPPER "${CONFIG_TYPE}" CONFIG_TYPE_UPPER)
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_TYPE_UPPER}
            "${CMAKE_BINARY_DIR}/${CONFIG_TYPE}")
    endforeach()
endif()

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
    net
)

# 确保可执行文件输出到 Debug 目录（显式设置目标属性）
set_target_properties(outDebug PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Debug"
)

# 添加子目录（core、utils、memory、transaction 和 net 库）
add_subdirectory(src/core)
add_subdirectory(src/utils)
add_subdirectory(src/index)
add_subdirectory(src/memory)
add_subdirectory(src/transaction)
add_subdirectory(src/net)

# B+树测试可执行文件（如果文件存在）
# if(EXISTS ${CMAKE_SOURCE_DIR}/src/test_bplustree.cpp)
#     add_executable(test_bplustree
#         src/test_bplustree.cpp
#     )
#     target_link_libraries(test_bplustree PRIVATE core utils Threads::Threads)
# endif()


