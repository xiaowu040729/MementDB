// File: src/transaction/src/MVCCEngine.cpp
// MVCC实现

#include "MVCCEngine.hpp"
#include "TransactionContext.hpp"
#include "../../utils/LoggingSystem/LogMacros.hpp"
#include <algorithm>
#include <chrono>

namespace mementodb {
namespace transaction {

MVCCEngine::MVCCEngine(const MVCCConfig& config)
    : config_(config)
{
    // 使用当前时间（毫秒）作为初始时间戳
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    current_timestamp_.store(static_cast<uint64_t>(now));
    LOG_INFO("MVCCEngine", "MVCC引擎初始化完成");
}

MVCCEngine::~MVCCEngine() {
    LOG_INFO("MVCCEngine", "MVCC引擎析构");
}

Snapshot::Ptr MVCCEngine::create_snapshot(TransactionID tid) {
    uint64_t snapshot_timestamp = current_timestamp_.load();
    // 默认使用可重复读快照类型，并且当前实现不跟踪全局活跃事务集合
    return Snapshot::create(
        SnapshotType::REPEATABLE_READ,
        snapshot_timestamp,
        {},
        tid
    );
}

std::optional<std::vector<char>> MVCCEngine::read_version(
    const std::vector<char>& key,
    const Snapshot& snapshot
) {
    std::shared_lock<std::shared_mutex> lock(versions_mutex_);
    
    auto it = versions_.find(key);
    if (it == versions_.end()) {
        return std::nullopt;
    }
    
    // 查找可读的版本（基于 Version::is_visible 进行判断）
    auto version = find_readable_version(key, snapshot.timestamp());
    if (version.has_value()) {
        return version->value;
    }
    
    return std::nullopt;
}

bool MVCCEngine::write_version(
    TransactionID tid,
    const std::vector<char>& key,
    const std::vector<char>& value
) {
    uint64_t write_timestamp = current_timestamp_.fetch_add(1);
    
    {
        std::lock_guard<std::shared_mutex> lock(versions_mutex_);
        
        Version new_version;
        new_version.timestamp = write_timestamp;
        new_version.value = value;
        new_version.transaction_id = tid;
        new_version.committed = false;
        new_version.create_txid = tid;
        new_version.begin_timestamp = write_timestamp;
        
        versions_[key].push_back(new_version);
        
        // 限制版本数量
        if (versions_[key].size() > config_.max_version_count) {
            versions_[key].erase(versions_[key].begin());
        }
    }
    
    {
        std::lock_guard<std::mutex> lock(write_sets_mutex_);
        write_sets_[tid].push_back(key);
    }
    
    return true;
}

bool MVCCEngine::commit_version(TransactionID tid, uint64_t commit_timestamp) {
    std::lock_guard<std::mutex> write_lock(write_sets_mutex_);
    
    auto it = write_sets_.find(tid);
    if (it == write_sets_.end()) {
        return false;
    }
    
    std::lock_guard<std::shared_mutex> version_lock(versions_mutex_);
    
    // 标记所有写操作为已提交
    for (const auto& key : it->second) {
        auto key_it = versions_.find(key);
        if (key_it != versions_.end()) {
            for (auto& version : key_it->second) {
                if (version.transaction_id == tid && !version.committed) {
                    version.committed = true;
                    version.timestamp = commit_timestamp;
                }
            }
        }
    }
    
    write_sets_.erase(it);
    return true;
}

bool MVCCEngine::abort_version(TransactionID tid) {
    std::lock_guard<std::mutex> write_lock(write_sets_mutex_);
    
    auto it = write_sets_.find(tid);
    if (it == write_sets_.end()) {
        return false;
    }
    
    std::lock_guard<std::shared_mutex> version_lock(versions_mutex_);
    
    // 删除未提交的版本
    for (const auto& key : it->second) {
        auto key_it = versions_.find(key);
        if (key_it != versions_.end()) {
            auto& versions = key_it->second;
            versions.erase(
                std::remove_if(versions.begin(), versions.end(),
                    [tid](const Version& v) {
                        return v.transaction_id == tid && !v.committed;
                    }),
                versions.end()
            );
            
            if (versions.empty()) {
                versions_.erase(key_it);
            }
        }
    }
    
    write_sets_.erase(it);
    return true;
}

MVCCEngine::EngineStats MVCCEngine::get_stats() const {
    EngineStats stats;

    {
        std::shared_lock<std::shared_mutex> lock(versions_mutex_);
        stats.total_keys = versions_.size();

        uint64_t oldest_ts = std::numeric_limits<uint64_t>::max();
        uint64_t newest_ts = 0;

        for (const auto& [key, ver_list] : versions_) {
            (void)key;
            stats.total_versions += ver_list.size();

            for (const auto& v : ver_list) {
                // 近似统计内存：版本头 + value 大小
                stats.approximate_memory_bytes += sizeof(Version) + v.value.size();
                if (v.timestamp < oldest_ts) {
                    oldest_ts = v.timestamp;
                }
                if (v.timestamp > newest_ts) {
                    newest_ts = v.timestamp;
                }
            }
        }

        if (stats.total_versions > 0) {
            stats.oldest_version_timestamp = oldest_ts;
            stats.newest_version_timestamp = newest_ts;
        }
    }

    {
        std::lock_guard<std::mutex> lock(write_sets_mutex_);
        stats.active_transactions = write_sets_.size();
    }

    return stats;
}

MVCCEngine::GCStats MVCCEngine::get_gc_stats() const {
    std::lock_guard<std::mutex> lock(write_sets_mutex_);
    // gc_stats_ 只在 cleanup_old_versions 中更新，这里直接返回拷贝
    return gc_stats_;
}

void MVCCEngine::cleanup_old_versions(uint64_t current_timestamp) {
    uint64_t cutoff_timestamp = current_timestamp - config_.version_retention_time_ms;
    
    std::lock_guard<std::shared_mutex> lock(versions_mutex_);
    
    size_t removed_total = 0;
    size_t reclaimed_bytes = 0;

    for (auto it = versions_.begin(); it != versions_.end();) {
        size_t before = it->second.size();
        remove_old_versions(it->second, cutoff_timestamp, removed_total, reclaimed_bytes);
        
        if (it->second.empty()) {
            it = versions_.erase(it);
        } else {
            ++it;
        }
    }

    // 更新 GC 统计信息
    gc_stats_.collected_versions = removed_total;
    gc_stats_.memory_reclaimed = reclaimed_bytes;
    gc_stats_.total_versions = 0;
    for (const auto& [key, ver_list] : versions_) {
        (void)key;
        gc_stats_.total_versions += ver_list.size();
    }
    gc_stats_.last_collection_time = current_timestamp;
}

size_t MVCCEngine::get_version_count(const std::vector<char>& key) const {
    std::shared_lock<std::shared_mutex> lock(versions_mutex_);
    
    auto it = versions_.find(key);
    if (it == versions_.end()) {
        return 0;
    }
    
    return it->second.size();
}

std::optional<MVCCEngine::Version> MVCCEngine::find_readable_version(
    const std::vector<char>& key,
    uint64_t snapshot_timestamp
) const {
    auto it = versions_.find(key);
    if (it == versions_.end()) {
        return std::nullopt;
    }
    
    // 从最新版本开始查找，使用版本自身的可见性规则
    const auto& versions = it->second;
    for (auto rit = versions.rbegin(); rit != versions.rend(); ++rit) {
        if (rit->is_visible(snapshot_timestamp)) {
            return *rit;
        }
    }
    
    return std::nullopt;
}

void MVCCEngine::remove_old_versions(
    std::vector<Version>& versions,
    uint64_t cutoff_timestamp,
    size_t& removed_count,
    size_t& reclaimed_bytes
) {
    auto it = std::remove_if(
        versions.begin(), versions.end(),
        [cutoff_timestamp, &removed_count, &reclaimed_bytes](const Version& v) {
            if (v.committed && v.timestamp < cutoff_timestamp) {
                ++removed_count;
                reclaimed_bytes += sizeof(Version) + v.value.size();
                return true;
            }
            return false;
        });
    versions.erase(it, versions.end());
}

} // namespace transaction
} // namespace mementodb

