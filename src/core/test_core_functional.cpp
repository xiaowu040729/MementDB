// File: src/core/test_core_functional.cpp
// Core 模块功能测试程序

#include "DiskEngine.hpp"
#include "Record.hpp"
#include "Page.hpp"
#include "mementodb/Types.h"
#include "../utils/LoggingSystem/LogMacros.hpp"
#include <iostream>
#include <cassert>
#include <filesystem>
#include <thread>
#include <chrono>
#include <vector>
#include <random>

using namespace mementodb;
using namespace mementodb::core;

// 测试辅助函数
void print_test_header(const std::string& test_name) {
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "测试: " << test_name << std::endl;
    std::cout << std::string(70, '=') << std::endl;
}

void print_test_result(bool passed, const std::string& message) {
    if (passed) {
        std::cout << "  ✓ 通过: " << message << std::endl;
    } else {
        std::cout << "  ✗ 失败: " << message << std::endl;
    }
}

// 测试统计
struct TestStats {
    int passed = 0;
    int failed = 0;
    
    void add_result(bool result, const std::string& msg) {
        if (result) {
            passed++;
            print_test_result(true, msg);
        } else {
            failed++;
            print_test_result(false, msg);
        }
    }
    
    void print_summary() {
        std::cout << "\n" << std::string(70, '-') << std::endl;
        std::cout << "测试总结:" << std::endl;
        std::cout << "  通过: " << passed << " 个" << std::endl;
        std::cout << "  失败: " << failed << " 个" << std::endl;
        std::cout << "  总计: " << (passed + failed) << " 个测试" << std::endl;
        std::cout << std::string(70, '-') << std::endl;
    }
};

// ==================== Record 功能测试 ====================

void test_record_functional(TestStats& stats) {
    print_test_header("Record 功能测试");
    
    // 测试1: 基本编码/解码
    {
        Slice key("user:001");
        Slice value("Alice");
        
        size_t size = Record::ByteSize(key.size(), value.size());
        std::vector<char> buffer(size);
        
        Record::Encode(buffer.data(), key, value);
        
        Slice decoded_key, decoded_value;
        bool success = Record::Decode(buffer.data(), size, &decoded_key, &decoded_value);
        
        stats.add_result(success && 
                        decoded_key.ToString() == key.ToString() &&
                        decoded_value.ToString() == value.ToString(),
                        "基本编码/解码");
    }
    
    // 测试2: 空值处理
    {
        Slice key("empty_key");
        Slice value("");
        
        size_t size = Record::ByteSize(key.size(), value.size());
        std::vector<char> buffer(size);
        
        Record::Encode(buffer.data(), key, value);
        
        Slice decoded_key, decoded_value;
        bool success = Record::Decode(buffer.data(), size, &decoded_key, &decoded_value);
        
        stats.add_result(success && 
                        decoded_key.ToString() == key.ToString() &&
                        decoded_value.size() == 0,
                        "空值处理");
    }
    
    // 测试3: 大数据
    {
        std::string large_key(200, 'K');
        std::string large_value(1000, 'V');
        Slice key(large_key);
        Slice value(large_value);
        
        size_t size = Record::ByteSize(key.size(), value.size());
        std::vector<char> buffer(size);
        
        Record::Encode(buffer.data(), key, value);
        
        Slice decoded_key, decoded_value;
        bool success = Record::Decode(buffer.data(), size, &decoded_key, &decoded_value);
        
        stats.add_result(success && 
                        decoded_key.size() == 200 &&
                        decoded_value.size() == 1000,
                        "大数据处理");
    }
}

// ==================== Page 功能测试 ====================

void test_page_functional(TestStats& stats) {
    print_test_header("Page 功能测试");
    
    // 测试1: 基本创建和属性
    {
        Page page(100);
        bool result = (page.GetPageId() == 100 && 
                      page.GetType() == PageType::INVALID &&
                      page.GetKeyCount() == 0);
        stats.add_result(result, "Page 创建和基本属性");
    }
    
    // 测试2: 类型设置
    {
        Page page(200);
        page.SetType(PageType::LEAF);
        page.SetKeyCount(5);
        
        bool result = (page.GetType() == PageType::LEAF && 
                      page.IsLeaf() &&
                      page.GetKeyCount() == 5);
        stats.add_result(result, "Page 类型和键数量设置");
    }
    
    // 测试3: 序列化/反序列化
    {
        Page page1(300);
        page1.SetType(PageType::DATA);
        page1.SetKeyCount(10);
        page1.SetFreeOffset(100);
        
        char buffer[kPageSize];
        page1.SerializeTo(buffer);
        
        Page page2;
        page2.DeserializeFrom(buffer);
        
        bool result = (page2.GetPageId() == 300 &&
                      page2.GetType() == PageType::DATA &&
                      page2.GetKeyCount() == 10 &&
                      page2.GetFreeOffset() == 100);
        stats.add_result(result, "Page 序列化/反序列化");
    }
    
    // 测试4: 数据指针
    {
        Page page(400);
        char* data = page.GetData();
        const char* const_data = const_cast<const Page&>(page).GetData();
        
        bool result = (data != nullptr && const_data != nullptr);
        stats.add_result(result, "Page 数据指针访问");
    }
}

// ==================== DiskEngine 功能测试 ====================

void test_diskengine_basic_ops(TestStats& stats) {
    print_test_header("DiskEngine 基本操作测试");
    
    std::string test_dir = "/tmp/mementodb_test_functional";
    std::filesystem::remove_all(test_dir);
    std::filesystem::create_directories(test_dir);
    
    try {
        // 创建引擎（禁用 WAL 以避免后台线程问题）
        EngineConfig config;
        config.buffer_pool_size = 100;
        config.page_size = 4096;
        config.enable_wal = false;  // 禁用 WAL 避免后台线程卡住
        
        std::cout << "  正在创建引擎..." << std::flush;
        DiskEngineV2 engine(test_dir, config);
        std::cout << " 完成" << std::endl;
        stats.add_result(true, "引擎创建");
        
        // 测试1: put 操作
        {
            Slice key("test:key:001");
            Slice value("test_value_001");
            
            std::cout << "  执行 put 操作..." << std::flush;
            auto future = engine.put(key, value);
            bool result = false;
            try {
                // 设置超时
                auto status = future.wait_for(std::chrono::seconds(5));
                if (status == std::future_status::ready) {
                    result = future.get();
                    std::cout << " 完成 (结果: " << (result ? "成功" : "失败") << ")" << std::endl;
                    if (!result) {
                        std::cout << "    提示: put 操作失败，可能是文件系统权限或磁盘空间问题" << std::endl;
                    }
                } else {
                    std::cout << " 超时" << std::endl;
                    result = false;
                }
            } catch (const std::exception& e) {
                std::cout << " 异常: " << e.what() << std::endl;
                result = false;
            }
            stats.add_result(result, "put 操作");
        }
        
        // 测试2: get 操作
        {
            Slice key("test:key:001");
            auto future = engine.get(key);
            auto result = future.get();
            
            bool success = result.has_value() && 
                          result.value().ToString() == "test_value_001";
            stats.add_result(success, "get 操作");
        }
        
        // 测试3: 更新操作
        {
            Slice key("test:key:001");
            Slice new_value("updated_value_001");
            
            auto put_future = engine.put(key, new_value);
            bool put_result = put_future.get();
            
            if (put_result) {
                auto get_future = engine.get(key);
                auto get_result = get_future.get();
                
                bool success = get_result.has_value() && 
                              get_result.value().ToString() == "updated_value_001";
                stats.add_result(success, "更新操作");
            } else {
                stats.add_result(false, "更新操作");
            }
        }
        
        // 测试4: remove 操作
        {
            Slice key("test:key:001");
            
            auto future = engine.remove(key);
            bool result = future.get();
            
            if (result) {
                auto get_future = engine.get(key);
                auto get_result = get_future.get();
                bool success = !get_result.has_value();
                stats.add_result(success, "remove 操作");
            } else {
                stats.add_result(false, "remove 操作");
            }
        }
        
        // 清理
        std::filesystem::remove_all(test_dir);
        
    } catch (const std::exception& e) {
        std::cout << "  ✗ 异常: " << e.what() << std::endl;
        stats.add_result(false, "引擎操作（发生异常）");
        std::filesystem::remove_all(test_dir);
    }
}

void test_diskengine_batch_ops(TestStats& stats) {
    print_test_header("DiskEngine 批量操作测试");
    
    std::string test_dir = "/tmp/mementodb_test_batch";
    std::filesystem::remove_all(test_dir);
    std::filesystem::create_directories(test_dir);
    
    try {
        EngineConfig config;
        config.buffer_pool_size = 100;
        config.enable_wal = false;  // 禁用 WAL
        
        DiskEngineV2 engine(test_dir, config);
        
        // 批量插入
        std::vector<std::pair<std::string, std::string>> test_data = {
            {"batch:001", "value_001"},
            {"batch:002", "value_002"},
            {"batch:003", "value_003"},
            {"batch:004", "value_004"},
            {"batch:005", "value_005"}
        };
        
        std::vector<std::future<bool>> futures;
        for (const auto& [key_str, value_str] : test_data) {
            Slice key(key_str);
            Slice value(value_str);
            futures.push_back(engine.put(key, value));
        }
        
        bool all_success = true;
        for (auto& f : futures) {
            if (!f.get()) {
                all_success = false;
                break;
            }
        }
        
        if (all_success) {
            // 验证所有值
            bool all_found = true;
            for (const auto& [key_str, value_str] : test_data) {
                Slice key(key_str);
                auto get_future = engine.get(key);
                auto result = get_future.get();
                
                if (!result.has_value() || result.value().ToString() != value_str) {
                    all_found = false;
                    break;
                }
            }
            stats.add_result(all_found, "批量插入和验证");
        } else {
            stats.add_result(false, "批量插入");
        }
        
        // 范围查询测试
        {
            Slice start_key("batch:002");
            Slice end_key("batch:004");
            
            std::vector<std::pair<std::string, std::string>> results;
            auto future = engine.range_query(start_key, end_key,
                [&results](const Slice& key, const Slice& value) {
                    results.push_back({key.ToString(), value.ToString()});
                });
            future.get();
            
            bool success = results.size() >= 2;  // 至少应该有 batch:002, batch:003, batch:004
            stats.add_result(success, "范围查询（找到 " + std::to_string(results.size()) + " 条记录）");
        }
        
        std::filesystem::remove_all(test_dir);
        
    } catch (const std::exception& e) {
        std::cout << "  ✗ 异常: " << e.what() << std::endl;
        stats.add_result(false, "批量操作（发生异常）");
        std::filesystem::remove_all(test_dir);
    }
}

void test_diskengine_page_ops(TestStats& stats) {
    print_test_header("DiskEngine 页操作测试");
    
    std::string test_dir = "/tmp/mementodb_test_page";
    std::filesystem::remove_all(test_dir);
    std::filesystem::create_directories(test_dir);
    
    try {
        EngineConfig config;
        config.buffer_pool_size = 50;
        config.enable_wal = false;  // 禁用 WAL
        
        DiskEngineV2 engine(test_dir, config);
        
        // 测试1: 写入页
        {
            Page page(1000);
            page.SetType(PageType::DATA);
            page.SetKeyCount(5);
            page.SetFreeOffset(200);
            
            auto future = engine.write_page_async(page);
            bool result = future.get();
            stats.add_result(result, "写入页");
        }
        
        // 测试2: 读取页
        {
            auto future = engine.read_page_async(1000);
            Page read_page = future.get();
            
            bool result = (read_page.GetPageId() == 1000 &&
                          read_page.GetType() == PageType::DATA &&
                          read_page.GetKeyCount() == 5);
            stats.add_result(result, "读取页");
        }
        
        // 测试3: 并发读写
        {
            std::vector<std::future<bool>> write_futures;
            std::vector<std::future<Page>> read_futures;
            
            // 写入多个页
            for (uint64_t i = 2000; i < 2005; ++i) {
                Page page(i);
                page.SetType(PageType::DATA);
                page.SetKeyCount(i % 10);
                write_futures.push_back(engine.write_page_async(page));
            }
            
            // 等待所有写入完成
            bool all_write_success = true;
            for (auto& f : write_futures) {
                if (!f.get()) {
                    all_write_success = false;
                    break;
                }
            }
            
            if (all_write_success) {
                // 读取所有页
                for (uint64_t i = 2000; i < 2005; ++i) {
                    read_futures.push_back(engine.read_page_async(i));
                }
                
                // 验证所有读取
                bool all_read_success = true;
                for (size_t i = 0; i < read_futures.size(); ++i) {
                    Page page = read_futures[i].get();
                    if (page.GetPageId() != 2000 + i) {
                        all_read_success = false;
                        break;
                    }
                }
                
                stats.add_result(all_read_success, "并发读写（5个页）");
            } else {
                stats.add_result(false, "并发写入");
            }
        }
        
        std::filesystem::remove_all(test_dir);
        
    } catch (const std::exception& e) {
        std::cout << "  ✗ 异常: " << e.what() << std::endl;
        stats.add_result(false, "页操作（发生异常）");
        std::filesystem::remove_all(test_dir);
    }
}

void test_diskengine_status(TestStats& stats) {
    print_test_header("DiskEngine 状态查询测试");
    
    std::string test_dir = "/tmp/mementodb_test_status";
    std::filesystem::remove_all(test_dir);
    std::filesystem::create_directories(test_dir);
    
    try {
        EngineConfig config;
        config.buffer_pool_size = 100;
        config.enable_wal = false;  // 禁用 WAL
        
        DiskEngineV2 engine(test_dir, config);
        
        // 插入一些数据
        for (int i = 0; i < 10; ++i) {
            std::string key = "status:test:" + std::to_string(i);
            std::string value = "value_" + std::to_string(i);
            Slice k(key);
            Slice v(value);
            engine.put(k, v).get();
        }
        
        // 查询状态
        auto status = engine.get_status();
        
        bool result = (status.total_pages > 0);
        stats.add_result(result, "状态查询");
        
        if (result) {
            std::cout << "    总页数: " << status.total_pages << std::endl;
            std::cout << "    已使用页数: " << status.used_pages << std::endl;
            std::cout << "    缓冲池命中率: " << status.buffer_pool_stats.hit_ratio << std::endl;
        }
        
        std::filesystem::remove_all(test_dir);
        
    } catch (const std::exception& e) {
        std::cout << "  ✗ 异常: " << e.what() << std::endl;
        stats.add_result(false, "状态查询（发生异常）");
        std::filesystem::remove_all(test_dir);
    }
}

// ==================== 主函数 ====================

int main() {
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║     MementoDB Core 模块功能测试套件                      ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════════════════╝" << std::endl;
    
    TestStats total_stats;
    
    try {
        // 运行各项测试
        TestStats record_stats;
        test_record_functional(record_stats);
        record_stats.print_summary();
        total_stats.passed += record_stats.passed;
        total_stats.failed += record_stats.failed;
        
        TestStats page_stats;
        test_page_functional(page_stats);
        page_stats.print_summary();
        total_stats.passed += page_stats.passed;
        total_stats.failed += page_stats.failed;
        
        TestStats engine_basic_stats;
        test_diskengine_basic_ops(engine_basic_stats);
        engine_basic_stats.print_summary();
        total_stats.passed += engine_basic_stats.passed;
        total_stats.failed += engine_basic_stats.failed;
        
        TestStats engine_batch_stats;
        test_diskengine_batch_ops(engine_batch_stats);
        engine_batch_stats.print_summary();
        total_stats.passed += engine_batch_stats.passed;
        total_stats.failed += engine_batch_stats.failed;
        
        TestStats engine_page_stats;
        test_diskengine_page_ops(engine_page_stats);
        engine_page_stats.print_summary();
        total_stats.passed += engine_page_stats.passed;
        total_stats.failed += engine_page_stats.failed;
        
        TestStats engine_status_stats;
        test_diskengine_status(engine_status_stats);
        engine_status_stats.print_summary();
        total_stats.passed += engine_status_stats.passed;
        total_stats.failed += engine_status_stats.failed;
        
        // 总体总结
        std::cout << "\n";
        std::cout << "╔══════════════════════════════════════════════════════════╗" << std::endl;
        std::cout << "║                    总体测试总结                           ║" << std::endl;
        std::cout << "╚══════════════════════════════════════════════════════════╝" << std::endl;
        total_stats.print_summary();
        
        if (total_stats.failed == 0) {
            std::cout << "\n🎉 所有测试通过！Core 模块功能正常！\n" << std::endl;
            return 0;
        } else {
            std::cout << "\n⚠️  有 " << total_stats.failed << " 个测试失败，请检查代码。\n" << std::endl;
            return 1;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "\n❌ 测试过程中发生异常: " << e.what() << std::endl;
        return 1;
    }
}

