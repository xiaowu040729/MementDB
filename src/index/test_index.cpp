#include "HashIndex.hpp"
#include "SkipListIndex.hpp"
#include "BloomFilter.hpp"
#include "IndexManager.hpp"
#include "mementodb/Types.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <chrono>

using namespace mementodb;
using namespace mementodb::index;

// 测试辅助函数
void print_test_header(const std::string& test_name) {
    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << "测试: " << test_name << "\n";
    std::cout << std::string(60, '=') << "\n";
}

void print_success(const std::string& msg) {
    std::cout << "✓ " << msg << "\n";
}

void print_error(const std::string& msg) {
    std::cout << "✗ " << msg << "\n";
}

// 测试 HashIndex
void test_hash_index() {
    print_test_header("HashIndex 测试");
    
    try {
        HashIndexConfig config;
        config.name = "test_hash";
        config.initial_bucket_count = 16;
        config.max_load_factor = 0.75;
        config.enable_rehashing = true;
        
        HashIndex index(config);
        
        // 测试插入
        assert(index.insert(Slice("key1"), 100));
        assert(index.insert(Slice("key2"), 200));
        assert(index.insert(Slice("key3"), 300));
        print_success("插入操作");
        
        // 测试查找
        auto val1 = index.get(Slice("key1"));
        assert(val1.has_value() && val1.value() == 100);
        auto val2 = index.get(Slice("key2"));
        assert(val2.has_value() && val2.value() == 200);
        print_success("查找操作");
        
        // 测试 contains
        assert(index.contains(Slice("key1")));
        assert(!index.contains(Slice("key999")));
        print_success("contains 操作");
        
        // 测试更新
        assert(index.insert(Slice("key1"), 150));
        auto val1_updated = index.get(Slice("key1"));
        assert(val1_updated.has_value() && val1_updated.value() == 150);
        print_success("更新操作");
        
        // 测试删除
        assert(index.remove(Slice("key2")));
        assert(!index.contains(Slice("key2")));
        print_success("删除操作");
        
        // 测试大小
        assert(index.size() == 2);
        assert(!index.empty());
        print_success("size 和 empty 操作");
        
        // 测试批量操作
        std::vector<std::pair<Slice, uint64_t>> batch_data = {
            {Slice("batch1"), 1000},
            {Slice("batch2"), 2000},
            {Slice("batch3"), 3000}
        };
        auto [success_count, failed] = index.batch_insert(batch_data);
        assert(success_count == 3);
        assert(failed.empty());
        print_success("批量插入");
        
        // 测试 insert_or_update
        auto old_val = index.insert_or_update(Slice("key1"), 999);
        assert(old_val.has_value());
        auto new_val = index.get(Slice("key1"));
        assert(new_val.has_value() && new_val.value() == 999);
        print_success("insert_or_update 操作");
        
        // 测试统计信息
        auto stats = index.get_statistics();
        assert(stats.total_keys > 0);
        print_success("统计信息获取");
        
        // 测试负载因子
        double load = index.load_factor();
        std::cout << "  负载因子: " << load << "\n";
        print_success("负载因子计算");
        
        // 测试清空
        index.clear();
        assert(index.empty());
        assert(index.size() == 0);
        print_success("清空操作");
        
        std::cout << "\n所有 HashIndex 测试通过！\n";
    } catch (const std::exception& e) {
        print_error(std::string("HashIndex 测试失败: ") + e.what());
        throw;
    }
}

// 测试 SkipListIndex
void test_skip_list_index() {
    print_test_header("SkipListIndex 测试");
    
    try {
        SkipListIndexConfig config;
        config.name = "test_skiplist";
        config.max_level = 8;
        config.probability = 0.5;
        config.thread_safe = true;
        
        SkipListIndex index(config);
        
        // 测试插入
        assert(index.insert(Slice("apple"), 1));
        assert(index.insert(Slice("banana"), 2));
        assert(index.insert(Slice("cherry"), 3));
        assert(index.insert(Slice("date"), 4));
        assert(index.insert(Slice("elderberry"), 5));
        print_success("插入操作");
        
        // 测试查找
        auto val1 = index.get(Slice("banana"));
        assert(val1.has_value() && val1.value() == 2);
        print_success("查找操作");
        
        // 测试范围查询
        auto range_result = index.range_query(Slice("banana"), Slice("date"));
        assert(!range_result.empty());
        assert(range_result.size() >= 2);
        std::cout << "  范围查询结果数量: " << range_result.size() << "\n";
        print_success("范围查询");
        
        // 测试前缀查询
        auto prefix_result = index.prefix_query(Slice("c"));
        assert(!prefix_result.empty());
        std::cout << "  前缀查询结果数量: " << prefix_result.size() << "\n";
        print_success("前缀查询");
        
        // 测试 min/max
        auto min_pair = index.min();
        assert(min_pair.has_value());
        std::cout << "  最小键: " << min_pair->first.ToString() << "\n";
        
        auto max_pair = index.max();
        assert(max_pair.has_value());
        std::cout << "  最大键: " << max_pair->first.ToString() << "\n";
        print_success("min/max 操作");
        
        // 测试迭代器
        auto it = index.begin();
        size_t count = 0;
        while (it.valid()) {
            count++;
            it.next();
        }
        assert(count == index.size());
        print_success("迭代器操作");
        
        // 测试批量操作
        std::vector<std::pair<Slice, uint64_t>> batch_data = {
            {Slice("fruit1"), 10},
            {Slice("fruit2"), 20},
            {Slice("fruit3"), 30}
        };
        auto [success_count, failed] = index.batch_insert(batch_data);
        assert(success_count == 3);
        print_success("批量插入");
        
        // 测试内存使用
        size_t mem_usage = index.memory_usage();
        std::cout << "  内存使用: " << mem_usage << " 字节\n";
        print_success("内存使用统计");
        
        // 测试统计信息
        auto stats = index.get_statistics();
        assert(stats.total_keys > 0);
        print_success("统计信息获取");
        
        std::cout << "\n所有 SkipListIndex 测试通过！\n";
    } catch (const std::exception& e) {
        print_error(std::string("SkipListIndex 测试失败: ") + e.what());
        throw;
    }
}

// 测试 BloomFilter
void test_bloom_filter() {
    print_test_header("BloomFilter 测试");
    
    try {
        BloomFilterConfig config;
        config.name = "test_bloom";
        config.expected_element_count = 1000;
        config.false_positive_rate = 0.01;
        config.counting_bloom = false;
        config.enable_fast_hash = true;
        config.calculate_optimal_parameters();
        
        BloomFilter filter(config);
        std::cout << "  bit_array_size=" << filter.get_bit_array_size()
                  << ", num_hash=" << filter.get_num_hash_functions() << "\n";
        
        // 测试插入
        assert(filter.insert(Slice("item1"), 1));
        assert(filter.insert(Slice("item2"), 2));
        assert(filter.insert(Slice("item3"), 3));
        print_success("插入操作");
        
        // 测试存在性检查
        assert(filter.contains(Slice("item1")));
        assert(filter.contains(Slice("item2")));
        assert(!filter.contains(Slice("nonexistent")));
        print_success("存在性检查");
        
        // 测试批量插入
        std::vector<std::pair<Slice, uint64_t>> batch_data;
        for (int i = 0; i < 100; ++i) {
            batch_data.push_back({Slice("batch_" + std::to_string(i)), i});
        }
        auto batch_result = filter.batch_insert(batch_data);
        size_t batch_count = batch_result.first;
        assert(batch_count == 100);
        print_success("批量插入");
        
        // 测试假阳性率
        double false_positive = filter.get_actual_false_positive_rate();
        std::cout << "  实际假阳性率: " << (false_positive * 100) << "%\n";
        print_success("假阳性率计算");
        
        // 测试位使用率
        double bit_usage = filter.get_bit_usage_ratio();
        std::cout << "  位使用率: " << (bit_usage * 100) << "%\n";
        print_success("位使用率计算");
        
        // 测试估计元素数量
        size_t estimated = filter.get_estimated_element_count();
        std::cout << "  估计元素数量: " << estimated << "\n";
        print_success("估计元素数量");
        
        // 测试统计信息
        auto stats = filter.get_statistics();
        assert(stats.total_keys > 0);
        print_success("统计信息获取");
        
        // 测试清空
        filter.clear();
        assert(filter.empty());
        print_success("清空操作");
        
        std::cout << "\n所有 BloomFilter 测试通过！\n";
    } catch (const std::exception& e) {
        print_error(std::string("BloomFilter 测试失败: ") + e.what());
        throw;
    }
}

// 测试计数布隆过滤器
void test_counting_bloom_filter() {
    print_test_header("Counting BloomFilter 测试");
    
    try {
        BloomFilterConfig config;
        config.name = "test_counting_bloom";
        config.expected_element_count = 100;
        config.false_positive_rate = 0.01;
        config.counting_bloom = true;
        config.counting_bits = 4;
        config.calculate_optimal_parameters();
        
        BloomFilter filter(config);
        
        // 测试插入
        assert(filter.insert(Slice("item1"), 1));
        assert(filter.contains(Slice("item1")));
        print_success("插入操作");
        
        // 测试删除（计数布隆过滤器支持）
        assert(filter.remove(Slice("item1")));
        assert(!filter.contains(Slice("item1")));
        print_success("删除操作（计数布隆过滤器）");
        
        // 测试不支持删除的提示
        assert(filter.supports_deletion());
        print_success("删除支持检查");
        
        std::cout << "\n所有 Counting BloomFilter 测试通过！\n";
    } catch (const std::exception& e) {
        print_error(std::string("Counting BloomFilter 测试失败: ") + e.what());
        throw;
    }
}

// 测试 IndexManager
void test_index_manager() {
    print_test_header("IndexManager 测试");
    
    try {
        IndexManagerConfig mgr_config;
        mgr_config.storage_path = "./test_indexes";
        mgr_config.enable_cache = true;
        mgr_config.cache_size = 10;
        
        IndexManager manager(mgr_config);
        
        // 测试创建哈希索引
        HashIndexConfig hash_config;
        hash_config.name = "manager_hash";
        hash_config.initial_bucket_count = 32;
        
        auto hash_index = manager.create_index<HashIndexConfig>("hash1", hash_config);
        assert(hash_index != nullptr);
        assert(hash_index->get_name() == "hash1");
        print_success("创建哈希索引");
        
        // 测试创建跳表索引
        SkipListIndexConfig skip_config;
        skip_config.name = "manager_skiplist";
        skip_config.max_level = 8;
        
        auto skip_index = manager.create_index<SkipListIndexConfig>("skiplist1", skip_config);
        assert(skip_index != nullptr);
        print_success("创建跳表索引");
        
        // 测试创建布隆过滤器
        BloomFilterConfig bloom_config;
        bloom_config.name = "manager_bloom";
        bloom_config.expected_element_count = 1000;
        bloom_config.calculate_optimal_parameters();
        
        auto bloom_index = manager.create_index<BloomFilterConfig>("bloom1", bloom_config);
        assert(bloom_index != nullptr);
        print_success("创建布隆过滤器");
        
        // 测试获取索引
        auto retrieved = manager.get_index("hash1");
        assert(retrieved != nullptr);
        assert(retrieved->get_name() == "hash1");
        print_success("获取索引");
        
        // 测试检查索引是否存在
        assert(manager.has_index("hash1"));
        assert(!manager.has_index("nonexistent"));
        print_success("检查索引存在性");
        
        // 测试列出索引
        auto index_list = manager.list_indexes();
        assert(index_list.size() >= 3);
        std::cout << "  索引列表大小: " << index_list.size() << "\n";
        print_success("列出索引");
        
        // 测试获取所有统计信息
        auto all_stats = manager.get_all_statistics();
        assert(all_stats.size() >= 3);
        print_success("获取所有统计信息");
        
        // 测试获取指标
        auto metrics = manager.get_metrics();
        assert(metrics.total_indexes >= 3);
        std::cout << "  总索引数: " << metrics.total_indexes << "\n";
        print_success("获取管理器指标");
        
        // 测试删除索引
        assert(manager.remove_index("hash1", false));
        assert(!manager.has_index("hash1"));
        print_success("删除索引");
        
        std::cout << "\n所有 IndexManager 测试通过！\n";
    } catch (const std::exception& e) {
        print_error(std::string("IndexManager 测试失败: ") + e.what());
        throw;
    }
}

// 性能测试
void test_performance() {
    print_test_header("性能测试");
    
    try {
        // HashIndex 性能测试
        {
            HashIndexConfig config;
            config.name = "perf_hash";
            config.initial_bucket_count = 1024;
            HashIndex index(config);
            
            const int test_size = 10000;
            auto start = std::chrono::high_resolution_clock::now();
            
            for (int i = 0; i < test_size; ++i) {
                std::string key = "key_" + std::to_string(i);
                index.insert(Slice(key), i);
            }
            
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
            
            std::cout << "  HashIndex 插入 " << test_size << " 个元素: " 
                      << duration.count() << " 微秒\n";
            std::cout << "  平均每次插入: " << (duration.count() / test_size) << " 微秒\n";
        }
        
        // SkipListIndex 性能测试
        {
            SkipListIndexConfig config;
            config.name = "perf_skiplist";
            config.max_level = 16;
            SkipListIndex index(config);
            
            const int test_size = 10000;
            auto start = std::chrono::high_resolution_clock::now();
            
            for (int i = 0; i < test_size; ++i) {
                std::string key = "key_" + std::to_string(i);
                index.insert(Slice(key), i);
            }
            
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
            
            std::cout << "  SkipListIndex 插入 " << test_size << " 个元素: " 
                      << duration.count() << " 微秒\n";
            std::cout << "  平均每次插入: " << (duration.count() / test_size) << " 微秒\n";
        }
        
        // BloomFilter 性能测试
        {
            BloomFilterConfig config;
            config.name = "perf_bloom";
            config.expected_element_count = 10000;
            config.calculate_optimal_parameters();
            BloomFilter filter(config);
            
            const int test_size = 10000;
            auto start = std::chrono::high_resolution_clock::now();
            
            for (int i = 0; i < test_size; ++i) {
                std::string key = "key_" + std::to_string(i);
                filter.insert(Slice(key), i);
            }
            
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
            
            std::cout << "  BloomFilter 插入 " << test_size << " 个元素: " 
                      << duration.count() << " 微秒\n";
            std::cout << "  平均每次插入: " << (duration.count() / test_size) << " 微秒\n";
        }
        
        print_success("性能测试完成");
    } catch (const std::exception& e) {
        print_error(std::string("性能测试失败: ") + e.what());
        throw;
    }
}

int main() {
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║         MementoDB Index 模块测试程序                      ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n";
    
    try {
        // 运行所有测试
        test_hash_index();
        test_skip_list_index();
        test_bloom_filter();
        test_counting_bloom_filter();
        test_index_manager();
        test_performance();
        
        std::cout << "\n";
        std::cout << "╔══════════════════════════════════════════════════════════╗\n";
        std::cout << "║           所有测试通过！✓                                ║\n";
        std::cout << "╚══════════════════════════════════════════════════════════╝\n";
        std::cout << "\n";
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n测试失败: " << e.what() << "\n";
        return 1;
    }
}

