// File: src/transaction/tests/TransactionTest.cpp
// 事务模块测试

#include "../src/TransactionManager.hpp"
#include "../include/Transaction.hpp"
#include "../include/IsolationLevel.hpp"
#include <iostream>
#include <cassert>
#include <thread>
#include <vector>

using namespace mementodb::transaction;

void test_basic_transaction() {
    std::cout << "测试：基本事务操作" << std::endl;
    
    TransactionManager::TransactionConfig config;
    TransactionManager manager(config);
    
    // 开始事务
    auto tid = manager.begin_transaction(IsolationLevel::REPEATABLE_READ);
    auto txn = manager.get_transaction(tid);
    assert(txn != nullptr);
    assert(txn->is_active());
    
    // 提交事务
    bool result = manager.commit_transaction(tid);
    assert(result);
    assert(manager.is_committed(tid));
    assert(!txn->is_active());
    
    std::cout << "✓ 基本事务操作测试通过" << std::endl;
}

void test_transaction_rollback() {
    std::cout << "测试：事务回滚" << std::endl;
    
    TransactionManager::TransactionConfig config;
    TransactionManager manager(config);
    
    auto tid = manager.begin_transaction(IsolationLevel::REPEATABLE_READ);
    auto txn = manager.get_transaction(tid);
    assert(txn != nullptr);
    assert(txn->is_active());
    
    // 回滚事务
    bool result = manager.rollback_transaction(tid);
    assert(result);
    assert(manager.is_aborted(tid));
    assert(!txn->is_active());
    
    std::cout << "✓ 事务回滚测试通过" << std::endl;
}

void test_concurrent_transactions() {
    std::cout << "测试：并发事务" << std::endl;
    
    TransactionManager::TransactionConfig config;
    TransactionManager manager(config);
    
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};
    
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&manager, &success_count]() {
            auto tid = manager.begin_transaction(IsolationLevel::REPEATABLE_READ);
            auto txn = manager.get_transaction(tid);
            if (txn) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                if (manager.commit_transaction(tid)) {
                    success_count.fetch_add(1);
                }
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    assert(success_count.load() == 10);
    std::cout << "✓ 并发事务测试通过" << std::endl;
}

int main() {
    std::cout << "开始事务模块测试..." << std::endl;
    
    try {
        test_basic_transaction();
        test_transaction_rollback();
        test_concurrent_transactions();
        
        std::cout << "\n所有测试通过！" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "测试失败: " << e.what() << std::endl;
        return 1;
    }
}

