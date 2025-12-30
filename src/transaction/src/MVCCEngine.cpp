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

std::unique_ptr<Snapshot> MVCCEngine::create_snapshot(TransactionID tid) {
    uint64_t snapshot_timestamp = current_timestamp_.load();
    return std::make_unique<Snapshot>(tid, snapshot_timestamp);
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
    
    // 查找可读的版本（时间戳 <= snapshot.timestamp 且已提交）
    auto version = find_readable_version(key, snapshot.get_timestamp());
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

void MVCCEngine::cleanup_old_versions(uint64_t current_timestamp) {
    uint64_t cutoff_timestamp = current_timestamp - config_.version_retention_time_ms;
    
    std::lock_guard<std::shared_mutex> lock(versions_mutex_);
    
    for (auto it = versions_.begin(); it != versions_.end();) {
        remove_old_versions(it->second, cutoff_timestamp);
        
        if (it->second.empty()) {
            it = versions_.erase(it);
        } else {
            ++it;
        }
    }
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
    
    // 从最新版本开始查找
    const auto& versions = it->second;
    for (auto rit = versions.rbegin(); rit != versions.rend(); ++rit) {
        if (rit->committed && rit->timestamp <= snapshot_timestamp) {
            return *rit;
        }
    }
    
    return std::nullopt;
}

void MVCCEngine::remove_old_versions(
    std::vector<Version>& versions,
    uint64_t cutoff_timestamp
) {
    versions.erase(
        std::remove_if(versions.begin(), versions.end(),
            [cutoff_timestamp](const Version& v) {
                return v.committed && v.timestamp < cutoff_timestamp;
            }),
        versions.end()
    );
}

} // namespace transaction
} // namespace mementodb

