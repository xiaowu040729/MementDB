// File: src/transaction/src/LockTableGraphBuilder.cpp
// 基于 LockTable 的等待图构建器实现
#include "LockTableGraphBuilder.hpp"
#include "LockTable.hpp"
#include "TransactionManager.hpp"
#include "TransactionContext.hpp"
#include <algorithm>
#include <chrono>

namespace mementodb {
namespace transaction {

LockTableGraphBuilder::LockTableGraphBuilder(
    LockTable* lock_table,
    TransactionManager* transaction_manager)
    : lock_table_(lock_table)
    , transaction_manager_(transaction_manager)
{
}

std::vector<WaitForEdge> LockTableGraphBuilder::get_current_edges() {
    return build_edges_from_lock_table();
}

std::vector<WaitForEdge> LockTableGraphBuilder::get_edges_for_transaction(TransactionID tid) {
    auto all_edges = build_edges_from_lock_table();
    std::vector<WaitForEdge> result;
    
    for (const auto& edge : all_edges) {
        if (edge.waiter == tid || edge.holder == tid) {
            result.push_back(edge);
        }
    }
    
    return result;
}

std::vector<TransactionID> LockTableGraphBuilder::get_active_transactions() {
    if (!lock_table_) {
        return {};
    }
    
    std::unordered_set<TransactionID> transactions;
    
    // 从所有键中收集事务
    auto keys = lock_table_->get_all_keys();
    for (const auto& key : keys) {
        auto holders = lock_table_->get_holders(key);
        auto waiters = lock_table_->get_waiting_transactions(key);
        
        for (TransactionID tid : holders) {
            transactions.insert(tid);
        }
        for (TransactionID tid : waiters) {
            transactions.insert(tid);
        }
    }
    
    return std::vector<TransactionID>(transactions.begin(), transactions.end());
}

WaitForGraphBuilder::TransactionInfo LockTableGraphBuilder::get_transaction_info(TransactionID tid) {
    TransactionInfo info;
    info.start_time = 0;
    info.modifications_count = 0;
    info.locks_held = 0;
    
    if (transaction_manager_) {
        auto ctx = transaction_manager_->get_transaction(tid);
        if (ctx) {
            info.start_time = ctx->get_start_timestamp();
            info.modifications_count = ctx->get_write_set().size();
            info.locks_held = ctx->get_lock_references().size();
        }
    }
    
    return info;
}

std::vector<WaitForEdge> LockTableGraphBuilder::build_edges_from_lock_table() {
    if (!lock_table_) {
        return {};
    }
    
    std::vector<WaitForEdge> edges;
    
    // 获取所有键
    auto keys = lock_table_->get_all_keys();
    auto current_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    
    for (const auto& key : keys) {
        // 获取等待该键的事务
        auto waiters = lock_table_->get_waiting_transactions(key);
        // 获取持有该键的事务
        auto holders = lock_table_->get_holders(key);
        
        // 为每个等待者创建边
        for (TransactionID waiter : waiters) {
            for (TransactionID holder : holders) {
                void* resource = string_to_resource(key);
                edges.emplace_back(waiter, holder, resource, current_time);
            }
        }
    }
    
    return edges;
}

void* LockTableGraphBuilder::string_to_resource(const std::string& key) {
    // 将字符串转换为 void*（用于通用资源指针）
    // 注意：这只是临时方案，实际应该使用更安全的方式
    static std::unordered_map<std::string, void*> resource_map;
    static std::mutex map_mutex;
    
    std::lock_guard<std::mutex> lock(map_mutex);
    auto it = resource_map.find(key);
    if (it != resource_map.end()) {
        return it->second;
    }
    
    // 创建新的资源指针（使用字符串的地址）
    void* resource = const_cast<void*>(static_cast<const void*>(key.c_str()));
    resource_map[key] = resource;
    return resource;
}

std::string LockTableGraphBuilder::resource_to_string(void* resource) {
    // 将 void* 转换回字符串
    // 注意：这只是临时方案
    return static_cast<const char*>(resource);
}

} // namespace transaction
} // namespace mementodb

