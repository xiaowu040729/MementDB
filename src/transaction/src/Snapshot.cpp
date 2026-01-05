// File: src/transaction/src/Snapshot.cpp
// 快照管理实现（增强版）

#include "Snapshot.hpp"
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace mementodb {
namespace transaction {

namespace {
std::atomic<uint64_t> g_next_snapshot_id{1};
}

// ========== Snapshot 基类实现 ==========

Snapshot::Snapshot(
    uint64_t snapshot_id,
    SnapshotType type,
    uint64_t snapshot_timestamp,
    std::set<TransactionID> active_transactions,
    TransactionID creator_txid
)
    : snapshot_id_(snapshot_id)
    , type_(type)
    , snapshot_timestamp_(snapshot_timestamp)
    , active_transactions_(std::move(active_transactions))
    , creator_txid_(creator_txid)
{
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    creation_time_ = static_cast<uint64_t>(now_ms);
}

Snapshot::Ptr Snapshot::create(
    SnapshotType type,
    uint64_t snapshot_timestamp,
    std::set<TransactionID> active_transactions,
    TransactionID creator_txid
) {
    uint64_t id = g_next_snapshot_id.fetch_add(1);
    
    switch (type) {
        case SnapshotType::READ_COMMITTED:
            return std::make_shared<ReadCommittedSnapshot>(
                id, snapshot_timestamp, std::move(active_transactions), creator_txid);
        case SnapshotType::REPEATABLE_READ:
        case SnapshotType::SERIALIZABLE:
            // 当前实现中，可重复读和串行化使用相同的可见性规则
            return std::make_shared<RepeatableReadSnapshot>(
                id, snapshot_timestamp, std::move(active_transactions), creator_txid);
        case SnapshotType::SNAPSHOT_ISOLATION:
        case SnapshotType::CONSISTENT_READ:
        default:
            return std::make_shared<SnapshotIsolationSnapshot>(
                id, snapshot_timestamp, std::move(active_transactions), creator_txid);
    }
}

uint64_t Snapshot::get_visible_horizon() const {
    // 简化实现：直接返回快照时间戳
    return snapshot_timestamp_;
}

bool Snapshot::is_current() const {
    // 简化实现：认为最近 1 秒内创建的快照是“当前”的
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    uint64_t now = static_cast<uint64_t>(now_ms);
    return (now - creation_time_) < 1000;
}

uint64_t Snapshot::age() const {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    uint64_t now = static_cast<uint64_t>(now_ms);
    return now - creation_time_;
}

std::vector<char> Snapshot::serialize() const {
    // 简单二进制格式：id, type, timestamp, creator_txid, active_txn_count, active_txns...
    std::vector<char> data;
    auto append_u64 = [&data](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            data.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
        }
    };
    
    append_u64(snapshot_id_);
    append_u64(static_cast<uint64_t>(type_));
    append_u64(snapshot_timestamp_);
    append_u64(static_cast<uint64_t>(creator_txid_));
    append_u64(static_cast<uint64_t>(active_transactions_.size()));
    for (auto txid : active_transactions_) {
        append_u64(static_cast<uint64_t>(txid));
    }
    return data;
}

Snapshot::Ptr Snapshot::deserialize(const std::vector<char>& data) {
    auto read_u64 = [&data](size_t& offset) -> uint64_t {
        if (offset + 8 > data.size()) return 0;
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= (static_cast<uint64_t>(
                static_cast<unsigned char>(data[offset + i])) << (i * 8));
        }
        offset += 8;
        return v;
    };
    
    size_t offset = 0;
    uint64_t id = read_u64(offset);
    auto type_val = static_cast<uint64_t>(read_u64(offset));
    uint64_t ts = read_u64(offset);
    TransactionID creator = static_cast<TransactionID>(read_u64(offset));
    uint64_t count = read_u64(offset);
    
    std::set<TransactionID> active;
    for (uint64_t i = 0; i < count && offset + 8 <= data.size(); ++i) {
        active.insert(static_cast<TransactionID>(read_u64(offset)));
    }
    
    SnapshotType type = static_cast<SnapshotType>(type_val);
    
    // 直接构造对应类型的快照
    switch (type) {
        case SnapshotType::READ_COMMITTED:
            return std::make_shared<ReadCommittedSnapshot>(id, ts, std::move(active), creator);
        case SnapshotType::REPEATABLE_READ:
        case SnapshotType::SERIALIZABLE:
            return std::make_shared<RepeatableReadSnapshot>(id, ts, std::move(active), creator);
        case SnapshotType::SNAPSHOT_ISOLATION:
        case SnapshotType::CONSISTENT_READ:
        default:
            return std::make_shared<SnapshotIsolationSnapshot>(id, ts, std::move(active), creator);
    }
}

// ========== ReadCommittedSnapshot 实现 ==========

ReadCommittedSnapshot::ReadCommittedSnapshot(
    uint64_t snapshot_id,
    uint64_t snapshot_timestamp,
    std::set<TransactionID> active_transactions,
    TransactionID creator_txid
)
    : Snapshot(snapshot_id,
               SnapshotType::READ_COMMITTED,
               snapshot_timestamp,
               std::move(active_transactions),
               creator_txid)
{}

bool ReadCommittedSnapshot::is_transaction_visible(
    TransactionID /*transaction_id*/,
    uint64_t transaction_commit_timestamp
) const {
    // 读已提交：只要提交时间戳不为 0 且小于等于当前快照时间戳即可
    if (transaction_commit_timestamp == 0) {
        return false;
    }
    return transaction_commit_timestamp <= timestamp();
}

bool ReadCommittedSnapshot::is_timestamp_visible(uint64_t ts) const {
    return ts <= timestamp();
}

bool ReadCommittedSnapshot::is_version_visible(
    TransactionID /*version_create_txid*/,
    uint64_t version_create_timestamp,
    TransactionID /*version_delete_txid*/,
    uint64_t version_delete_timestamp
) const {
    // 简化规则：创建时间戳小于等于快照时间戳，且未被在快照前删除
    if (version_create_timestamp > timestamp()) {
        return false;
    }
    if (version_delete_timestamp != 0 && version_delete_timestamp <= timestamp()) {
        return false;
    }
    return true;
}

void ReadCommittedSnapshot::update_timestamp(uint64_t new_timestamp) {
    // 读已提交语义：允许快照时间戳向前推进
    if (new_timestamp > timestamp()) {
        // const_cast 只在内部使用，不暴露给外部
        auto* self = const_cast<Snapshot*>(static_cast<const Snapshot*>(this));
        // 直接修改私有成员
        struct SnapshotHack {
            uint64_t id;
            SnapshotType type;
            uint64_t ts;
        };
        auto* hack = reinterpret_cast<SnapshotHack*>(self);
        hack->ts = new_timestamp;
    }
}

// ========== RepeatableReadSnapshot 实现 ==========

RepeatableReadSnapshot::RepeatableReadSnapshot(
    uint64_t snapshot_id,
    uint64_t snapshot_timestamp,
    std::set<TransactionID> active_transactions,
    TransactionID creator_txid
)
    : Snapshot(snapshot_id,
               SnapshotType::REPEATABLE_READ,
               snapshot_timestamp,
               std::move(active_transactions),
               creator_txid)
{}

bool RepeatableReadSnapshot::is_transaction_visible(
    TransactionID transaction_id,
    uint64_t transaction_commit_timestamp
) const {
    // 在快照创建时活跃的事务不可见；否则需要已提交且提交时间 <= 快照时间戳
    if (was_transaction_active(transaction_id)) {
        return false;
    }
    if (transaction_commit_timestamp == 0) {
        return false;
    }
    return transaction_commit_timestamp <= timestamp();
}

bool RepeatableReadSnapshot::is_timestamp_visible(uint64_t ts) const {
    return ts <= timestamp();
}

bool RepeatableReadSnapshot::is_version_visible(
    TransactionID version_create_txid,
    uint64_t version_create_timestamp,
    TransactionID version_delete_txid,
    uint64_t version_delete_timestamp
) const {
    if (was_transaction_active(version_create_txid)) {
        // 创建事务在快照时还活跃 → 不可见
        return false;
    }
    if (version_create_timestamp > timestamp()) {
        return false;
    }
    if (version_delete_txid != 0 &&
        version_delete_timestamp != 0 &&
        version_delete_timestamp <= timestamp()) {
        return false;
    }
    return true;
}

// ========== SnapshotIsolationSnapshot 实现 ==========

SnapshotIsolationSnapshot::SnapshotIsolationSnapshot(
    uint64_t snapshot_id,
    uint64_t snapshot_timestamp,
    std::set<TransactionID> active_transactions,
    TransactionID creator_txid
)
    : Snapshot(snapshot_id,
               SnapshotType::SNAPSHOT_ISOLATION,
               snapshot_timestamp,
               std::move(active_transactions),
               creator_txid)
{}

bool SnapshotIsolationSnapshot::is_transaction_visible(
    TransactionID transaction_id,
    uint64_t transaction_commit_timestamp
) const {
    // 与可重复读类似
    if (was_transaction_active(transaction_id)) {
        return false;
    }
    if (transaction_commit_timestamp == 0) {
        return false;
    }
    return transaction_commit_timestamp <= timestamp();
}

bool SnapshotIsolationSnapshot::is_timestamp_visible(uint64_t ts) const {
    return ts <= timestamp();
}

bool SnapshotIsolationSnapshot::is_version_visible(
    TransactionID version_create_txid,
    uint64_t version_create_timestamp,
    TransactionID version_delete_txid,
    uint64_t version_delete_timestamp
) const {
    if (was_transaction_active(version_create_txid)) {
        return false;
    }
    if (version_create_timestamp > timestamp()) {
        return false;
    }
    if (version_delete_txid != 0 &&
        version_delete_timestamp != 0 &&
        version_delete_timestamp <= timestamp()) {
        return false;
    }
    return true;
}

void SnapshotIsolationSnapshot::add_read_key(const std::vector<char>& key) {
    std::lock_guard<std::shared_mutex> lock(rw_set_mutex_);
    read_set_.insert(key);
}

void SnapshotIsolationSnapshot::add_write_key(const std::vector<char>& key) {
    std::lock_guard<std::shared_mutex> lock(rw_set_mutex_);
    write_set_.insert(key);
}

bool SnapshotIsolationSnapshot::has_read_key(const std::vector<char>& key) const {
    std::shared_lock<std::shared_mutex> lock(rw_set_mutex_);
    return read_set_.find(key) != read_set_.end();
}

bool SnapshotIsolationSnapshot::has_write_key(const std::vector<char>& key) const {
    std::shared_lock<std::shared_mutex> lock(rw_set_mutex_);
    return write_set_.find(key) != write_set_.end();
}

// ========== SnapshotManager 实现 ==========

class SnapshotManager::Impl {
public:
    std::atomic<uint64_t> current_timestamp{1};
    std::atomic<uint64_t> total_created{0};
    
    mutable std::mutex mutex;
    std::vector<Snapshot::Ptr> snapshots;
    std::unordered_map<TransactionID, Snapshot::Ptr> txn_snapshots;
};

SnapshotManager::SnapshotManager()
    : impl_(std::make_unique<Impl>()) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    impl_->current_timestamp.store(static_cast<uint64_t>(now_ms));
}

SnapshotManager::~SnapshotManager() = default;

Snapshot::Ptr SnapshotManager::create_snapshot_for_transaction(
    TransactionID txid,
    SnapshotType type
) {
    uint64_t ts = allocate_timestamp();
    auto snapshot = Snapshot::create(type, ts, get_active_transactions(), txid);
    
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->snapshots.push_back(snapshot);
        impl_->txn_snapshots[txid] = snapshot;
        impl_->total_created.fetch_add(1);
    }
    return snapshot;
}

Snapshot::Ptr SnapshotManager::create_readonly_snapshot(SnapshotType type) {
    uint64_t ts = allocate_timestamp();
    auto snapshot = Snapshot::create(type, ts, get_active_transactions(), 0);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->snapshots.push_back(snapshot);
        impl_->total_created.fetch_add(1);
    }
    return snapshot;
}

Snapshot::Ptr SnapshotManager::get_current_snapshot() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->snapshots.empty()) {
        return nullptr;
    }
    return impl_->snapshots.back();
}

Snapshot::Ptr SnapshotManager::get_snapshot_for_transaction(TransactionID txid) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->txn_snapshots.find(txid);
    if (it == impl_->txn_snapshots.end()) {
        return nullptr;
    }
    return it->second;
}

void SnapshotManager::register_transaction_snapshot(
    TransactionID txid,
    Snapshot::Ptr snapshot
) {
    if (!snapshot) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->txn_snapshots[txid] = snapshot;
    impl_->snapshots.push_back(std::move(snapshot));
}

void SnapshotManager::unregister_transaction_snapshot(TransactionID txid) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->txn_snapshots.erase(txid);
}

std::vector<Snapshot::Ptr> SnapshotManager::get_all_snapshots() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->snapshots;
}

Snapshot::Ptr SnapshotManager::get_oldest_snapshot() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->snapshots.empty()) {
        return nullptr;
    }
    return impl_->snapshots.front();
}

size_t SnapshotManager::garbage_collect(uint64_t min_snapshot_age_ms) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    size_t before = impl_->snapshots.size();
    
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    uint64_t now = static_cast<uint64_t>(now_ms);
    
    impl_->snapshots.erase(
        std::remove_if(
            impl_->snapshots.begin(),
            impl_->snapshots.end(),
            [&](const Snapshot::Ptr& s) {
                return s && (now - s->timestamp()) >= min_snapshot_age_ms;
            }),
        impl_->snapshots.end());
    
    size_t after = impl_->snapshots.size();
    return before - after;
}

uint64_t SnapshotManager::allocate_timestamp() {
    return impl_->current_timestamp.fetch_add(1);
}

uint64_t SnapshotManager::current_timestamp() const {
    return impl_->current_timestamp.load();
}

std::set<TransactionID> SnapshotManager::get_active_transactions() const {
    std::set<TransactionID> result;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& kv : impl_->txn_snapshots) {
        result.insert(kv.first);
    }
    return result;
}

SnapshotManager::Statistics SnapshotManager::get_statistics() const {
    Statistics stats;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    
    stats.total_snapshots_created = impl_->total_created.load();
    stats.total_snapshots_active = impl_->snapshots.size();
    stats.current_timestamp_value = impl_->current_timestamp.load();
    stats.active_transaction_count = impl_->txn_snapshots.size();
    
    if (!impl_->snapshots.empty()) {
        stats.oldest_snapshot_timestamp = impl_->snapshots.front()->timestamp();
        stats.newest_snapshot_timestamp = impl_->snapshots.back()->timestamp();
    }
    
    return stats;
}

// ========== SnapshotUtils 实现 ==========

bool SnapshotUtils::are_snapshots_compatible(
    const Snapshot& snapshot1,
    const Snapshot& snapshot2
) {
    // 简化实现：可见性区间有交集即认为兼容
    uint64_t h1 = snapshot1.get_visible_horizon();
    uint64_t h2 = snapshot2.get_visible_horizon();
    return !(h1 < h2 ? snapshot1.timestamp() < h2 : snapshot2.timestamp() < h1);
}

Snapshot::Ptr SnapshotUtils::merge_snapshots(
    const Snapshot& snapshot1,
    const Snapshot& snapshot2
) {
    // 简化实现：取较新的时间戳和活跃事务并集
    uint64_t ts = std::max(snapshot1.timestamp(), snapshot2.timestamp());
    std::set<TransactionID> active = snapshot1.active_transactions();
    active.insert(snapshot2.active_transactions().begin(),
                  snapshot2.active_transactions().end());
    return Snapshot::create(SnapshotType::SNAPSHOT_ISOLATION, ts, std::move(active), 0);
}

Snapshot::Ptr SnapshotUtils::create_consistent_snapshot() {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    uint64_t ts = static_cast<uint64_t>(now_ms);
    return Snapshot::create(SnapshotType::CONSISTENT_READ, ts, {}, 0);
}

bool SnapshotUtils::validate_snapshot(const Snapshot& snapshot) {
    // 简单校验：时间戳和ID非零即可
    return snapshot.id() != 0 && snapshot.timestamp() != 0;
}

// ========== 工厂函数 ==========

std::unique_ptr<SnapshotManager> create_snapshot_manager() {
    return std::make_unique<SnapshotManager>();
}

std::unique_ptr<SnapshotManager> create_high_performance_snapshot_manager() {
    // 当前实现与普通管理器相同，后续可替换为无锁结构
    return std::make_unique<SnapshotManager>();
}

} // namespace transaction
} // namespace mementodb


