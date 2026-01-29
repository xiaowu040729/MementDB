# Core 模块构建配置

add_library(core
    Page.cpp
    FileManager.cpp
    BPlusTree.cpp
    DiskEngine.cpp
    Record.cpp
    BufferPool_Enhanced.cpp
    # BufferPool.cpp  # Temporarily disabled - class definitions missing in header
)

target_include_directories(core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

# 查找 ZSTD 库
find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
    pkg_check_modules(ZSTD QUIET libzstd)
endif()

# 如果 pkg-config 没找到，尝试 find_library
if (NOT ZSTD_FOUND)
    find_library(ZSTD_LIBRARY
        NAMES zstd libzstd
        PATHS
            /usr/lib
            /usr/local/lib
            /opt/homebrew/lib
    )
    
    find_path(ZSTD_INCLUDE_DIR
        NAMES zstd.h
        PATHS
            /usr/include
            /usr/local/include
            /opt/homebrew/include
    )
    
    if (ZSTD_LIBRARY AND ZSTD_INCLUDE_DIR)
        set(ZSTD_FOUND TRUE)
        set(ZSTD_LIBRARIES ${ZSTD_LIBRARY})
        set(ZSTD_INCLUDE_DIRS ${ZSTD_INCLUDE_DIR})
    endif()
endif()

# 如果找到了 ZSTD，添加定义和链接
if (ZSTD_FOUND)
    message(STATUS "Found ZSTD: ${ZSTD_LIBRARIES}")
    add_definitions(-DHAVE_ZSTD)
    target_include_directories(core PRIVATE ${ZSTD_INCLUDE_DIRS})
else()
    message(WARNING "ZSTD library not found. Compression features will be disabled.")
    message(WARNING "Install zstd development package: sudo apt-get install libzstd-dev (Ubuntu/Debian)")
    message(WARNING "or: brew install zstd (macOS)")
endif()

# 链接依赖
target_link_libraries(core PRIVATE
    Threads::Threads
)

# 如果找到了 ZSTD，链接它
if (ZSTD_FOUND)
    target_link_libraries(core PRIVATE ${ZSTD_LIBRARIES})
endif()

# 测试可执行文件已移除

