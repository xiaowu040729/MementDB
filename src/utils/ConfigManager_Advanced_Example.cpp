// File: src/utils/ConfigManager_Advanced_Example.cpp
// 高级ConfigManager使用示例

#include "ConfigManager.hpp"
#include <iostream>
#include <iomanip>

using namespace mementodb::utils;

void ExampleBuilderPattern() {
    std::cout << "=== Builder模式示例 ===\n\n";
    
    // 创建配置管理器
    auto config = ConfigManager::Builder()
        .add_file("config.json", ConfigFormat::JSON)
        .add_file("config.conf", ConfigFormat::KEY_VALUE)
        .add_env_vars("MEMENTODB_")
        .set_environment("production")
        .set_watch_enabled(true)
        .set_watch_interval(std::chrono::milliseconds(2000))
        .set_default("server.port", ConfigValue(static_cast<int64_t>(6379)))
        .set_default("server.host", ConfigValue(std::string("localhost")))
        .on_error([](const std::string& msg, const std::error_code& ec) {
            std::cerr << "配置错误: " << msg << " (" << ec.message() << ")\n";
        })
        .build();
    
    // 加载配置
    if (config->load()) {
        std::cout << "✓ 配置加载成功\n";
    } else {
        std::cout << "✗ 配置加载失败\n";
    }
    
    // 读取配置
    auto port = config->get<int64_t>("server.port", 6379);
    auto host = config->get<std::string>("server.host", "localhost");
    
    std::cout << "server.port = " << port << "\n";
    std::cout << "server.host = " << host << "\n";
    std::cout << "environment = " << config->get_environment() << "\n";
    std::cout << "is_production = " << (config->is_production() ? "true" : "false") << "\n";
}

void ExampleSchemaValidation() {
    std::cout << "\n=== Schema验证示例 ===\n\n";
    
    // 定义配置模式
    ConfigSchema schema;
    schema.name = "ServerConfig";
    
    // 添加字段
    schema.fields.push_back(ConfigSchema::Field::create<int64_t>(
        "server.port",
        6379,
        true, // 必需
        [](const ConfigValue& value) -> ValidationResult {
            if (auto p = std::get_if<int64_t>(&value)) {
                if (*p < 1 || *p > 65535) {
                    return ValidationResult{
                        false,
                        "端口必须在1-65535之间",
                        std::make_error_code(std::errc::invalid_argument)
                    };
                }
            }
            return ValidationResult{true, "", {}};
        },
        "服务器端口号"
    ));
    
    schema.fields.push_back(ConfigSchema::Field::create<std::string>(
        "server.host",
        std::string("localhost"),
        false,
        nullptr,
        "服务器主机地址"
    ));
    
    // 创建配置管理器并设置模式
    auto config = ConfigManager::Builder()
        .set_schema(schema)
        .build();
    
    // 设置配置
    config->set("server.port", static_cast<int64_t>(8080));
    config->set("server.host", std::string("0.0.0.0"));
    
    // 验证配置
    auto result = config->validate();
    if (result) {
        std::cout << "✓ 配置验证通过\n";
    } else {
        std::cout << "✗ 配置验证失败: " << result.message << "\n";
    }
    
    // 尝试设置无效值
    if (!config->set("server.port", static_cast<int64_t>(99999))) {
        std::cout << "✓ 成功阻止了无效端口设置\n";
    }
}

void ExampleWatchAndHotReload() {
    std::cout << "\n=== 配置监视和热重载示例 ===\n\n";
    
    auto config = ConfigManager::Builder()
        .add_file("config.conf", ConfigFormat::KEY_VALUE)
        .set_watch_enabled(true)
        .build();
    
    if (!config->load()) {
        std::cout << "✗ 配置加载失败\n";
        return;
    }
    
    // 注册配置变更回调
    auto handle = config->watch("server\\.port", 
        [](const std::string& key, const ConfigValue& old_val, const ConfigValue& new_val) {
            std::cout << "配置变更: " << key << "\n";
            std::cout << "  旧值: ";
            std::visit([](const auto& v) {
                std::cout << v;
            }, old_val);
            std::cout << "\n  新值: ";
            std::visit([](const auto& v) {
                std::cout << v;
            }, new_val);
            std::cout << "\n";
        });
    
    std::cout << "已注册监视器，handle = " << handle << "\n";
    std::cout << "启动配置监视...\n";
    
    config->start_watching();
    
    // 模拟配置变更
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    config->set("server.port", static_cast<int64_t>(8080));
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // 停止监视
    config->stop_watching();
    config->unwatch(handle);
    
    std::cout << "配置监视已停止\n";
}

void ExampleStatistics() {
    std::cout << "\n=== 统计信息示例 ===\n\n";
    
    auto config = ConfigManager::Builder()
        .add_file("config.conf", ConfigFormat::KEY_VALUE)
        .add_env_vars("MEMENTODB_")
        .build();
    
    config->load();
    
    auto stats = config->get_statistics();
    std::cout << "统计信息:\n";
    std::cout << "  总配置项数: " << stats.total_keys << "\n";
    std::cout << "  成功加载的源: " << stats.loaded_sources << "\n";
    std::cout << "  失败的源: " << stats.failed_sources << "\n";
    std::cout << "  重载次数: " << stats.reload_count << "\n";
    std::cout << "  监视回调数: " << stats.watch_callbacks << "\n";
    
    // 获取源信息
    auto source_info = config->get_source_info();
    std::cout << "\n配置源信息:\n";
    for (const auto& info : source_info) {
        std::cout << "  源: " << info.source.source << "\n";
        std::cout << "    格式: " << static_cast<int>(info.source.format) << "\n";
        std::cout << "    状态: " << (info.loaded ? "成功" : "失败") << "\n";
    }
}

void ExampleExportAndImport() {
    std::cout << "\n=== 导出和导入示例 ===\n\n";
    
    auto config = ConfigManager::Builder()
        .set_default("server.port", ConfigValue(static_cast<int64_t>(6379)))
        .set_default("server.host", ConfigValue(std::string("localhost")))
        .set_default("database.name", ConfigValue(std::string("mementodb")))
        .build();
    
    config->load();
    
    // 导出为JSON
    std::string json_str = config->export_to_string(ConfigFormat::JSON);
    std::cout << "JSON格式:\n" << json_str << "\n";
    
    // 导出为键值对
    std::string kv_str = config->export_to_string(ConfigFormat::KEY_VALUE);
    std::cout << "\n键值对格式:\n" << kv_str << "\n";
    
    // 导出到文件
    if (config->export_to_file("config.exported.json", ConfigFormat::JSON)) {
        std::cout << "\n✓ 配置已导出到 config.exported.json\n";
    }
}

void ExampleSubsetAndFind() {
    std::cout << "\n=== 子集和查找示例 ===\n\n";
    
    auto config = ConfigManager::Builder()
        .set_default("server.port", ConfigValue(static_cast<int64_t>(6379)))
        .set_default("server.host", ConfigValue(std::string("localhost")))
        .set_default("database.name", ConfigValue(std::string("mementodb")))
        .set_default("database.pool_size", ConfigValue(static_cast<int64_t>(10)))
        .build();
    
    config->load();
    
    // 获取server子集
    auto server_config = config->get_subset("server");
    std::cout << "server子集:\n";
    for (const auto& pair : server_config) {
        std::cout << "  " << pair.first << " = ";
        std::visit([](const auto& v) { std::cout << v; }, pair.second);
        std::cout << "\n";
    }
    
    // 查找匹配的键
    auto keys = config->find_keys("server\\..*");
    std::cout << "\n匹配 'server.*' 的键:\n";
    for (const auto& key : keys) {
        std::cout << "  " << key << "\n";
    }
}

int main() {
    try {
        ExampleBuilderPattern();
        ExampleSchemaValidation();
        ExampleWatchAndHotReload();
        ExampleStatistics();
        ExampleExportAndImport();
        ExampleSubsetAndFind();
    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}

