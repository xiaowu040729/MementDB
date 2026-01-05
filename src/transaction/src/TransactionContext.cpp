// File: src/transaction/src/TransactionContext.cpp
// 事务上下文实现（增强版）

#include "TransactionContext.hpp"
#include "../../utils/LoggingSystem/LogMacros.hpp"
#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace mementodb {
namespace transaction {

// ========== TransactionKey 实现 ==========

bool TransactionKey::operator==(const TransactionKey& other) const {
    return type == other.type &&
           table_id == other.table_id &&
           index_id == other.index_id &&
           data == other.data &&
           components == other.components;
}

bool TransactionKey::operator<(const TransactionKey& other) const {
    if (table_id != other.table_id) return table_id < other.table_id;
    if (index_id != other.index_id) return index_id < other.index_id;
    if (type != other.type) return static_cast<uint8_t>(type) <
                                  static_cast<uint8_t>(other.type);
    if (data != other.data) return data < other.data;
    return components < other.components;
}

size_t TransactionKey::Hash::operator()(const TransactionKey& key) const {
    std::hash<uint64_t> h64;
    std::hash<int> hi;
    size_t hash = h64(key.table_id) ^ (h64(key.index_id) << 1);
    hash ^= static_cast<size_t>(static_cast<uint8_t>(key.type)) + 0x9e3779b9 +
            (hash << 6) + (hash >> 2);
    for (char c : key.data) {
        hash ^= static_cast<size_t>(static_cast<unsigned char>(c)) +
                0x9e3779b9 + (hash << 6) + (hash >> 2);
    }
    for (const auto& comp : key.components) {
        hash ^= Hash{}(comp) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    }
    return hash;
}

// ========== TransactionContext 实现 ==========

TransactionContext::Ptr TransactionContext::create(
    TransactionID tid,
    IsolationLevel isolation_level,
    TransactionPriority priority
) {
    return std::shared_ptr<TransactionContext>(
        new TransactionContext(tid, isolation_level, priority));
}

TransactionContext::TransactionContext(
    TransactionID tid,
    IsolationLevel isolation_level,
    TransactionPriority priority
)
    : transaction_id_(tid)
    , isolation_level_(isolation_level)
    , priority_(priority)
    , state_(TransactionState::ACTIVE) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    start_timestamp_ = static_cast<uint64_t>(now_ms);
}

TransactionContext::~TransactionContext() = default;

bool TransactionContext::is_active() const {
    TransactionState s = state_.load();
    return s == TransactionState::ACTIVE ||
           s == TransactionState::PREPARING ||
           s == TransactionState::PREPARED ||
           s == TransactionState::COMMITTING;
}

bool TransactionContext::is_committed() const {
    return state_.load() == TransactionState::COMMITTED;
}

bool TransactionContext::is_aborted() const {
    TransactionState s = state_.load();
    return s == TransactionState::ABORTED ||
           s == TransactionState::ROLLBACK_ONLY;
}

bool TransactionContext::is_prepared() const {
    return state_.load() == TransactionState::PREPARED;
}

bool TransactionContext::is_read_only() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return write_set_.empty();
}

bool TransactionContext::transition_state(TransactionState new_state) {
    TransactionState expected = state_.load();
    // 简化状态机：只禁止从终止状态回到非终止
    if (expected == TransactionState::COMMITTED ||
        expected == TransactionState::ABORTED) {
        return false;
    }
    state_.store(new_state);
    return true;
}

bool TransactionContext::force_state(TransactionState new_state) {
    state_.store(new_state);
    return true;
}

void TransactionContext::set_commit_timestamp(uint64_t timestamp) {
    commit_timestamp_ = timestamp;
}

void TransactionContext::set_prepare_timestamp(uint64_t timestamp) {
    prepare_timestamp_ = timestamp;
}

uint64_t TransactionContext::get_duration_ms() const {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    uint64_t now = static_cast<uint64_t>(now_ms);
    return now - start_timestamp_;
}

uint64_t TransactionContext::get_wait_time_ms() const {
    if (!waiting_.load()) {
        return 0;
    }
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t now = static_cast<uint64_t>(now_ms);
    auto start = std::chrono::duration_cast<std::chrono::milliseconds>(
        wait_start_.time_since_epoch()).count();
    return now - static_cast<uint64_t>(start);
}

// ========== 快照管理 ==========

Snapshot::Ptr TransactionContext::get_snapshot() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return snapshot_;
}

void TransactionContext::set_snapshot(Snapshot::Ptr snapshot) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    snapshot_ = std::move(snapshot);
}

void TransactionContext::create_statement_snapshot() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (!snapshot_) {
        return;
    }
    // 对于读已提交，每条语句可以使用新的 READ_COMMITTED 快照
    auto stmt_snapshot = Snapshot::create(
        SnapshotType::READ_COMMITTED,
        snapshot_->timestamp(),
        snapshot_->active_transactions(),
        snapshot_->creator_transaction().value_or(transaction_id_));
    statement_snapshots_.push_back(std::move(stmt_snapshot));
}

// ========== 读/写集合管理 ==========

void TransactionContext::add_read_key(
    const TransactionKey& key,
    uint64_t /*timestamp*/
) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    read_set_.push_back(key);
    ++read_count_;
}

void TransactionContext::add_write_key(
    const TransactionKey& key,
    const std::vector<char>* old_value,
    const std::vector<char>* new_value
) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    write_set_.push_back(key);
    if (old_value) {
        old_values_[key] = *old_value;
    }
    if (new_value) {
        new_values_[key] = *new_value;
    }
    ++write_count_;
}

std::optional<std::vector<char>> TransactionContext::get_old_value(
    const TransactionKey& key
) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = old_values_.find(key);
    if (it == old_values_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<std::vector<char>> TransactionContext::get_new_value(
    const TransactionKey& key
) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = new_values_.find(key);
    if (it == new_values_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void TransactionContext::clear_read_write_sets() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    read_set_.clear();
    write_set_.clear();
    old_values_.clear();
    new_values_.clear();
}

// ========== 锁管理 ==========

void TransactionContext::add_lock_reference(
    void* lock_object,
    const std::string& resource_id,
    LockMode mode
) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    LockReference ref;
    ref.lock_object = lock_object;
    ref.resource_id = resource_id;
    ref.mode = mode;
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    ref.acquire_time = static_cast<uint64_t>(now_ms);
    lock_references_.push_back(std::move(ref));
    ++lock_acquisitions_;
}

bool TransactionContext::remove_lock_reference(void* lock_object) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = std::remove_if(
        lock_references_.begin(),
        lock_references_.end(),
        [lock_object](const LockReference& ref) {
            return ref.lock_object == lock_object;
        });
    if (it == lock_references_.end()) {
        return false;
    }
    lock_references_.erase(it, lock_references_.end());
    return true;
}

bool TransactionContext::holds_lock(
    const std::string& resource_id,
    LockMode mode
) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (const auto& ref : lock_references_) {
        if (ref.resource_id == resource_id && ref.mode == mode) {
            return true;
        }
    }
    return false;
}

void TransactionContext::add_prepared_lsn(uint64_t lsn) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    prepared_lsns_.push_back(lsn);
}

// ========== 保存点管理 ==========

bool TransactionContext::create_savepoint(const std::string& name) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    Savepoint sp;
    sp.name = name;
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    sp.create_time = static_cast<uint64_t>(now_ms);
    sp.read_set_size = read_set_.size();
    sp.write_set_size = write_set_.size();
    sp.savepoint_lsn = last_lsn_;
    savepoints_.push_back(sp);
    ++savepoints_created_;
    return true;
}

bool TransactionContext::rollback_to_savepoint(const std::string& name) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = std::find_if(
        savepoints_.rbegin(), savepoints_.rend(),
        [&name](const Savepoint& sp) { return sp.name == name; });
    if (it == savepoints_.rend()) {
        return false;
    }
    // 回滚读写集合到保存点大小
    read_set_.resize(it->read_set_size);
    write_set_.resize(it->write_set_size);
    ++savepoints_rolled_back_;
    return true;
}

bool TransactionContext::release_savepoint(const std::string& name) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = std::remove_if(
        savepoints_.begin(),
        savepoints_.end(),
        [&name](const Savepoint& sp) { return sp.name == name; });
    if (it == savepoints_.end()) {
        return false;
    }
    savepoints_.erase(it, savepoints_.end());
    return true;
}

// ========== 等待与超时 ==========

void TransactionContext::start_waiting() {
    waiting_.store(true);
    wait_start_ = std::chrono::steady_clock::now();
}

void TransactionContext::stop_waiting() {
    if (waiting_.load()) {
        uint64_t waited = get_wait_time_ms();
        waiting_.store(false);
        if (waited > 0) {
            // 累加到总等待时间
            // 这里简单通过锁统计结构更新
        }
    }
}

void TransactionContext::set_timeout(uint64_t timeout_ms) {
    timeout_ms_ = timeout_ms;
}

// ========== 错误和回滚信息 ==========

void TransactionContext::set_error(const std::string& error) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    error_message_ = error;
}

std::string TransactionContext::get_error() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return error_message_;
}

void TransactionContext::set_rollback_reason(const std::string& reason) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    rollback_reason_ = reason;
}

std::string TransactionContext::get_rollback_reason() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return rollback_reason_;
}

// ========== 统计 ==========

TransactionContext::Statistics TransactionContext::get_statistics() const {
    Statistics stats;
    stats.read_operations = read_count_.load();
    stats.write_operations = write_count_.load();
    stats.lock_waits = lock_waits_.load();
    stats.lock_acquisitions = lock_acquisitions_.load();
    stats.savepoints_created = savepoints_created_.load();
    stats.savepoints_rolled_back = savepoints_rolled_back_.load();
    stats.total_wait_time_ms = get_wait_time_ms();
    return stats;
}

void TransactionContext::increment_read_count() {
    ++read_count_;
}

void TransactionContext::increment_write_count() {
    ++write_count_;
}

void TransactionContext::increment_lock_waits() {
    ++lock_waits_;
}

// ========== 序列化（简化实现，仅核心字段）==========

std::vector<char> TransactionContext::serialize() const {
    // 简单占位实现：当前未在系统中使用，返回空
    return {};
}

TransactionContext::Ptr TransactionContext::deserialize(
    const std::vector<char>& /*data*/
) {
    // 简化实现：当前未在系统中使用
    return nullptr;
}

// ========== TransactionContextManager 实现 ==========

class TransactionContextManager::Impl {
public:
    mutable std::shared_mutex mutex;
    std::unordered_map<TransactionID, TransactionContext::Ptr> contexts;
    
    // 统计
    std::atomic<size_t> total_created{0};
    std::atomic<size_t> committed{0};
    std::atomic<size_t> aborted{0};
    std::atomic<size_t> timed_out{0};
    std::atomic<size_t> suspended{0};
};

TransactionContextManager::TransactionContextManager()
    : impl_(std::make_unique<Impl>()) {}

TransactionContextManager::~TransactionContextManager() = default;

TransactionContext::Ptr TransactionContextManager::create_context(
    TransactionID tid,
    IsolationLevel isolation_level,
    TransactionPriority priority
) {
    auto ctx = TransactionContext::create(tid, isolation_level, priority);
    {
        std::unique_lock<std::shared_mutex> lock(impl_->mutex);
        impl_->contexts[tid] = ctx;
        impl_->total_created.fetch_add(1);
    }
    return ctx;
}

TransactionContext::Ptr TransactionContextManager::get_context(
    TransactionID tid
) const {
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    auto it = impl_->contexts.find(tid);
    if (it == impl_->contexts.end()) {
        return nullptr;
    }
    return it->second;
}

bool TransactionContextManager::destroy_context(TransactionID tid) {
    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    return impl_->contexts.erase(tid) > 0;
}

std::vector<TransactionContext::Ptr>
TransactionContextManager::get_active_contexts() const {
    std::vector<TransactionContext::Ptr> result;
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    for (const auto& [tid, ctx] : impl_->contexts) {
        (void)tid;
        if (ctx && ctx->is_active()) {
            result.push_back(ctx);
        }
    }
    return result;
}

std::vector<TransactionContext::Ptr>
TransactionContextManager::get_contexts_by_state(
    TransactionState state
) const {
    std::vector<TransactionContext::Ptr> result;
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    for (const auto& [tid, ctx] : impl_->contexts) {
        (void)tid;
        if (ctx && ctx->get_state() == state) {
            result.push_back(ctx);
        }
    }
    return result;
}

TransactionContext::Ptr TransactionContextManager::get_oldest_context() const {
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    TransactionContext::Ptr oldest;
    uint64_t oldest_ts = std::numeric_limits<uint64_t>::max();
    for (const auto& [tid, ctx] : impl_->contexts) {
        (void)tid;
        if (ctx && ctx->get_start_timestamp() < oldest_ts) {
            oldest_ts = ctx->get_start_timestamp();
            oldest = ctx;
        }
    }
    return oldest;
}

bool TransactionContextManager::exists(TransactionID tid) const {
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    return impl_->contexts.find(tid) != impl_->contexts.end();
}

bool TransactionContextManager::force_abort(
    TransactionID tid,
    const std::string& reason
) {
    auto ctx = get_context(tid);
    if (!ctx) {
        return false;
    }
    ctx->set_rollback_reason(reason);
    ctx->force_state(TransactionState::ABORTED);
    impl_->aborted.fetch_add(1);
    return true;
}

bool TransactionContextManager::mark_timeout(TransactionID tid) {
    auto ctx = get_context(tid);
    if (!ctx) {
        return false;
    }
    ctx->force_state(TransactionState::TIMED_OUT);
    impl_->timed_out.fetch_add(1);
    return true;
}

TransactionContextManager::ManagerStatistics
TransactionContextManager::get_statistics() const {
    ManagerStatistics stats;
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    stats.total_transactions_created = impl_->total_created.load();
    stats.active_transactions = 0;
    stats.committed_transactions = impl_->committed.load();
    stats.aborted_transactions = impl_->aborted.load();
    stats.timed_out_transactions = impl_->timed_out.load();
    stats.suspended_transactions = impl_->suspended.load();
    for (const auto& [tid, ctx] : impl_->contexts) {
        (void)tid;
        if (ctx && ctx->is_active()) {
            ++stats.active_transactions;
        }
    }
    return stats;
}

size_t TransactionContextManager::cleanup_completed_transactions() {
    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    size_t before = impl_->contexts.size();
    for (auto it = impl_->contexts.begin(); it != impl_->contexts.end();) {
        auto& ctx = it->second;
        if (ctx &&
            (ctx->is_committed() || ctx->is_aborted() ||
             ctx->get_state() == TransactionState::TIMED_OUT)) {
            it = impl_->contexts.erase(it);
        } else {
            ++it;
        }
    }
    size_t after = impl_->contexts.size();
    return before - after;
}

void TransactionContextManager::reset() {
    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    impl_->contexts.clear();
    impl_->total_created.store(0);
    impl_->committed.store(0);
    impl_->aborted.store(0);
    impl_->timed_out.store(0);
    impl_->suspended.store(0);
}

// ========== 工厂函数 ==========

std::unique_ptr<TransactionContextManager> create_transaction_context_manager() {
    return std::make_unique<TransactionContextManager>();
}

std::unique_ptr<TransactionContextManager>
create_high_performance_context_manager() {
    return std::make_unique<TransactionContextManager>();
}

} // namespace transaction
} // namespace mementodb


