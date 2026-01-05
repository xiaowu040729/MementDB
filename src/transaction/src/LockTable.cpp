// File: src/transaction/src/LockTable.cpp
// 锁表实现（最小可用版本）

#include "LockTable.hpp"
#include <thread>

namespace mementodb {
namespace transaction {

// ===== LockEntry =====
LockEntry::LockEntry() = default;

bool LockEntry::try_acquire(TransactionID tid, LockMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 已持有排他锁的事务重复获取直接成功
    if (exclusive_holder_.has_value() && exclusive_holder_.value() == tid) {
        return true;
    }

    if (mode == LockMode::SHARED) {
        // 如果有其他事务持有排他锁则失败
        if (exclusive_holder_.has_value() && exclusive_holder_.value() != tid) {
            return false;
        }
        shared_holders_.insert(tid);
        cv_.notify_all();
        return true;
    }

    // EXCLUSIVE
    if (exclusive_holder_.has_value() && exclusive_holder_.value() != tid) {
        return false;
    }
    // 允许从自己持有的共享锁升级
    if (!shared_holders_.empty()) {
        if (shared_holders_.size() == 1 && shared_holders_.count(tid) == 1) {
            shared_holders_.clear();
        } else {
            return false;
        }
    }
    exclusive_holder_ = tid;
    cv_.notify_all();
    return true;
}

bool LockEntry::release(TransactionID tid) {
    std::lock_guard<std::mutex> lock(mutex_);

    bool released = false;
    if (exclusive_holder_.has_value() && exclusive_holder_.value() == tid) {
        exclusive_holder_.reset();
        released = true;
    }
    if (shared_holders_.erase(tid) > 0) {
        released = true;
    }

    if (released) {
        cv_.notify_all();
    }
    return released;
}

bool LockEntry::has_lock(TransactionID tid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (exclusive_holder_.has_value() && exclusive_holder_.value() == tid) {
        return true;
    }
    return shared_holders_.count(tid) > 0;
}

LockMode LockEntry::get_olock_mode(TransactionID tid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (exclusive_holder_.has_value() && exclusive_holder_.value() == tid) {
        return LockMode::EXCLUSIVE;
    }
    if (shared_holders_.count(tid) > 0) {
        return LockMode::SHARED;
    }
    return LockMode::SHARED;
}

void LockEntry::add_request(const LockRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    waiting_queue_.push(request);
}

void LockEntry::remove_request(TransactionID tid) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::queue<LockRequest> new_q;
    while (!waiting_queue_.empty()) {
        auto req = waiting_queue_.front();
        waiting_queue_.pop();
        if (req.transaction_id != tid) {
            new_q.push(req);
        }
    }
    waiting_queue_ = std::move(new_q);
}

std::vector<TransactionID> LockEntry::get_waiting_transactions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TransactionID> res;
    auto q = waiting_queue_;
    while (!q.empty()) {
        res.push_back(q.front().transaction_id);
        q.pop();
    }
    return res;
}

std::vector<TransactionID> LockEntry::get_holders() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TransactionID> res;
    if (exclusive_holder_.has_value()) {
        res.push_back(exclusive_holder_.value());
    }
    for (auto tid : shared_holders_) {
        res.push_back(tid);
    }
    return res;
}

// ===== LockTable =====
LockTable::LockTable() = default;

LockEntry* LockTable::get_or_create_entry(const std::string& key) {
    auto it = lock_table_.find(key);
    if (it == lock_table_.end()) {
        auto entry = std::make_unique<LockEntry>();
        auto* ptr = entry.get();
        lock_table_[key] = std::move(entry);
        return ptr;
    }
    return it->second.get();
}

LockEntry* LockTable::get_entry(const std::string& key) const {
    auto it = lock_table_.find(key);
    if (it == lock_table_.end()) return nullptr;
    return it->second.get();
}

bool LockTable::acquire_lock(const std::string& key, TransactionID tid,
                             LockMode mode, uint64_t timeout_ms) {
    auto* entry = get_or_create_entry(key);
    auto start = std::chrono::steady_clock::now();

    // 简单等待重试策略
    while (true) {
        if (entry->try_acquire(tid, mode)) {
            std::lock_guard<std::mutex> lock(mutex_);
            transaction_locks_[tid].insert(key);
            return true;
        }

        entry->add_request(LockRequest(tid, mode));

        if (timeout_ms == 0) {
            entry->remove_request(tid);
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() >=
            timeout_ms) {
            entry->remove_request(tid);
            return false;
        }
    }
}

bool LockTable::release_lock(const std::string& key, TransactionID tid) {
    auto* entry = get_entry(key);
    if (!entry) return false;

    bool released = entry->release(tid);
    if (released) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = transaction_locks_.find(tid);
        if (it != transaction_locks_.end()) {
            it->second.erase(key);
            if (it->second.empty()) {
                transaction_locks_.erase(it);
            }
        }
    }
    return released;
}

size_t LockTable::release_all_locks(TransactionID tid) {
    std::vector<std::string> keys;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = transaction_locks_.find(tid);
        if (it != transaction_locks_.end()) {
            keys.assign(it->second.begin(), it->second.end());
        }
    }

    size_t count = 0;
    for (const auto& key : keys) {
        if (release_lock(key, tid)) {
            ++count;
        }
    }
    return count;
}

bool LockTable::has_lock(const std::string& key, TransactionID tid) const {
    auto* entry = get_entry(key);
    if (!entry) return false;
    return entry->has_lock(tid);
}

LockMode LockTable::get_lock_mode(const std::string& key, TransactionID tid) const {
    auto* entry = get_entry(key);
    if (!entry) return LockMode::SHARED;
    return entry->get_olock_mode(tid);
}

std::vector<TransactionID> LockTable::get_waiting_transactions(const std::string& key) const {
    auto* entry = get_entry(key);
    if (!entry) return {};
    return entry->get_waiting_transactions();
}

std::vector<TransactionID> LockTable::get_holders(const std::string& key) const {
    auto* entry = get_entry(key);
    if (!entry) return {};
    return entry->get_holders();
}

std::vector<std::string> LockTable::get_all_locks(TransactionID tid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> res;
    auto it = transaction_locks_.find(tid);
    if (it != transaction_locks_.end()) {
        res.assign(it->second.begin(), it->second.end());
    }
    return res;
}

void LockTable::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    lock_table_.clear();
    transaction_locks_.clear();
}

std::vector<std::string> LockTable::get_all_keys() const {
    std::vector<std::string> keys;
    keys.reserve(lock_table_.size());
    for (const auto& kv : lock_table_) {
        keys.push_back(kv.first);
    }
    return keys;
}

LockTable::Stats LockTable::get_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats s{};
    s.total_entries = lock_table_.size();
    for (const auto& kv : lock_table_) {
        auto holders = kv.second->get_holders();
        s.total_held_locks += holders.size();
        s.total_waiting += kv.second->get_waiting_transactions().size();
    }
    return s;
}

} // namespace transaction
} // namespace mementodb


