// File: src/transaction/include/Transaction.hpp
// 事务接口定义
#pragma once

#include "IsolationLevel.hpp"
#include <cstdint>
#include <string>
#include <memory>

namespace mementodb {
namespace transaction {

// 前向声明
class TransactionContext;
class TransactionManager;

/**
 * TransactionID - 事务 ID 类型
 */
using TransactionID = uint64_t;

/**
 * Transaction - 事务接口
 * 
 * 提供事务的基本操作接口：
 * - 开始/提交/回滚
 * - 状态查询
 * - 隔离级别管理
 */
class Transaction {
public:
    virtual ~Transaction() = default;
    
    /**
     * get_transaction_id - 获取事务 ID
     */
    virtual TransactionID get_transaction_id() const = 0;
    
    /**
     * get_isolation_level - 获取隔离级别
     */
    virtual IsolationLevel get_isolation_level() const = 0;
    
    /**
     * commit - 提交事务
     * @return 是否成功
     */
    virtual bool commit() = 0;
    
    /**
     * rollback - 回滚事务
     * @return 是否成功
     */
    virtual bool rollback() = 0;
    
    /**
     * is_active - 事务是否活跃
     */
    virtual bool is_active() const = 0;
    
    /**
     * is_committed - 事务是否已提交
     */
    virtual bool is_committed() const = 0;
    
    /**
     * is_aborted - 事务是否已中止
     */
    virtual bool is_aborted() const = 0;
};

} // namespace transaction
} // namespace mementodb

