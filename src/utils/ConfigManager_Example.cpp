// File: src/utils/ConfigManager_Example.cpp
// 这是一个使用示例文件，展示如何使用ConfigManager

#include "ConfigManager.hpp"
#include <iostream>
#include <string>

using namespace mementodb::utils;

void ExampleBasicUsage() {
    std::cout << "=== ConfigManager 基本使用示例 ===\n\n";
    
    // 获取单例实例
    auto& config = ConfigManager::GetInstance();
    
    // 1. 从文件加载配置
    std::cout << "1. 从文件加载配置:\n";
    if (config.LoadFromFile("config.example.conf", "keyvalue")) {
        std::cout << "   ✓ 配置文件加载成功\n";
    } else {
        std::cout << "   ✗ 配置文件加载失败\n";
    }
    
    // 2. 从环境变量加载配置（前缀：MEMENTODB_）
    std::cout << "\n2. 从环境变量加载配置:\n";
    size_t env_count = config.LoadFromEnv("MEMENTODB_");
    std::cout << "   加载了 " << env_count << " 个环境变量配置\n";
    
    // 3. 读取配置值（类型安全）
    std::cout << "\n3. 读取配置值:\n";
    int port = config.Get<int>("server.port", 6379);
    std::string host = config.Get<std::string>("server.host", "localhost");
    bool use_direct_io = config.Get<bool>("performance.use_direct_io", false);
    
    std::cout << "   server.port = " << port << "\n";
    std::cout << "   server.host = " << host << "\n";
    std::cout << "   performance.use_direct_io = " << (use_direct_io ? "true" : "false") << "\n";
    
    // 4. 设置配置值
    std::cout << "\n4. 设置配置值:\n";
    config.Set("server.port", "6380");
    std::cout << "   设置 server.port = 6380\n";
    std::cout << "   读取 server.port = " << config.Get<int>("server.port") << "\n";
    
    // 5. 检查配置是否存在
    std::cout << "\n5. 检查配置是否存在:\n";
    if (config.Has("server.host")) {
        std::cout << "   ✓ server.host 存在\n";
    }
    if (!config.Has("nonexistent.key")) {
        std::cout << "   ✗ nonexistent.key 不存在\n";
    }
    
    // 6. 获取所有配置键
    std::cout << "\n6. 获取所有配置键:\n";
    auto keys = config.GetKeys();
    std::cout << "   共有 " << keys.size() << " 个配置项\n";
    std::cout << "   前5个配置键:\n";
    for (size_t i = 0; i < std::min(keys.size(), size_t(5)); ++i) {
        std::cout << "     - " << keys[i] << "\n";
    }
    
    // 7. 配置变更回调
    std::cout << "\n7. 配置变更回调:\n";
    config.RegisterCallback("server.port", [](const std::string& key, const std::string& value) {
        std::cout << "   配置变更: " << key << " = " << value << "\n";
    });
    config.Set("server.port", "6381");
    
    // 8. 配置验证
    std::cout << "\n8. 配置验证:\n";
    bool valid = config.Validate("server.port", [](const std::string& value) {
        int port = std::stoi(value);
        return port > 0 && port < 65536;
    });
    std::cout << "   server.port 验证结果: " << (valid ? "通过" : "失败") << "\n";
    
    // 9. 保存配置到文件
    std::cout << "\n9. 保存配置到文件:\n";
    if (config.SaveToFile("config.saved.conf", "keyvalue")) {
        std::cout << "   ✓ 配置已保存到 config.saved.conf\n";
    } else {
        std::cout << "   ✗ 配置保存失败\n";
    }
    
    // 10. 转储所有配置
    std::cout << "\n10. 转储所有配置:\n";
    std::cout << config.Dump() << "\n";
}

void ExampleIntegration() {
    std::cout << "\n=== ConfigManager 集成示例 ===\n\n";
    
    auto& config = ConfigManager::GetInstance();
    
    // 模拟从配置加载存储引擎配置
    struct EngineConfig {
        size_t page_size;
        size_t buffer_pool_size;
        bool use_direct_io;
        bool enable_wal;
    };
    
    EngineConfig engine_config;
    engine_config.page_size = config.Get<size_t>("storage.page_size", 4096);
    engine_config.buffer_pool_size = config.Get<size_t>("storage.buffer_pool_size", 1048576);
    engine_config.use_direct_io = config.Get<bool>("performance.use_direct_io", true);
    engine_config.enable_wal = config.Get<bool>("wal.enable", true);
    
    std::cout << "存储引擎配置:\n";
    std::cout << "  page_size = " << engine_config.page_size << "\n";
    std::cout << "  buffer_pool_size = " << engine_config.buffer_pool_size << "\n";
    std::cout << "  use_direct_io = " << (engine_config.use_direct_io ? "true" : "false") << "\n";
    std::cout << "  enable_wal = " << (engine_config.enable_wal ? "true" : "false") << "\n";
}

int main() {
    try {
        ExampleBasicUsage();
        ExampleIntegration();
    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}

