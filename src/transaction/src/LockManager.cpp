// File: src/transaction/src/LockManager.cpp
// 锁管理器实现（简化版，不启用真实死锁检测）

#include "LockManager.hpp"
#include "TransactionManager.hpp"
#include <thread>

namespace mementodb {
namespace transaction {

LockManager::LockManager()
    : lock_table_(std::make_unique<LockTable>())
    , graph_builder_(nullptr)
    , deadlock_detector_(nullptr)
    , transaction_manager_(nullptr) {
}

LockManager::LockManager(TransactionManager* transaction_manager)
    : lock_table_(std::make_unique<LockTable>())
    , graph_builder_(nullptr)
    , deadlock_detector_(nullptr)
    , transaction_manager_(transaction_manager) {
}

LockManager::~LockManager() = default;

LockManager::LockResult LockManager::acquire_read_lock(const std::string& key,
                                                       TransactionID tid,
                                                       uint64_t timeout_ms) {
    bool ok = lock_table_->acquire_lock(key, tid, LockMode::SHARED, timeout_ms);
    if (ok) {
        total_locks_acquired_++;
        return LockResult::SUCCESS;
    }
    total_timeouts_++;
    return LockResult::TIMEOUT;
}

LockManager::LockResult LockManager::acquire_write_lock(const std::string& key,
                                                        TransactionID tid,
                                                        uint64_t timeout_ms) {
    bool ok = lock_table_->acquire_lock(key, tid, LockMode::EXCLUSIVE, timeout_ms);
    if (ok) {
        total_locks_acquired_++;
        return LockResult::SUCCESS;
    }
    total_timeouts_++;
    return LockResult::TIMEOUT;
}

bool LockManager::release_lock(const std::string& key, TransactionID tid) {
    bool ok = lock_table_->release_lock(key, tid);
    if (ok) {
        total_locks_released_++;
    }
    return ok;
}

size_t LockManager::release_all_locks(TransactionID tid) {
    size_t count = lock_table_->release_all_locks(tid);
    total_locks_released_ += count;
    return count;
}

bool LockManager::has_read_lock(const std::string& key, TransactionID tid) const {
    return lock_table_->get_lock_mode(key, tid) == LockMode::SHARED;
}

bool LockManager::has_write_lock(const std::string& key, TransactionID tid) const {
    return lock_table_->get_lock_mode(key, tid) == LockMode::EXCLUSIVE;
}

LockManager::LockResult LockManager::upgrade_lock(const std::string& key,
                                                  TransactionID tid,
                                                  uint64_t timeout_ms) {
    // 先尝试获取写锁（LockTable 会在持有共享锁且唯一持有者时允许升级）
    return acquire_write_lock(key, tid, timeout_ms);
}

void LockManager::enable_deadlock_detection(uint64_t /*interval_ms*/) {
    // 简化版本暂不实现死锁检测
}

void LockManager::disable_deadlock_detection() {
    // 简化版本暂不实现
}

TransactionID LockManager::handle_deadlock(const DeadlockInfo& /*deadlock_info*/) {
    // 简化版本：返回 0 表示未处理
    return 0;
}

void LockManager::set_deadlock_config(const DeadlockDetectionConfig& /*config*/) {
    // 简化版本暂不实现
}

LockManager::Stats LockManager::get_stats() const {
    LockManager::Stats s{};
    s.total_locks_acquired = total_locks_acquired_.load();
    s.total_locks_released = total_locks_released_.load();
    s.total_timeouts = total_timeouts_.load();
    s.total_deadlocks = total_deadlocks_.load();
    // current_held_locks 通过锁表统计
    auto lt_stats = lock_table_->get_stats();
    s.current_held_locks = lt_stats.total_held_locks;
    return s;
}

void LockManager::initialize_deadlock_detector() {
    // 简化版本暂不实现
}

} // namespace transaction
} // namespace mementodb


