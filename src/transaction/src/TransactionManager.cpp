// File: src/transaction/src/TransactionManager.cpp
// 事务管理器实现

#include "TransactionManager.hpp"
#include "FileWAL.hpp"
#include <filesystem>

namespace mementodb {
namespace transaction {

TransactionManager::TransactionManager(const TransactionConfig& config)
    : config_(config)
{
    // 初始化 WAL
    WALConfig wal_config;
    if (!config_.wal_data_dir.empty()) {
        std::filesystem::create_directories(config_.wal_data_dir);
        wal_config.log_dir = config_.wal_data_dir;
    } else {
        std::filesystem::create_directories("/tmp/mementodb_wal");
        wal_config.log_dir = "/tmp/mementodb_wal";
    }
    wal_ = std::make_unique<FileWAL>(wal_config);
    
    // 初始化恢复管理器（暂时不注册协议，避免链接错误）
    recovery_manager_ = std::make_unique<RecoveryManager>(wal_.get());
    // 注意：如果需要恢复功能，需要实现 ARIESRecoveryProtocol 和 SimpleRedoUndoProtocol
    
    // 初始化锁管理器
    lock_manager_ = std::make_unique<LockManager>();
    
    if (config_.enable_deadlock_detection) {
        lock_manager_->enable_deadlock_detection(
            config_.deadlock_detection_interval_ms
        );
    }
}

TransactionManager::~TransactionManager() {
    // 清理所有活跃事务
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [tid, ctx] : transactions_) {
        if (ctx && ctx->get_state() == TransactionState::ACTIVE) {
            abort_transaction(tid);
        }
    }
}

TransactionID TransactionManager::generate_transaction_id() {
    return next_transaction_id_.fetch_add(1);
}

TransactionID TransactionManager::begin_transaction(IsolationLevel isolation_level) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    TransactionID tid = generate_transaction_id();
    
    // 创建事务上下文
    auto ctx = TransactionContext::create(tid, isolation_level);
    if (!ctx) {
        return 0;
    }
    
    // 记录到 WAL
    uint64_t lsn = wal_->log_begin(tid);
    ctx->set_first_lsn(lsn);
    
    // 存储事务
    transactions_[tid] = ctx;
    
    return tid;
}

bool TransactionManager::commit_transaction(TransactionID tid) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = transactions_.find(tid);
    if (it == transactions_.end()) {
        return false;
    }
    
    auto ctx = it->second;
    if (ctx->get_state() != TransactionState::ACTIVE) {
        return false;
    }
    
    // 记录提交到 WAL
    uint64_t commit_lsn = wal_->log_commit(tid);
    ctx->set_last_lsn(commit_lsn);
    
    // 刷新 WAL
    wal_->flush();
    
    // 释放所有锁
    for (const auto& lock_ref : ctx->get_lock_references()) {
        lock_manager_->release_lock(lock_ref.resource_id, tid);
    }
    
    // 设置提交时间戳
    ctx->set_commit_timestamp(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
    
    // 更新状态为已提交
    ctx->transition_state(TransactionState::COMMITTED);
    
    // 移除事务（延迟到析构时）
    // remove_transaction(tid);
    
    return true;
}

bool TransactionManager::abort_transaction(TransactionID tid) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = transactions_.find(tid);
    if (it == transactions_.end()) {
        return false;
    }
    
    auto ctx = it->second;
    if (ctx->get_state() != TransactionState::ACTIVE) {
        return false;
    }
    
    // 记录中止到 WAL
    uint64_t abort_lsn = wal_->log_abort(tid);
    ctx->set_last_lsn(abort_lsn);
    
    // 释放所有锁
    for (const auto& lock_ref : ctx->get_lock_references()) {
        lock_manager_->release_lock(lock_ref.resource_id, tid);
    }
    
    // 更新状态为已中止
    ctx->transition_state(TransactionState::ABORTED);
    
    // 移除事务（延迟到析构时）
    // remove_transaction(tid);
    
    return true;
}

std::shared_ptr<TransactionContext> TransactionManager::get_transaction(TransactionID tid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = transactions_.find(tid);
    if (it != transactions_.end()) {
        return it->second;
    }
    return nullptr;
}

bool TransactionManager::is_active(TransactionID tid) const {
    auto ctx = get_transaction(tid);
    return ctx && ctx->is_active();
}

bool TransactionManager::is_committed(TransactionID tid) const {
    auto ctx = get_transaction(tid);
    return ctx && ctx->get_state() == TransactionState::COMMITTED;
}

bool TransactionManager::is_aborted(TransactionID tid) const {
    auto ctx = get_transaction(tid);
    return ctx && ctx->get_state() == TransactionState::ABORTED;
}

bool TransactionManager::acquire_read_lock(TransactionID tid, const std::string& key, uint64_t timeout_ms) {
    auto ctx = get_transaction(tid);
    if (!ctx || ctx->get_state() != TransactionState::ACTIVE) {
        return false;
    }
    
    auto result = lock_manager_->acquire_read_lock(key, tid, timeout_ms);
    if (result == LockManager::LockResult::SUCCESS) {
        ctx->add_lock_reference(nullptr, key, LockMode::SHARED);
        return true;
    }
    return false;
}

bool TransactionManager::acquire_write_lock(TransactionID tid, const std::string& key, uint64_t timeout_ms) {
    auto ctx = get_transaction(tid);
    if (!ctx || ctx->get_state() != TransactionState::ACTIVE) {
        return false;
    }
    
    auto result = lock_manager_->acquire_write_lock(key, tid, timeout_ms);
    if (result == LockManager::LockResult::SUCCESS) {
        ctx->add_lock_reference(nullptr, key, LockMode::EXCLUSIVE);
        return true;
    }
    return false;
}

bool TransactionManager::release_lock(TransactionID tid, const std::string& key) {
    auto ctx = get_transaction(tid);
    if (!ctx) {
        return false;
    }
    
    bool result = lock_manager_->release_lock(key, tid);
    if (result) {
        // 从上下文中移除锁引用
        const auto& lock_refs = ctx->get_lock_references();
        for (const auto& lock_ref : lock_refs) {
            if (lock_ref.resource_id == key) {
                ctx->remove_lock_reference(lock_ref.lock_object);
                break;
            }
        }
    }
    return result;
}

uint64_t TransactionManager::log_begin(TransactionID tid) {
    return wal_->log_begin(tid);
}

uint64_t TransactionManager::log_physical_update(
    TransactionID tid,
    PageID page_id,
    uint32_t offset,
    const char* old_data,
    const char* new_data,
    uint32_t length
) {
    return wal_->log_physical_update(tid, page_id, offset, old_data, new_data, length);
}

uint64_t TransactionManager::log_commit(TransactionID tid) {
    return wal_->log_commit(tid);
}

uint64_t TransactionManager::log_abort(TransactionID tid) {
    return wal_->log_abort(tid);
}

size_t TransactionManager::recover(
    RecoveryManager::RecoveryCallback redo_callback,
    RecoveryManager::RecoveryCallback undo_callback
) {
    // 使用 perform_crash_recovery 进行恢复
    RecoveryConfig recovery_config;
    auto result = recovery_manager_->perform_crash_recovery(recovery_config);
    return result.transactions_recovered;
}

TransactionManager::Stats TransactionManager::get_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    Stats stats{};
    stats.total_transactions = transactions_.size();
    
    for (const auto& [tid, ctx] : transactions_) {
        auto state = ctx->get_state();
        if (state == TransactionState::ACTIVE) {
            stats.active_transactions++;
        } else if (state == TransactionState::COMMITTED) {
            stats.committed_transactions++;
        } else if (state == TransactionState::ABORTED) {
            stats.aborted_transactions++;
        }
    }
    
    return stats;
}

void TransactionManager::remove_transaction(TransactionID tid) {
    transactions_.erase(tid);
}

} // namespace transaction
} // namespace mementodb

