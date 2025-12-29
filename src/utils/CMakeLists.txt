add_library(utils
    Hash.cpp
    Coding.cpp
    Random.cpp
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

target_link_libraries(utils PRIVATE Threads::Threads)


