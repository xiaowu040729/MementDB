// File: src/transaction/include/IsolationLevel.hpp
// 隔离级别定义
#pragma once

namespace mementodb {
namespace transaction {

/**
 * 隔离级别枚举
 * 定义事务的隔离级别，控制并发访问时的可见性
 */
enum class IsolationLevel {
    READ_UNCOMMITTED = 0,  // 读未提交（最低隔离级别）
    READ_COMMITTED = 1,     // 读已提交
    REPEATABLE_READ = 2,    // 可重复读
    SERIALIZABLE = 3        // 可串行化（最高隔离级别）
};

/**
 * 隔离级别名称
 */
inline const char* IsolationLevelToString(IsolationLevel level) {
    switch (level) {
        case IsolationLevel::READ_UNCOMMITTED:
            return "READ_UNCOMMITTED";
        case IsolationLevel::READ_COMMITTED:
            return "READ_COMMITTED";
        case IsolationLevel::REPEATABLE_READ:
            return "REPEATABLE_READ";
        case IsolationLevel::SERIALIZABLE:
            return "SERIALIZABLE";
        default:
            return "UNKNOWN";
    }
}

} // namespace transaction
} // namespace mementodb

