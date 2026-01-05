// File: src/transaction/src/LockTableGraphBuilder.hpp
// 基于 LockTable 的等待图构建器实现
#pragma once

#include "DeadlockDetector.hpp"
#include "LockTable.hpp"
#include "TransactionContext.hpp"
#include <unordered_map>
#include <memory>
#include <mutex>

namespace mementodb {
namespace transaction {

// 前向声明
class TransactionManager;

/**
 * LockTableGraphBuilder - 基于 LockTable 的等待图构建器
 */
class LockTableGraphBuilder : public WaitForGraphBuilder {
public:
    /**
     * 构造函数
     * @param lock_table 锁表指针（不拥有所有权）
     * @param transaction_manager 事务管理器指针（用于获取事务信息）
     */
    LockTableGraphBuilder(
        LockTable* lock_table,
        TransactionManager* transaction_manager = nullptr);
    
    ~LockTableGraphBuilder() = default;
    
    // 实现 WaitForGraphBuilder 接口
    std::vector<WaitForEdge> get_current_edges() override;
    
    std::vector<WaitForEdge> get_edges_for_transaction(TransactionID tid) override;
    
    std::vector<TransactionID> get_active_transactions() override;
    
    TransactionInfo get_transaction_info(TransactionID tid) override;
    
    /**
     * 设置事务管理器（用于获取事务信息）
     */
    void set_transaction_manager(TransactionManager* manager) {
        transaction_manager_ = manager;
    }
    
private:
    LockTable* lock_table_;
    TransactionManager* transaction_manager_;
    
    /**
     * 从 LockTable 构建等待边
     */
    std::vector<WaitForEdge> build_edges_from_lock_table();
    
    /**
     * 获取资源的字符串表示（用于 void* 转换）
     */
    static void* string_to_resource(const std::string& key);
    static std::string resource_to_string(void* resource);
};

} // namespace transaction
} // namespace mementodb

