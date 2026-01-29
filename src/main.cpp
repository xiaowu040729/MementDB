// File: src/main.cpp

#include <iostream>
#include <memory>
#include "core/DiskEngine.hpp"
#include "net/Server.hpp"

using namespace mementodb;

int main() {
    std::cout << "=== MementoDB Server Test ===" << std::endl;
    try {
        std::cout << "1. Creating engine config..." << std::endl;
        // 创建存储引擎配置（关闭 WAL/直接IO/uring，避免卡住）
        core::EngineConfig engine_cfg;
        engine_cfg.use_direct_io = false;
        engine_cfg.use_mmap = false;
        engine_cfg.use_io_uring = false;
        engine_cfg.enable_wal = false;
        engine_cfg.buffer_pool_size = 1024 * 64; // 64K pages (~256MB)
        
        std::cout << "2. Creating server config..." << std::endl;
        // 创建默认服务器配置
        net::ServerConfig server_cfg;
        if (server_cfg.listen_configs.empty()) {
            net::ServerConfig::ListenConfig lc;
            lc.host = "127.0.0.1";
            lc.port = 6380; // 避免与本地已有服务冲突
            server_cfg.listen_configs.push_back(lc);
        }

        std::cout << "3. Creating engine..." << std::endl;
        // 直接使用 make_shared 构造，传入构造参数而不是对象
        auto engine_ptr = std::make_shared<core::DiskEngineV2>("./data", engine_cfg);
        
        std::cout << "4. Creating server..." << std::endl;
        auto server = net::Server::create(engine_ptr, server_cfg);

        std::cout << "5. Running server (press Ctrl+C to stop)..." << std::endl;
        // 前台常驻运行，直到收到退出信号
        server->run(true);
    } catch (const std::exception& e) {
        std::cerr << "Exception in main: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "Unknown exception in main" << std::endl;
        return 1;
    }

    return 0;
}
