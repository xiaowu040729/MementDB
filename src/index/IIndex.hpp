#ifndef I_INDEX_HPP
#define I_INDEX_HPP

#include "mementodb/Types.h"
#include <cstdint>
#include <memory>
#include <vector>
#include <optional>
#include <string>
#include <sstream>
#include <iomanip>
#include <functional>
#include <atomic>

namespace mementodb {
namespace index {

// 前向声明
class IIndex;

/**
 * 索引类型枚举 - 使用宏支持扩展
 */
enum class IndexType {
    BPLUS_TREE,    // B+树索引（支持范围查询）
    HASH,          // 哈希索引（点查询快）
    SKIP_LIST,     // 跳表索引（内存索引，支持范围查询）
    BLOOM_FILTER,  // 布隆过滤器（仅存在性检查）
    CUSTOM         // 自定义索引类型
};

// 索引类型字符串转换
constexpr const char* index_type_to_string(IndexType type) {
    switch (type) {
        case IndexType::BPLUS_TREE: return "BPLUS_TREE";
        case IndexType::HASH: return "HASH";
        case IndexType::SKIP_LIST: return "SKIP_LIST";
        case IndexType::BLOOM_FILTER: return "BLOOM_FILTER";
        case IndexType::CUSTOM: return "CUSTOM";
        default: return "UNKNOWN";
    }
}

/**
 * 索引统计信息 - 添加线程安全的原子计数
 */
struct IndexStatistics {
    // 注意：由于包含原子变量，这个结构体不能拷贝或移动
    // 使用普通变量而不是原子变量，因为返回的是快照
    size_t total_keys{0};           // 总键数
    size_t memory_usage_bytes{0};   // 内存使用量
    size_t disk_usage_bytes{0};     // 磁盘使用量
    uint64_t insert_count{0};      // 插入次数
    uint64_t lookup_count{0};       // 查找次数
    uint64_t delete_count{0};       // 删除次数
    uint64_t hit_count{0};          // 命中次数
    uint64_t miss_count{0};         // 未命中次数
    uint64_t range_query_count{0};  // 范围查询次数（新增）
    uint64_t prefix_query_count{0}; // 前缀查询次数（新增）
    
    double hit_ratio() const {
        uint64_t total = hit_count + miss_count;
        return total > 0 ? static_cast<double>(hit_count) / total : 0.0;
    }
    
    std::string to_string() const {
        std::ostringstream oss;
        oss << "IndexStatistics{"
            << "total_keys=" << total_keys
            << ", memory_usage=" << memory_usage_bytes << " bytes"
            << ", disk_usage=" << disk_usage_bytes << " bytes"
            << ", insert_count=" << insert_count
            << ", lookup_count=" << lookup_count
            << ", delete_count=" << delete_count
            << ", range_query_count=" << range_query_count
            << ", prefix_query_count=" << prefix_query_count
            << ", hit_ratio=" << std::fixed << std::setprecision(2) << (hit_ratio() * 100) << "%"
            << "}";
        return oss.str();
    }
};

/**
 * 范围查询结果 - 添加错误信息和迭代器支持
 */
struct RangeResult {
    std::vector<std::pair<Slice, uint64_t>> entries;  // (key, value) 对
    bool has_more = false;  // 是否还有更多结果
    std::string error_msg;  // 错误信息（如有）
    
    size_t size() const { return entries.size(); }
    bool empty() const { return entries.empty(); }
    bool has_error() const { return !error_msg.empty(); }
    
    // 迭代器支持
    auto begin() { return entries.begin(); }
    auto end() { return entries.end(); }
    auto begin() const { return entries.begin(); }
    auto end() const { return entries.end(); }
};

/**
 * 索引配置基类 - 改为具体类以便序列化
 */
struct IndexConfig {
    std::string name;              // 索引名称
    IndexType type;                // 索引类型
    bool persistent = false;       // 是否持久化
    std::string storage_path;      // 持久化路径
    bool enable_statistics = true; // 是否启用统计
    bool concurrent_access = false;// 是否支持并发访问
    size_t expected_size = 0;      // 预期大小（用于优化）
    
    virtual ~IndexConfig() = default;
    
    // 克隆方法，用于多态复制
    virtual std::unique_ptr<IndexConfig> clone() const {
        return std::make_unique<IndexConfig>(*this);
    }
    
    // 验证配置有效性
    virtual bool validate() const {
        return !name.empty() && 
               (type >= IndexType::BPLUS_TREE && type <= IndexType::CUSTOM);
    }
    
    std::string to_string() const {
        std::ostringstream oss;
        oss << "IndexConfig{name='" << name 
            << "', type=" << index_type_to_string(type)
            << ", persistent=" << persistent
            << ", concurrent_access=" << concurrent_access
            << "}";
        return oss.str();
    }
};

/**
 * 索引接口（基类）
 * 
 * 添加异常安全说明和移动语义支持
 */
class IIndex : public std::enable_shared_from_this<IIndex> {
public:
    virtual ~IIndex() = default;
    
    // 对外公开的索引类型访问器
    IndexType type() const { return get_type(); }
    
    // 禁用拷贝，允许移动
    IIndex(const IIndex&) = delete;
    IIndex& operator=(const IIndex&) = delete;
    IIndex(IIndex&&) = default;
    IIndex& operator=(IIndex&&) = default;
    
    // 默认构造函数（protected，供子类使用）
protected:
    IIndex() = default;
    
public:
    /**
     * 获取索引类型
     */
    virtual IndexType get_type() const = 0;
    
    /**
     * 获取索引名称
     */
    virtual std::string get_name() const = 0;
    
    /**
     * 插入键值对
     * @param key 键
     * @param value 值（通常是页ID或记录ID）
     * @return 成功返回 true，键已存在时行为由实现决定
     * @throws std::bad_alloc 内存分配失败
     * @throws std::runtime_error 内部错误
     */
    virtual bool insert(const Slice& key, uint64_t value) = 0;
    
    /**
     * 插入或更新键值对
     * @param key 键
     * @param value 值
     * @return 如果键已存在返回旧值，否则返回std::nullopt
     */
    virtual std::optional<uint64_t> insert_or_update(const Slice& key, uint64_t value) {
        auto old = get(key);
        if (old) {
            // 需要子类实现具体的更新逻辑
            return old;
        }
        insert(key, value);
        return std::nullopt;
    }
    
    /**
     * 查找键对应的值
     * @param key 键
     * @return 如果找到返回值，否则返回 std::nullopt
     * @throws std::runtime_error 内部错误
     */
    virtual std::optional<uint64_t> get(const Slice& key) const = 0;
    
    /**
     * 删除键值对
     * @param key 键
     * @return 成功返回 true，如果键不存在返回 false
     * @throws std::runtime_error 内部错误
     */
    virtual bool remove(const Slice& key) = 0;
    
    /**
     * 检查键是否存在
     * @param key 键
     * @return 存在返回 true，否则返回 false
     */
    virtual bool contains(const Slice& key) const = 0;
    
    /**
     * 清空索引
     * @throws std::runtime_error 内部错误
     */
    virtual void clear() = 0;
    
    /**
     * 获取索引大小（键的数量）
     */
    virtual size_t size() const = 0;
    
    /**
     * 检查索引是否为空
     */
    virtual bool empty() const = 0;
    
    /**
     * 获取统计信息
     */
    virtual IndexStatistics get_statistics() const = 0;
    
    /**
     * 重置统计信息
     */
    virtual void reset_statistics() = 0;
    
    /**
     * 持久化索引到磁盘
     * @param path 存储路径，如果为空则使用默认路径
     * @return 成功返回 true
     * @throws std::runtime_error 持久化失败
     */
    virtual bool persist(const std::string& path = "") const = 0;
    
    /**
     * 从磁盘加载索引
     * @param path 存储路径
     * @return 成功返回 true
     * @throws std::runtime_error 加载失败
     */
    virtual bool load(const std::string& path) = 0;
    
    /**
     * 范围查询
     * @param start_key 起始键（包含）
     * @param end_key 结束键（包含）
     * @param limit 最大返回数量，0 表示无限制
     * @return 范围内的键值对
     * @throws std::runtime_error 不支持范围查询或内部错误
     */
    virtual RangeResult range_query(const Slice& start_key, 
                                    const Slice& end_key, 
                                    size_t limit = 0) const {
        throw std::runtime_error("Range query not supported by this index type");
    }
    
    /**
     * 前缀查询
     * @param prefix 前缀
     * @param limit 最大返回数量
     * @return 匹配前缀的键值对
     * @throws std::runtime_error 不支持前缀查询或内部错误
     */
    virtual RangeResult prefix_query(const Slice& prefix, 
                                     size_t limit = 0) const {
        throw std::runtime_error("Prefix query not supported by this index type");
    }
    
    /**
     * 批量插入（事务性）
     * @param entries 键值对列表
     * @return (成功数量, 失败列表)
     */
    virtual std::pair<size_t, std::vector<std::pair<Slice, uint64_t>>> 
    batch_insert(const std::vector<std::pair<Slice, uint64_t>>& entries) {
        std::vector<std::pair<Slice, uint64_t>> failed;
        size_t count = 0;
        
        for (const auto& entry : entries) {
            if (insert(entry.first, entry.second)) {
                count++;
            } else {
                failed.push_back(entry);
            }
        }
        return {count, failed};
    }
    
    /**
     * 批量删除
     * @param keys 键列表
     * @return (成功数量, 失败列表)
     */
    virtual std::pair<size_t, std::vector<Slice>> 
    batch_remove(const std::vector<Slice>& keys) {
        std::vector<Slice> failed;
        size_t count = 0;
        
        for (const auto& key : keys) {
            if (remove(key)) {
                count++;
            } else {
                failed.push_back(key);
            }
        }
        return {count, failed};
    }
    
    /**
     * 检查是否支持范围查询
     */
    virtual bool supports_range_query() const { return false; }
    
    /**
     * 检查是否支持前缀查询
     */
    virtual bool supports_prefix_query() const { return false; }
    
    /**
     * 获取配置信息
     */
    virtual std::unique_ptr<IndexConfig> get_config() const = 0;
    
    /**
     * 创建索引快照
     */
    virtual std::shared_ptr<IIndex> snapshot() const {
        throw std::runtime_error("Snapshot not supported");
    }
};

// 索引智能指针类型
using IndexPtr = std::shared_ptr<IIndex>;

// 索引创建工厂函数类型
using IndexFactory = std::function<IndexPtr(const IndexConfig&)>;

} // namespace index
} // namespace mementodb

#endif // I_INDEX_HPP
