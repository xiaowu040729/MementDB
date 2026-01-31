add_library(utils
    Hash.cpp
    Coding.cpp
    Random.cpp
    CRC32.cpp
    Compression.cpp
    ConfigManager.cpp
    Metrics.cpp
    MetricsCollector.cpp
    LoggingSystem/Logger.cpp
    LoggingSystem/ConsoleSink.cpp
    LoggingSystem/FileSink.cpp
    LoggingSystem/LogSink.cpp
    LoggingSystem/LogMessage.cpp
)

target_include_directories(utils PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

# 第三方库的包含目录会自动通过 target_link_libraries 传递

# 查找第三方库（Conan 2.x 生成的大小写敏感）
# 如果找不到，则设为可选（用于测试）
find_package(Crc32c QUIET)
find_package(xxHash QUIET)
find_package(zstd QUIET)

# 若 Conan 未提供 zstd，尝试系统包（如 libzstd-dev）
if(NOT zstd_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(ZSTD QUIET libzstd)
        if(ZSTD_FOUND)
            set(zstd_FOUND TRUE)
        endif()
    endif()
endif()
# 若根目录已通过 FetchContent 提供 zstd，直接使用
if(NOT zstd_FOUND AND TARGET libzstd_static)
    set(zstd_FOUND TRUE)
endif()

target_link_libraries(utils PUBLIC
    Threads::Threads
)

# 如果找到了第三方库，则链接它们
if(Crc32c_FOUND)
    target_link_libraries(utils PUBLIC Crc32c::crc32c)
    target_compile_definitions(utils PUBLIC HAVE_CRC32C)
endif()

if(xxHash_FOUND)
    target_link_libraries(utils PUBLIC xxHash::xxhash)
    target_compile_definitions(utils PUBLIC HAVE_XXHASH)
endif()

if(zstd_FOUND)
    target_compile_definitions(utils PUBLIC HAVE_ZSTD)
    if(TARGET zstd::libzstd_static)
        target_link_libraries(utils PUBLIC zstd::libzstd_static)
    elseif(TARGET libzstd_static)
        # FetchContent 提供的 zstd：链接静态库并添加头文件路径
        target_link_libraries(utils PUBLIC libzstd_static)
        target_include_directories(utils PUBLIC ${zstd_SOURCE_DIR}/lib)
    else()
        target_link_libraries(utils PUBLIC ${ZSTD_LIBRARIES})
        target_include_directories(utils PUBLIC ${ZSTD_INCLUDE_DIRS})
    endif()
endif()


