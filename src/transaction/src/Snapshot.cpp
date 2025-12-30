// File: src/transaction/src/Snapshot.cpp
// 快照管理

#include "Snapshot.hpp"
#include <shared_mutex>

namespace mementodb {
namespace transaction {

Snapshot::Snapshot(TransactionID tid, uint64_t timestamp)
    : transaction_id_(tid)
    , timestamp_(timestamp)
{
}

void Snapshot::add_read_key(const std::vector<char>& key) {
    std::lock_guard<std::shared_mutex> lock(read_keys_mutex_);
    read_keys_.insert(key);
}

bool Snapshot::has_read_key(const std::vector<char>& key) const {
    std::shared_lock<std::shared_mutex> lock(read_keys_mutex_);
    return read_keys_.find(key) != read_keys_.end();
}

bool Snapshot::is_valid(uint64_t current_timestamp) const {
    // 快照在时间戳范围内有效
    // 这里可以根据需要实现更复杂的逻辑
    return current_timestamp >= timestamp_;
}

} // namespace transaction
} // namespace mementodb

