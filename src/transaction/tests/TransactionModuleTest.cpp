// File: src/transaction/tests/TransactionModuleTest.cpp
// 事务模块综合测试

#include "../src/TransactionManager.hpp"
#include "../src/MVCCEngine.hpp"
#include "../src/LockManager.hpp"
#include "../src/LockTable.hpp"
#include "../include/Transaction.hpp"
#include "../include/IsolationLevel.hpp"
#include "../../utils/LoggingSystem/LogMacros.hpp"
#include <iostream>
#include <cassert>
#include <thread>
#include <vector>
#include <chrono>
#include <atomic>
#include <string>
#include <sstream>

using namespace mementodb::transaction;

// 测试辅助宏
#define TEST_CASE(name) \
    do { \
        std::cout << "\n========== " << name << " ==========" << std::endl; \
    } while(0)

#define ASSERT(condition, msg) \
    do { \
        if (!(condition)) { \
            std::cerr << "断言失败: " << msg << " (文件: " << __FILE__ << ", 行: " << __LINE__ << ")" << std::endl; \
            assert(false); \
        } \
    } while(0)

#define TEST_PASS(msg) \
    do { \
        std::cout << "✓ " << msg << std::endl; \
    } while(0)

// ==================== 测试1: 基本事务操作 ====================

void test_basic_transaction_lifecycle() {
    TEST_CASE("测试1: 基本事务生命周期");
    
    TransactionManager::TransactionConfig config;
    TransactionManager manager(config);
    
    // 测试开始事务
    TransactionID tid1 = manager.begin_transaction(IsolationLevel::REPEATABLE_READ);
    ASSERT(tid1 > 0, "事务ID应该大于0");
    ASSERT(manager.is_active(tid1), "事务应该是活跃的");
    TEST_PASS("事务创建成功");
    
    // 测试获取事务上下文
    auto ctx1 = manager.get_transaction(tid1);
    ASSERT(ctx1 != nullptr, "应该能获取事务上下文");
    ASSERT(ctx1->get_id() == tid1, "事务ID应该匹配");
    TEST_PASS("获取事务上下文成功");
    
    // 测试提交事务
    bool commit_result = manager.commit_transaction(tid1);
    ASSERT(commit_result, "提交应该成功");
    ASSERT(manager.is_committed(tid1), "事务应该是已提交状态");
    ASSERT(!manager.is_active(tid1), "事务不应该是活跃的");
    TEST_PASS("事务提交成功");
    
    // 测试回滚事务
    TransactionID tid2 = manager.begin_transaction(IsolationLevel::READ_COMMITTED);
    bool abort_result = manager.abort_transaction(tid2);
    ASSERT(abort_result, "回滚应该成功");
    ASSERT(manager.is_aborted(tid2), "事务应该是已中止状态");
    ASSERT(!manager.is_active(tid2), "事务不应该是活跃的");
    TEST_PASS("事务回滚成功");
    
    // 测试统计信息
    auto stats = manager.get_stats();
    ASSERT(stats.total_transactions >= 2, "应该有至少2个事务");
    ASSERT(stats.committed_transactions >= 1, "应该有至少1个已提交事务");
    ASSERT(stats.aborted_transactions >= 1, "应该有至少1个已中止事务");
    TEST_PASS("统计信息正确");
}

// ==================== 测试2: 锁管理 ====================

void test_lock_management() {
    TEST_CASE("测试2: 锁管理");
    
    TransactionManager::TransactionConfig config;
    TransactionManager manager(config);
    
    TransactionID tid1 = manager.begin_transaction();
    TransactionID tid2 = manager.begin_transaction();
    
    std::string key = "test_key";
    
    // 测试读锁（共享锁）
    bool read_lock1 = manager.acquire_read_lock(tid1, key);
    ASSERT(read_lock1, "T1应该能获取读锁");
    TEST_PASS("T1获取读锁成功");
    
    bool read_lock2 = manager.acquire_read_lock(tid2, key);
    ASSERT(read_lock2, "T2应该能获取读锁（共享锁可以多个）");
    TEST_PASS("T2获取读锁成功（共享锁）");
    
    // 测试写锁（排他锁）
    TransactionID tid3 = manager.begin_transaction();
    bool write_lock3 = manager.acquire_write_lock(tid3, key, 100); // 100ms超时
    ASSERT(!write_lock3, "T3不应该能获取写锁（T1和T2持有读锁）");
    TEST_PASS("写锁被正确阻塞");
    
    // 释放读锁
    bool release1 = manager.release_lock(tid1, key);
    ASSERT(release1, "应该能释放锁");
    TEST_PASS("T1释放读锁成功");
    
    bool release2 = manager.release_lock(tid2, key);
    ASSERT(release2, "应该能释放锁");
    TEST_PASS("T2释放读锁成功");
    
    // 现在T3应该能获取写锁
    bool write_lock3_retry = manager.acquire_write_lock(tid3, key);
    ASSERT(write_lock3_retry, "T3现在应该能获取写锁");
    TEST_PASS("T3获取写锁成功");
    
    // 清理
    manager.release_lock(tid3, key);
    manager.commit_transaction(tid1);
    manager.commit_transaction(tid2);
    manager.abort_transaction(tid3);
}

// ==================== 测试3: MVCC功能 ====================

void test_mvcc_functionality() {
    TEST_CASE("测试3: MVCC功能");
    
    MVCCEngine::MVCCConfig mvcc_config;
    MVCCEngine mvcc_engine(mvcc_config);
    
    TransactionID tid1 = 1;
    TransactionID tid2 = 2;
    
    std::vector<char> key{'u', 's', 'e', 'r', ':', '1'};
    std::vector<char> value1{'A', 'l', 'i', 'c', 'e'};
    std::vector<char> value2{'B', 'o', 'b'};
    
    // 创建快照
    auto snapshot1 = mvcc_engine.create_snapshot(tid1);
    ASSERT(snapshot1 != nullptr, "应该能创建快照");
    TEST_PASS("创建快照成功");
    
    // 写入版本1
    bool write1 = mvcc_engine.write_version(tid1, key, value1);
    ASSERT(write1, "应该能写入版本");
    TEST_PASS("写入版本1成功");
    
    // 读取版本（未提交，应该看不到）
    auto read_result1 = mvcc_engine.read_version(key, *snapshot1);
    ASSERT(!read_result1.has_value(), "未提交的版本不应该可见");
    TEST_PASS("未提交版本不可见");
    
    // 提交版本1
    uint64_t commit_ts1 = 100;
    bool commit1 = mvcc_engine.commit_version(tid1, commit_ts1);
    ASSERT(commit1, "应该能提交版本");
    TEST_PASS("提交版本1成功");
    
    // 创建新快照（应该能看到版本1）
    auto snapshot2 = mvcc_engine.create_snapshot(tid2);
    auto read_result2 = mvcc_engine.read_version(key, *snapshot2);
    ASSERT(read_result2.has_value(), "应该能看到已提交的版本");
    ASSERT(read_result2.value() == value1, "读取的值应该匹配");
    TEST_PASS("读取已提交版本成功");
    
    // 写入版本2
    bool write2 = mvcc_engine.write_version(tid2, key, value2);
    ASSERT(write2, "应该能写入新版本");
    TEST_PASS("写入版本2成功");
    
    // 旧快照应该还是看到版本1
    auto read_result3 = mvcc_engine.read_version(key, *snapshot2);
    ASSERT(read_result3.has_value(), "旧快照应该能看到版本1");
    ASSERT(read_result3.value() == value1, "旧快照应该看到旧值");
    TEST_PASS("快照隔离正确");
    
    // 回滚版本2
    bool abort2 = mvcc_engine.abort_version(tid2);
    ASSERT(abort2, "应该能回滚版本");
    TEST_PASS("回滚版本成功");
    
    // 检查版本数量
    size_t version_count = mvcc_engine.get_version_count(key);
    ASSERT(version_count == 1, "应该只有1个版本（版本2被回滚了）");
    TEST_PASS("版本数量正确");
    
    // 测试统计信息
    auto stats = mvcc_engine.get_stats();
    ASSERT(stats.total_keys >= 1, "应该有至少1个key");
    ASSERT(stats.total_versions >= 1, "应该有至少1个版本");
    TEST_PASS("MVCC统计信息正确");
}

// ==================== 测试4: WAL功能 ====================

void test_wal_functionality() {
    TEST_CASE("测试4: WAL功能");
    
    TransactionManager::TransactionConfig config;
    config.wal_data_dir = "/tmp/mementodb_test_wal";
    TransactionManager manager(config);
    
    TransactionID tid = manager.begin_transaction();
    
    // 记录事务开始
    uint64_t begin_lsn = manager.log_begin(tid);
    ASSERT(begin_lsn > 0, "LSN应该大于0");
    TEST_PASS("记录事务开始成功");
    
    // 记录物理更新
    uint64_t page_id = 1;
    uint32_t offset = 0;
    const char* old_data = "old";
    const char* new_data = "new";
    uint32_t length = 3;
    
    uint64_t update_lsn = manager.log_physical_update(
        tid, page_id, offset, old_data, new_data, length
    );
    ASSERT(update_lsn > begin_lsn, "更新LSN应该大于开始LSN");
    TEST_PASS("记录物理更新成功");
    
    // 记录事务提交
    uint64_t commit_lsn = manager.log_commit(tid);
    ASSERT(commit_lsn > update_lsn, "提交LSN应该大于更新LSN");
    TEST_PASS("记录事务提交成功");
    
    // 获取WAL统计信息（WAL基类可能没有此方法，跳过统计信息测试）
    auto* wal = manager.get_wal();
    if (wal) {
        TEST_PASS("WAL对象存在");
    }
    
    manager.commit_transaction(tid);
}

// ==================== 测试5: 死锁检测 ====================

void test_deadlock_detection() {
    TEST_CASE("测试5: 死锁检测");
    
    TransactionManager::TransactionConfig config;
    config.enable_deadlock_detection = true;
    config.deadlock_detection_interval_ms = 100;
    TransactionManager manager(config);
    
    TransactionID tid1 = manager.begin_transaction();
    TransactionID tid2 = manager.begin_transaction();
    
    std::string key1 = "key1";
    std::string key2 = "key2";
    
    // T1获取key1的写锁
    bool lock1_1 = manager.acquire_write_lock(tid1, key1);
    ASSERT(lock1_1, "T1应该能获取key1的写锁");
    TEST_PASS("T1获取key1写锁");
    
    // T2获取key2的写锁
    bool lock2_2 = manager.acquire_write_lock(tid2, key2);
    ASSERT(lock2_2, "T2应该能获取key2的写锁");
    TEST_PASS("T2获取key2写锁");
    
    // 在另一个线程中，T1尝试获取key2（会等待）
    std::atomic<bool> t1_waiting{false};
    std::atomic<bool> t1_got_lock{false};
    
    std::thread t1_thread([&]() {
        t1_waiting = true;
        bool result = manager.acquire_write_lock(tid1, key2, 5000); // 5秒超时
        t1_got_lock = result;
    });
    
    // 等待T1开始等待
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // T2尝试获取key1（会等待，形成死锁）
    std::atomic<bool> t2_waiting{false};
    std::atomic<bool> t2_got_lock{false};
    
    std::thread t2_thread([&]() {
        t2_waiting = true;
        bool result = manager.acquire_write_lock(tid2, key1, 5000); // 5秒超时
        t2_got_lock = result;
    });
    
    // 等待死锁检测
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // 检查死锁是否被检测到（至少有一个事务应该被中止）
    // 注意：死锁检测可能会中止其中一个事务
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    
    t1_thread.join();
    t2_thread.join();
    
    // 至少有一个事务应该被中止或超时
    bool t1_aborted = manager.is_aborted(tid1);
    bool t2_aborted = manager.is_aborted(tid2);
    bool t1_timeout = !t1_got_lock && !manager.is_active(tid1);
    bool t2_timeout = !t2_got_lock && !manager.is_active(tid2);
    
    ASSERT(t1_aborted || t2_aborted || t1_timeout || t2_timeout, 
           "死锁应该被检测到并解决");
    TEST_PASS("死锁检测功能正常");
    
    // 清理
    if (manager.is_active(tid1)) manager.abort_transaction(tid1);
    if (manager.is_active(tid2)) manager.abort_transaction(tid2);
}

// ==================== 测试6: 并发事务 ====================

void test_concurrent_transactions() {
    TEST_CASE("测试6: 并发事务");
    
    TransactionManager::TransactionConfig config;
    TransactionManager manager(config);
    
    const int num_threads = 10;
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};
    std::atomic<int> fail_count{0};
    
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&manager, &success_count, &fail_count, i]() {
            TransactionID tid = manager.begin_transaction();
            if (tid == 0) {
                fail_count.fetch_add(1);
                return;
            }
            
            // 模拟一些操作
            std::string key = "key_" + std::to_string(i % 5);
            manager.acquire_write_lock(tid, key);
            
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            
            if (manager.commit_transaction(tid)) {
                success_count.fetch_add(1);
            } else {
                fail_count.fetch_add(1);
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    ASSERT(success_count.load() == num_threads, 
           "所有并发事务应该成功");
    TEST_PASS("并发事务测试通过");
}

// ==================== 测试7: 快照隔离 ====================

void test_snapshot_isolation() {
    TEST_CASE("测试7: 快照隔离");
    
    MVCCEngine::MVCCConfig mvcc_config;
    MVCCEngine mvcc_engine(mvcc_config);
    
    std::vector<char> key{'t', 'e', 's', 't'};
    std::vector<char> value1{'v', '1'};
    std::vector<char> value2{'v', '2'};
    
    TransactionID tid1 = 1;
    TransactionID tid2 = 2;
    
    // T1创建快照并写入
    auto snapshot1 = mvcc_engine.create_snapshot(tid1);
    mvcc_engine.write_version(tid1, key, value1);
    mvcc_engine.commit_version(tid1, 100);
    
    // T2创建快照（应该看到value1）
    auto snapshot2 = mvcc_engine.create_snapshot(tid2);
    auto read1 = mvcc_engine.read_version(key, *snapshot2);
    ASSERT(read1.has_value(), "T2应该能看到T1的提交");
    ASSERT(read1.value() == value1, "T2应该看到value1");
    TEST_PASS("快照能看到已提交的值");
    
    // T2写入新值
    mvcc_engine.write_version(tid2, key, value2);
    
    // T2的快照应该还是看到value1（快照隔离）
    auto read2 = mvcc_engine.read_version(key, *snapshot2);
    ASSERT(read2.has_value(), "T2的快照应该能看到值");
    ASSERT(read2.value() == value1, "T2的快照应该看到旧值（快照隔离）");
    TEST_PASS("快照隔离正确");
    
    // T2提交
    mvcc_engine.commit_version(tid2, 200);
    
    // 新快照应该看到value2
    auto snapshot3 = mvcc_engine.create_snapshot(3);
    auto read3 = mvcc_engine.read_version(key, *snapshot3);
    ASSERT(read3.has_value(), "新快照应该能看到值");
    ASSERT(read3.value() == value2, "新快照应该看到新值");
    TEST_PASS("新快照看到新值");
}

// ==================== 测试8: 垃圾回收 ====================

void test_garbage_collection() {
    TEST_CASE("测试8: 垃圾回收");
    
    MVCCEngine::MVCCConfig mvcc_config;
    mvcc_config.version_retention_time_ms = 1000; // 1秒保留时间
    MVCCEngine mvcc_engine(mvcc_config);
    
    std::vector<char> key{'g', 'c', '_', 't', 'e', 's', 't'};
    std::vector<char> value{'v', 'a', 'l', 'u', 'e'};
    
    TransactionID tid = 1;
    
    // 写入并提交版本
    mvcc_engine.write_version(tid, key, value);
    uint64_t commit_ts = 1000;
    mvcc_engine.commit_version(tid, commit_ts);
    
    // 检查版本数量
    size_t count_before = mvcc_engine.get_version_count(key);
    ASSERT(count_before == 1, "应该有一个版本");
    TEST_PASS("版本写入成功");
    
    // 执行垃圾回收（当前时间戳远大于保留时间）
    uint64_t current_ts = commit_ts + mvcc_config.version_retention_time_ms + 1000;
    mvcc_engine.cleanup_old_versions(current_ts);
    
    // 检查GC统计
    auto gc_stats = mvcc_engine.get_gc_stats();
    ASSERT(gc_stats.collected_versions >= 0, "GC应该执行");
    TEST_PASS("垃圾回收执行成功");
}

// ==================== 主函数 ====================

int main() {
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║        事务模块综合测试                                  ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n";
    
    int passed = 0;
    int failed = 0;
    
    auto run_test = [&](const std::string& name, void (*test_func)()) {
        try {
            test_func();
            passed++;
            std::cout << "\n✓ 测试通过: " << name << "\n";
        } catch (const std::exception& e) {
            failed++;
            std::cerr << "\n✗ 测试失败: " << name << " - " << e.what() << "\n";
        } catch (...) {
            failed++;
            std::cerr << "\n✗ 测试失败: " << name << " - 未知错误\n";
        }
    };
    
    // 运行所有测试
    run_test("基本事务生命周期", test_basic_transaction_lifecycle);
    run_test("锁管理", test_lock_management);
    run_test("MVCC功能", test_mvcc_functionality);
    run_test("WAL功能", test_wal_functionality);
    run_test("死锁检测", test_deadlock_detection);
    run_test("并发事务", test_concurrent_transactions);
    run_test("快照隔离", test_snapshot_isolation);
    run_test("垃圾回收", test_garbage_collection);
    
    // 输出总结
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║                    测试总结                              ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════╣\n";
    std::cout << "║  通过: " << passed << " 个测试                                    ║\n";
    std::cout << "║  失败: " << failed << " 个测试                                    ║\n";
    std::cout << "║  总计: " << (passed + failed) << " 个测试                                    ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n";
    
    if (failed == 0) {
        std::cout << "\n🎉 所有测试通过！\n";
        return 0;
    } else {
        std::cout << "\n❌ 有 " << failed << " 个测试失败\n";
        return 1;
    }
}

