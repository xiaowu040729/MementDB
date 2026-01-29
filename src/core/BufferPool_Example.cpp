// File: src/core/BufferPool_Example.cpp
// BufferPool使用示例

#include "BufferPool.h"
#include "FileManager.hpp"
#include <iostream>
#include <thread>
#include <chrono>

using namespace mementodb::core;

void ExampleBasicUsage() {
    std::cout << "=== BufferPool 基本使用示例 ===\n\n";
    
    // 创建FileManager（简化示例）
    FileManagerConfig file_config;
    FileManager file_manager(file_config);
    
    // 创建BufferPool配置
    auto config = BufferPoolConfig::default_config();
    config.pool_size = 1024;
    config.replacer_type = BufferPoolConfig::ReplacerType::LRU;
    config.flush_strategy = BufferPoolConfig::FlushStrategy::WRITE_BACK;
    
    // 创建BufferPool
    auto buffer_pool = BufferPool::create(&file_manager, config);
    if (!buffer_pool) {
        std::cout << "✗ BufferPool创建失败\n";
        return;
    }
    
    std::cout << "✓ BufferPool创建成功\n";
    std::cout << "  替换策略: " << buffer_pool->get_replacer()->get_name() << "\n";
    std::cout << "  缓冲池大小: " << config.pool_size << " 页\n";
    
    // 获取页面
    uint64_t page_id = 1;
    auto frame = buffer_pool->fetch_page(page_id);
    if (frame) {
        std::cout << "✓ 成功获取页面 " << page_id << "\n";
        
        // 使用页面
        frame->page().GetData()[0] = 0x42;
        frame->mark_dirty();
        
        // 释放页面
        buffer_pool->unpin_page(page_id, true);
        std::cout << "✓ 页面已标记为脏页并释放\n";
    }
}

void ExampleReplacerComparison() {
    std::cout << "\n=== 替换策略对比示例 ===\n\n";
    
    FileManagerConfig file_config;
    FileManager file_manager(file_config);
    
    // 测试LRU策略
    {
        auto config = BufferPoolConfig::default_config();
        config.pool_size = 100;
        config.replacer_type = BufferPoolConfig::ReplacerType::LRU;
        
        auto pool = BufferPool::create(&file_manager, config);
        if (pool) {
            std::cout << "LRU替换策略:\n";
            std::cout << "  名称: " << pool->get_replacer()->get_name() << "\n";
            std::cout << "  可替换页面数: " << pool->get_replacer()->get_replacable_count() << "\n";
        }
    }
    
    // 测试Clock策略
    {
        auto config = BufferPoolConfig::default_config();
        config.pool_size = 100;
        config.replacer_type = BufferPoolConfig::ReplacerType::CLOCK;
        
        auto pool = BufferPool::create(&file_manager, config);
        if (pool) {
            std::cout << "\nClock替换策略:\n";
            std::cout << "  名称: " << pool->get_replacer()->get_name() << "\n";
            std::cout << "  可替换页面数: " << pool->get_replacer()->get_replacable_count() << "\n";
        }
    }
}

void ExampleStatistics() {
    std::cout << "\n=== 统计信息示例 ===\n\n";
    
    FileManagerConfig file_config;
    FileManager file_manager(file_config);
    
    auto config = BufferPoolConfig::default_config();
    config.pool_size = 512;
    config.enable_statistics = true;
    
    auto buffer_pool = BufferPool::create(&file_manager, config);
    if (!buffer_pool) {
        return;
    }
    
    // 模拟一些页面访问
    for (uint64_t i = 1; i <= 100; ++i) {
        auto frame = buffer_pool->fetch_page(i);
        if (frame) {
            frame->mark_accessed();
            buffer_pool->unpin_page(i);
        }
    }
    
    // 获取统计信息
    auto stats = buffer_pool->get_statistics();
    std::cout << "BufferPool统计信息:\n";
    std::cout << "  总帧数: " << stats.total_frames << "\n";
    std::cout << "  使用帧数: " << stats.used_frames << "\n";
    std::cout << "  空闲帧数: " << stats.free_frames << "\n";
    std::cout << "  脏页数: " << stats.dirty_frames << "\n";
    std::cout << "  固定页数: " << stats.pinned_frames << "\n";
    std::cout << "  命中次数: " << stats.hit_count << "\n";
    std::cout << "  未命中次数: " << stats.miss_count << "\n";
    std::cout << "  命中率: " << (stats.hit_ratio * 100) << "%\n";
    std::cout << "  驱逐次数: " << stats.evict_count << "\n";
    std::cout << "  刷盘次数: " << stats.flush_count << "\n";
    std::cout << "  预取次数: " << stats.prefetch_count << "\n";
}

void ExamplePrefetch() {
    std::cout << "\n=== 预取示例 ===\n\n";
    
    FileManagerConfig file_config;
    FileManager file_manager(file_config);
    
    auto config = BufferPoolConfig::default_config();
    config.enable_prefetch = true;
    config.prefetch_size = 4;
    
    auto buffer_pool = BufferPool::create(&file_manager, config);
    if (!buffer_pool) {
        return;
    }
    
    // 预取页面
    std::vector<uint64_t> page_ids = {10, 11, 12, 13, 14};
    buffer_pool->prefetch_pages(page_ids);
    
    std::cout << "✓ 已提交预取请求: " << page_ids.size() << " 个页面\n";
    
    // 等待预取完成
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // 获取统计信息
    auto stats = buffer_pool->get_statistics();
    std::cout << "预取次数: " << stats.prefetch_count << "\n";
}

void ExampleFlushStrategies() {
    std::cout << "\n=== 刷盘策略示例 ===\n\n";
    
    FileManagerConfig file_config;
    FileManager file_manager(file_config);
    
    // 测试不同的刷盘策略
    std::vector<BufferPoolConfig::FlushStrategy> strategies = {
        BufferPoolConfig::FlushStrategy::LAZY,
        BufferPoolConfig::FlushStrategy::PERIODIC,
        BufferPoolConfig::FlushStrategy::WRITE_THROUGH,
        BufferPoolConfig::FlushStrategy::WRITE_BACK
    };
    
    for (auto strategy : strategies) {
        auto config = BufferPoolConfig::default_config();
        config.flush_strategy = strategy;
        
        auto pool = BufferPool::create(&file_manager, config);
        if (pool) {
            std::cout << "刷盘策略: ";
            switch (strategy) {
                case BufferPoolConfig::FlushStrategy::LAZY:
                    std::cout << "LAZY (惰性刷盘)\n";
                    break;
                case BufferPoolConfig::FlushStrategy::PERIODIC:
                    std::cout << "PERIODIC (定期刷盘)\n";
                    break;
                case BufferPoolConfig::FlushStrategy::WRITE_THROUGH:
                    std::cout << "WRITE_THROUGH (直写)\n";
                    break;
                case BufferPoolConfig::FlushStrategy::WRITE_BACK:
                    std::cout << "WRITE_BACK (回写)\n";
                    break;
            }
        }
    }
}

int main() {
    try {
        ExampleBasicUsage();
        ExampleReplacerComparison();
        ExampleStatistics();
        ExamplePrefetch();
        ExampleFlushStrategies();
    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}

