// File: src/transaction/tests/WALTest.cpp
// WAL测试

#include "../include/WALInterface.hpp"
#include "../src/FileWAL.hpp"
#include <iostream>
#include <cassert>
#include <filesystem>
#include <cstring>

using namespace mementodb::transaction;

void test_wal_basic_operations() {
    std::cout << "测试：WAL基本操作" << std::endl;
    
    // 创建临时目录
    std::string log_dir = "/tmp/mementodb_wal_test";
    std::filesystem::create_directories(log_dir);
    
    // 创建 FileWAL
    FileWAL wal(log_dir, 8192, true);
    
    TransactionID tid1 = 1;
    TransactionID tid2 = 2;
    PageID page_id = 100;
    
    // 测试 log_begin
    uint64_t lsn1 = wal.log_begin(tid1);
    assert(lsn1 > 0);
    std::cout << "  LSN for BEGIN: " << lsn1 << std::endl;
    
    // 测试 log_physical_update
    const char* old_data = "old_data";
    const char* new_data = "new_data";
    uint64_t lsn2 = wal.log_physical_update(tid1, page_id, 0, old_data, new_data, 8);
    assert(lsn2 > lsn1);
    std::cout << "  LSN for UPDATE: " << lsn2 << std::endl;
    
    // 测试 log_commit
    uint64_t lsn3 = wal.log_commit(tid1);
    assert(lsn3 > lsn2);
    std::cout << "  LSN for COMMIT: " << lsn3 << std::endl;
    
    // 测试读取日志记录
    auto record = wal.read_log_record(lsn1);
    assert(!record.empty());
    std::cout << "  读取日志记录大小: " << record.size() << std::endl;
    
    // 测试 flush
    wal.flush();
    
    // 测试 get_current_lsn
    uint64_t current_lsn = wal.get_current_lsn();
    assert(current_lsn >= lsn3);
    std::cout << "  当前 LSN: " << current_lsn << std::endl;
    
    // 清理
    std::filesystem::remove_all(log_dir);
    
    std::cout << "✓ WAL基本操作测试通过" << std::endl;
}

void test_wal_checkpoint() {
    std::cout << "测试：WAL检查点" << std::endl;
    
    std::string log_dir = "/tmp/mementodb_wal_test_checkpoint";
    std::filesystem::create_directories(log_dir);
    
    FileWAL wal(log_dir, 8192, true);
    
    TransactionID tid1 = 1;
    TransactionID tid2 = 2;
    
    wal.log_begin(tid1);
    wal.log_begin(tid2);
    
    // 创建检查点
    std::set<TransactionID> active_txns = {tid1, tid2};
    uint64_t checkpoint_lsn = wal.write_checkpoint(active_txns);
    assert(checkpoint_lsn > 0);
    std::cout << "  检查点 LSN: " << checkpoint_lsn << std::endl;
    
    // 清理
    std::filesystem::remove_all(log_dir);
    
    std::cout << "✓ WAL检查点测试通过" << std::endl;
}

void test_wal_scanner() {
    std::cout << "测试：WAL扫描器" << std::endl;
    
    std::string log_dir = "/tmp/mementodb_wal_test_scanner";
    std::filesystem::create_directories(log_dir);
    
    FileWAL wal(log_dir, 8192, true);
    
    TransactionID tid1 = 1;
    wal.log_begin(tid1);
    wal.log_commit(tid1);
    
    // 创建扫描器
    auto scanner = wal.create_scanner();
    assert(scanner != nullptr);
    
    bool opened = scanner->open(0);
    assert(opened);
    
    int record_count = 0;
    std::vector<char> record;
    while (scanner->next(record)) {
        record_count++;
        assert(!record.empty());
    }
    
    assert(record_count >= 2); // 至少应该有 BEGIN 和 COMMIT
    std::cout << "  扫描到 " << record_count << " 条记录" << std::endl;
    
    // 清理
    std::filesystem::remove_all(log_dir);
    
    std::cout << "✓ WAL扫描器测试通过" << std::endl;
}

int main() {
    std::cout << "开始WAL测试..." << std::endl;
    
    try {
        test_wal_basic_operations();
        test_wal_checkpoint();
        test_wal_scanner();
        
        std::cout << "\n所有测试通过！" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "测试失败: " << e.what() << std::endl;
        return 1;
    }
}

