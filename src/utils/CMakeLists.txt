add_library(utils
    Hash.cpp
    Coding.cpp
    Random.cpp
    CRC32.cpp
    Compression.cpp
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
find_package(Crc32c REQUIRED)
find_package(xxHash REQUIRED)
find_package(zstd REQUIRED)

target_link_libraries(utils PUBLIC
    Threads::Threads
    Crc32c::crc32c
    xxHash::xxhash
    zstd::libzstd_static
)


