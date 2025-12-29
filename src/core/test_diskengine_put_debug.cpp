// 一个用于单独调试 DiskEngineV2::put/get 的最小测试程序
// 目的：绕过大测试框架，专门看 put/get 在当前环境下哪里失败。

#include "DiskEngine.hpp"
#include "Record.hpp"
#include "mementodb/Types.h"
#include "../utils/LoggingSystem/Logger.hpp"
#include "../utils/LoggingSystem/ConsoleSink.hpp"
#include "../utils/LoggingSystem/LogMacros.hpp"
#include <iostream>
#include <filesystem>
#include <chrono>

using namespace mementodb::core;
using namespace LoggingSystem;

int main() {
    // 初始化日志系统
    Logger::initialize();
    Logger* logger = Logger::GetInstance();
    auto consoleSink = std::make_shared<ConsoleSink>();
    consoleSink->setMinimumLogLevel(LogLevel::DEBUG);
    logger->addSink(consoleSink);
    logger->setMinimumLogLevel(LogLevel::DEBUG);
    
    LOG_INFO("DEBUG_TEST", "日志系统已初始化");
    
    std::string test_dir = "/tmp/mementodb_put_debug";
    std::filesystem::remove_all(test_dir);
    std::filesystem::create_directories(test_dir);

    std::cout << "==== DiskEngineV2 put/get 调试测试 ====" << std::endl;
    std::cout << "测试目录: " << test_dir << std::endl;

    // 使用尽量“保守”的配置，先关掉高阶特性，排除环境依赖问题
    EngineConfig config;
    config.buffer_pool_size = 128;    // 小一点就够了
    config.enable_wal = false;        // 先关 WAL，简化路径
    config.use_direct_io = false;
    config.use_mmap = false;
    config.use_io_uring = false;
    config.use_zero_copy = false;
    config.enable_metrics = false;

    try {
        DiskEngineV2 engine(test_dir, config);
        std::cout << "[INFO] 引擎创建成功" << std::endl;

        mementodb::Slice key("debug:key:001");
        mementodb::Slice value("debug_value_001");

        std::cout << "[STEP] 调用 put..." << std::endl;
        LOG_DEBUG("DEBUG_TEST", "准备调用 put: key=" + std::string(key.data(), key.size()) + 
                  ", value=" + std::string(value.data(), value.size()));
        
        auto put_future = engine.put(key, value);

        // 加一个超时，避免卡死
        auto status = put_future.wait_for(std::chrono::seconds(5));
        if (status != std::future_status::ready) {
            std::cout << "[ERROR] put 超时 (5s 内 future 未就绪)" << std::endl;
            LOG_ERROR("DEBUG_TEST", "put 操作超时");
            return 1;
        }

        bool put_ok = put_future.get();
        std::cout << "[RESULT] put 返回值: " << (put_ok ? "true" : "false") << std::endl;
        LOG_INFO("DEBUG_TEST", "put 操作完成，返回值: " + std::to_string(put_ok));

        if (!put_ok) {
            std::cout << "[HINT] put 返回 false，详细原因请查看上面的日志 (DATABASE 模块)" << std::endl;
            std::cout << "[HINT] 请检查是否有以下错误：" << std::endl;
            std::cout << "  - Failed to allocate extent" << std::endl;
            std::cout << "  - Failed to write page" << std::endl;
            std::cout << "  - Failed to store record" << std::endl;
            return 2;
        }

        std::cout << "[STEP] 调用 get..." << std::endl;
        auto get_future = engine.get(key);
        status = get_future.wait_for(std::chrono::seconds(5));
        if (status != std::future_status::ready) {
            std::cout << "[ERROR] get 超时 (5s 内 future 未就绪)" << std::endl;
            return 3;
        }

        auto get_result = get_future.get();
        if (!get_result.has_value()) {
            std::cout << "[ERROR] get 返回空，未找到 key" << std::endl;
            return 4;
        }

        std::string got = get_result.value().ToString();
        std::cout << "[RESULT] get 返回值: \"" << got << "\"" << std::endl;

        if (got != "debug_value_001") {
            std::cout << "[ERROR] get 返回内容与写入不一致" << std::endl;
            return 5;
        }

        std::cout << "[OK] put/get 在精简配置下通过" << std::endl;
        std::cout.flush();

        // 等待一小段时间，确保所有异步任务完成
        std::cout << "[STEP] 等待异步任务完成..." << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::cout << "[INFO] 准备退出" << std::endl;
        std::cout.flush();

        // 清理
        std::filesystem::remove_all(test_dir);
        LOG_INFO("DEBUG_TEST", "退出完成");
        return 0;
    } catch (const std::exception& e) {
        std::cout << "[EXCEPTION] " << e.what() << std::endl;
        return 10;
    }
}


