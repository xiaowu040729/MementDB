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
    target_link_libraries(utils PUBLIC zstd::libzstd_static)
    target_compile_definitions(utils PUBLIC HAVE_ZSTD)
endif()


