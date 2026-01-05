// File: src/transaction/src/RecoveryManager.cpp
// 恢复管理实现

#include "RecoveryManager.hpp"
#include "../../utils/LoggingSystem/LogMacros.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>

namespace mementodb {
namespace transaction {

RecoveryManager::RecoveryManager(WAL* wal)
    : wal_(wal)
    , storage_accessor_(nullptr)
    , checkpoint_mgr_(nullptr)
    , default_protocol_name_("SimpleRedoUndo")
{
    reset_statistics();
    
    // 注册默认协议（如果协议已实现）
    // 注意：这些协议的实现可能还未完成，暂时注释掉以避免链接错误
    // register_protocol(std::make_unique<SimpleRedoUndoProtocol>());
    // register_protocol(std::make_unique<ARIESRecoveryProtocol>());
}

RecoveryManager::~RecoveryManager() {
    // 清理监控器
    std::lock_guard<std::mutex> lock(monitors_mutex_);
    monitors_.clear();
}

RecoveryResult RecoveryManager::perform_crash_recovery(const RecoveryConfig& config) {
    auto start_time = std::chrono::steady_clock::now();
    RecoveryResult result;
    result.type = config.type;
    result.start_lsn = 0;
    
    // 更新状态
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        current_status_ = RecoveryStatus();
        current_status_->is_recovering = true;
        current_status_->current_phase = RecoveryPhase::INITIALIZING;
        current_status_->start_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
    
    notify_monitors([&](RecoveryMonitor* m) {
        m->on_phase_started(RecoveryPhase::INITIALIZING);
    });
    
    try {
        // 分析阶段
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            current_status_->current_phase = RecoveryPhase::ANALYSIS;
        }
        notify_monitors([&](RecoveryMonitor* m) {
            m->on_phase_started(RecoveryPhase::ANALYSIS);
        });
        
        RecoveryInfo info = analyze_wal();
        result.stats.log_records_processed = info.total_log_records;
        result.transactions_recovered = info.uncommitted.size() + info.committed.size();
        
        notify_monitors([&](RecoveryMonitor* m) {
            m->on_phase_completed(RecoveryPhase::ANALYSIS);
        });
        
        // REDO阶段（简化实现，实际应该重放已提交的操作）
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            current_status_->current_phase = RecoveryPhase::REDO;
        }
        notify_monitors([&](RecoveryMonitor* m) {
            m->on_phase_started(RecoveryPhase::REDO);
        });
        
        // TODO: 实现真正的REDO逻辑
        result.stats.redo_operations = info.committed.size();
        
        notify_monitors([&](RecoveryMonitor* m) {
            m->on_phase_completed(RecoveryPhase::REDO);
        });
        
        // UNDO阶段（回滚未提交的事务）
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            current_status_->current_phase = RecoveryPhase::UNDO;
        }
        notify_monitors([&](RecoveryMonitor* m) {
            m->on_phase_started(RecoveryPhase::UNDO);
        });
        
        // TODO: 实现真正的UNDO逻辑
        result.stats.undo_operations = info.uncommitted.size();
        
        notify_monitors([&](RecoveryMonitor* m) {
            m->on_phase_completed(RecoveryPhase::UNDO);
        });
        
        // 清理阶段
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            current_status_->current_phase = RecoveryPhase::CLEANUP;
        }
        notify_monitors([&](RecoveryMonitor* m) {
            m->on_phase_started(RecoveryPhase::CLEANUP);
        });
        
        notify_monitors([&](RecoveryMonitor* m) {
            m->on_phase_completed(RecoveryPhase::CLEANUP);
        });
        
        result.success = true;
        result.final_phase = RecoveryPhase::COMPLETED;
        
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            current_status_->is_recovering = false;
            current_status_->current_phase = RecoveryPhase::COMPLETED;
        }
        
    } catch (const std::exception& e) {
        result.success = false;
        result.final_phase = RecoveryPhase::FAILED;
        result.error_message = e.what();
        
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            current_status_->is_recovering = false;
            current_status_->current_phase = RecoveryPhase::FAILED;
        }
        
        notify_monitors([&](RecoveryMonitor* m) {
            m->on_recovery_error(e.what(), result.end_lsn);
        });
    }
    
    auto end_time = std::chrono::steady_clock::now();
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time);
    result.end_lsn = wal_ ? wal_->get_current_lsn() : 0;
    
    // 更新统计
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.total_recoveries++;
        if (result.success) {
            stats_.successful_recoveries++;
        } else {
            stats_.failed_recoveries++;
        }
        stats_.total_recovery_time_ms += result.duration.count();
        stats_.average_recovery_time_ms = stats_.total_recoveries > 0 ?
            stats_.total_recovery_time_ms / stats_.total_recoveries : 0;
        if (result.transactions_recovered > stats_.max_transactions_recovered) {
            stats_.max_transactions_recovered = result.transactions_recovered;
        }
        if (result.pages_recovered > stats_.max_pages_recovered) {
            stats_.max_pages_recovered = result.pages_recovered;
        }
    }
    
    return result;
}

size_t RecoveryManager::recover_from_wal(
    RecoveryCallback redo_callback,
    RecoveryCallback undo_callback) {
    
    // 保持向后兼容：使用默认配置执行崩溃恢复
    RecoveryConfig config;
    RecoveryResult result = perform_crash_recovery(config);
    
    // TODO: 调用回调函数执行实际的恢复操作
    // 这里先返回恢复的事务数量
    return result.transactions_recovered;
}

bool RecoveryManager::recover_transaction(
    TransactionID tid,
    RecoveryCallback redo_callback,
    RecoveryCallback undo_callback) {
    
    if (!wal_) {
        return false;
    }
    
    // 检查事务是否已提交
    if (is_transaction_committed(tid)) {
        return redo_transaction(tid, redo_callback);
    } else {
        return undo_transaction(tid, undo_callback);
    }
}

bool RecoveryManager::rollback_transaction(uint64_t transaction_id) {
    // 简化实现：调用 undo_transaction
    return undo_transaction(transaction_id, nullptr);
}

std::vector<TransactionID> RecoveryManager::get_uncommitted_transactions() {
    RecoveryInfo info = analyze_wal();
    return info.uncommitted;
}

std::vector<TransactionID> RecoveryManager::get_committed_transactions() {
    RecoveryInfo info = analyze_wal();
    return info.committed;
}

bool RecoveryManager::redo_transaction(TransactionID tid, RecoveryCallback callback) {
    if (!wal_) {
        return false;
    }
    
    auto logs = get_transaction_logs(tid);
    if (logs.empty()) {
        return false;
    }
    
    // TODO: 解析日志并调用callback
    // 简化实现：只返回成功
    return true;
}

bool RecoveryManager::undo_transaction(TransactionID tid, RecoveryCallback callback) {
    if (!wal_) {
        return false;
    }
    
    auto logs = get_transaction_logs(tid);
    if (logs.empty()) {
        return false;
    }
    
    // TODO: 解析日志并调用callback进行回滚
    // 简化实现：只返回成功
    return true;
}

size_t RecoveryManager::scan_and_recover(
    uint64_t start_lsn,
    RecoveryCallback redo_callback,
    RecoveryCallback undo_callback) {
    
    if (!wal_) {
        return 0;
    }
    
    // TODO: 实现真正的扫描和恢复逻辑
    RecoveryInfo info = analyze_wal();
    return info.uncommitted.size() + info.committed.size();
}

RecoveryManager::RecoveryInfo RecoveryManager::analyze_wal() {
    RecoveryInfo info;
    
    if (!wal_) {
        return info;
    }
    
    auto scanner = wal_->create_scanner();
    if (!scanner || !scanner->open(0)) {
        return info;
    }
    
    std::unordered_set<TransactionID> seen_txns;
    std::unordered_map<TransactionID, bool> txn_status; // true=committed, false=uncommitted
    
    std::vector<char> record;
    while (scanner->next(record)) {
        if (record.size() < sizeof(LogHeader)) {
            continue;
        }
        
        LogHeader header;
        std::memcpy(reinterpret_cast<void*>(&header), record.data(), sizeof(LogHeader));
        
        info.total_log_records++;
        seen_txns.insert(header.transaction_id);
        
        if (header.type == LogType::COMMIT) {
            txn_status[header.transaction_id] = true;
        } else if (header.type == LogType::BEGIN) {
            if (txn_status.find(header.transaction_id) == txn_status.end()) {
                txn_status[header.transaction_id] = false;
            }
        }
    }
    
    info.total_transactions = seen_txns.size();
    
    for (const auto& [tid, committed] : txn_status) {
        if (committed) {
            info.committed.push_back(tid);
        } else {
            info.uncommitted.push_back(tid);
        }
    }
    
    return info;
}

bool RecoveryManager::is_transaction_committed(TransactionID tid) {
    if (!wal_) {
        return false;
    }
    
    auto scanner = wal_->create_scanner();
    if (!scanner || !scanner->open(0)) {
        return false;
    }
    
    std::vector<char> record;
    while (scanner->next(record)) {
        if (record.size() < sizeof(LogHeader)) {
            continue;
        }
        
        LogHeader header;
        std::memcpy(reinterpret_cast<void*>(&header), record.data(), sizeof(LogHeader));
        
        if (header.transaction_id == tid && header.type == LogType::COMMIT) {
            return true;
        }
    }
    
    return false;
}

std::vector<std::vector<char>> RecoveryManager::get_transaction_logs(TransactionID tid) {
    std::vector<std::vector<char>> logs;
    
    if (!wal_) {
        return logs;
    }
    
    auto scanner = wal_->create_scanner();
    if (!scanner || !scanner->open(0)) {
        return logs;
    }
    
    std::vector<char> record;
    while (scanner->next(record)) {
        if (record.size() < sizeof(LogHeader)) {
            continue;
        }
        
        LogHeader header;
        std::memcpy(reinterpret_cast<void*>(&header), record.data(), sizeof(LogHeader));
        
        if (header.transaction_id == tid) {
            logs.push_back(record);
        }
    }
    
    // 按LSN排序
    std::sort(logs.begin(), logs.end(), [](const std::vector<char>& a, const std::vector<char>& b) {
        if (a.size() < sizeof(LogHeader) || b.size() < sizeof(LogHeader)) {
            return false;
        }
        LogHeader ha, hb;
        std::memcpy(reinterpret_cast<void*>(&ha), a.data(), sizeof(LogHeader));
        std::memcpy(reinterpret_cast<void*>(&hb), b.data(), sizeof(LogHeader));
        return ha.lsn < hb.lsn;
    });
    
    return logs;
}

void RecoveryManager::add_monitor(std::shared_ptr<RecoveryMonitor> monitor) {
    std::lock_guard<std::mutex> lock(monitors_mutex_);
    monitors_.push_back(std::weak_ptr<RecoveryMonitor>(monitor));
}

void RecoveryManager::remove_monitor(std::shared_ptr<RecoveryMonitor> monitor) {
    std::lock_guard<std::mutex> lock(monitors_mutex_);
    monitors_.erase(
        std::remove_if(monitors_.begin(), monitors_.end(),
            [&monitor](const std::weak_ptr<RecoveryMonitor>& wp) {
                return wp.lock() == monitor;
            }),
        monitors_.end());
}

std::optional<RecoveryManager::RecoveryStatus> RecoveryManager::get_current_status() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return current_status_;
}

RecoveryManager::RecoveryStatistics RecoveryManager::get_statistics() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void RecoveryManager::reset_statistics() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = RecoveryStatistics();
}

uint64_t RecoveryManager::estimate_recovery_time(uint64_t start_lsn) const {
    // 简单估算：基于日志记录数量
    if (!wal_) {
        return 0;
    }
    
    uint64_t current_lsn = wal_->get_current_lsn();
    uint64_t log_count = current_lsn > start_lsn ? current_lsn - start_lsn : 0;
    
    // 假设每个日志记录需要1ms处理
    return log_count;
}

void RecoveryManager::notify_monitors(std::function<void(RecoveryMonitor*)> func) {
    std::lock_guard<std::mutex> lock(monitors_mutex_);
    
    // 清理失效的weak_ptr
    monitors_.erase(
        std::remove_if(monitors_.begin(), monitors_.end(),
            [](const std::weak_ptr<RecoveryMonitor>& wp) {
                return wp.expired();
            }),
        monitors_.end());
    
    // 通知所有监控器
    for (auto& wp : monitors_) {
        if (auto monitor = wp.lock()) {
            func(monitor.get());
        }
    }
}

} // namespace transaction
} // namespace mementodb

