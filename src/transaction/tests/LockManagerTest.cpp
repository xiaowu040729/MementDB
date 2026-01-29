// File: src/transaction/tests/LockManagerTest.cpp
// 锁管理器测试

#include "../src/LockManager.hpp"
#include "../include/Transaction.hpp"
#include <iostream>
#include <cassert>
#include <thread>
#include <vector>
#include <chrono>
#include <string>

using namespace mementodb::transaction;

void test_basic_lock() {
    std::cout << "测试：基本锁操作" << std::endl;
    
    LockManager lock_manager;
    
    TransactionID tid1 = 1;
    std::string key1 = "key1";
    
    // 获取读锁
    auto result = lock_manager.acquire_read_lock(key1, tid1);
    assert(result == LockManager::LockResult::SUCCESS);
    
    // 释放读锁
    lock_manager.release_lock(key1, tid1);
    
    std::cout << "✓ 基本锁操作测试通过" << std::endl;
}

void test_write_lock_exclusive() {
    std::cout << "测试：写锁独占性" << std::endl;
    
    LockManager lock_manager;
    
    TransactionID tid1 = 1;
    TransactionID tid2 = 2;
    std::string key1 = "key1";
    
    // tid1 获取写锁
    auto result1 = lock_manager.acquire_write_lock(key1, tid1);
    assert(result1 == LockManager::LockResult::SUCCESS);
    
    // tid2 尝试获取写锁（应该失败或等待）
    auto result2 = lock_manager.acquire_write_lock(key1, tid2, 100); // 100ms 超时
    assert(result2 != LockManager::LockResult::SUCCESS); // 应该超时失败
    
    // tid1 释放写锁
    lock_manager.release_lock(key1, tid1);
    
    // 现在 tid2 应该能获取写锁
    result2 = lock_manager.acquire_write_lock(key1, tid2);
    assert(result2 == LockManager::LockResult::SUCCESS);
    
    lock_manager.release_lock(key1, tid2);
    
    std::cout << "✓ 写锁独占性测试通过" << std::endl;
}

void test_read_write_conflict() {
    std::cout << "测试：读写冲突" << std::endl;
    
    LockManager lock_manager;
    
    TransactionID tid1 = 1;
    TransactionID tid2 = 2;
    std::string key1 = "key1";
    
    // tid1 获取读锁
    auto result1 = lock_manager.acquire_read_lock(key1, tid1);
    assert(result1 == LockManager::LockResult::SUCCESS);
    
    // tid2 尝试获取写锁（应该失败或等待）
    auto result2 = lock_manager.acquire_write_lock(key1, tid2, 100);
    assert(result2 != LockManager::LockResult::SUCCESS); // 应该超时失败
    
    // tid1 释放读锁
    lock_manager.release_lock(key1, tid1);
    
    // 现在 tid2 应该能获取写锁
    result2 = lock_manager.acquire_write_lock(key1, tid2);
    assert(result2 == LockManager::LockResult::SUCCESS);
    
    lock_manager.release_lock(key1, tid2);
    
    std::cout << "✓ 读写冲突测试通过" << std::endl;
}

int main() {
    std::cout << "开始锁管理器测试..." << std::endl;
    
    try {
        test_basic_lock();
        test_write_lock_exclusive();
        test_read_write_conflict();
        
        std::cout << "\n所有测试通过！" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "测试失败: " << e.what() << std::endl;
        return 1;
    }
}

