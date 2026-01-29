#ifndef BPLUS_TREE_HPP
#define BPLUS_TREE_HPP

#include <iostream>
#include <vector>
#include <algorithm>
#include <memory>
#include <queue>
#include <stack>
#include <functional>
#include <fstream>
#include <cassert>
#include "../utils/LoggingSystem/LogMacros.hpp"

namespace BPlusTree {

// 配置参数
struct BPlusTreeConfig {
    int order = 4;                  // 阶数，决定节点容量
    int leaf_order = 3;            // 叶子节点容量
    bool use_verbose_log = false;  // 调试日志开关
    std::string storage_path;      // 持久化存储路径
};

// 键值对模板
template<typename KeyType, typename ValueType>
struct KeyValuePair {
    KeyType key;
    ValueType value;
    
    KeyValuePair() = default;
    KeyValuePair(const KeyType& k, const ValueType& v) : key(k), value(v) {}
    
    bool operator<(const KeyValuePair& other) const {
        return key < other.key;
    }
    
    bool operator==(const KeyValuePair& other) const {
        return key == other.key;
    }
};

// 前向声明
template<typename KeyType, typename ValueType>
class BPlusTree;

// 节点基类
template<typename KeyType, typename ValueType>
class Node {
public:
    using KeyValue = KeyValuePair<KeyType, ValueType>;
    
    Node(bool is_leaf) 
        : is_leaf_(is_leaf), 
          parent_(nullptr), 
          next_(nullptr),
          prev_(nullptr),
          node_id_(generate_id_()) {}
    
    virtual ~Node() = default;
    
    bool is_leaf() const { return is_leaf_; }
    Node* parent() const { return parent_; }
    Node* next() const { return next_; }
    Node* prev() const { return prev_; }
    size_t node_id() const { return node_id_; }
    
    // 设置父节点
    void set_parent(Node* parent) { parent_ = parent; }
    // 设置下一个节点
    void set_next(Node* next) { next_ = next; }
    // 设置前一个节点
    void set_prev(Node* prev) { prev_ = prev; }
    
    // 获取节点大小
    virtual size_t size() const = 0;
    // 是否满
    virtual bool is_full() const = 0;
    // 是否下溢
    virtual bool is_underflow() const = 0;
    // 获取键
    virtual KeyType get_key(size_t index) const = 0;
    // 打印节点
    virtual void print(int indent = 0) const = 0;
    // 转换为字符串
    virtual std::string to_string() const = 0;
    
    // 序列化接口
    virtual void serialize(std::ostream& os) const = 0;
    // 反序列化接口
    virtual void deserialize(std::istream& is) = 0;
    
protected:
    static size_t generate_id_() {
        static size_t counter = 0;
        return counter++;
    }
    
    bool is_leaf_;
    Node* parent_;
    Node* next_;    // 叶子节点链表
    Node* prev_;    // 叶子节点链表
    size_t node_id_;
};

// 前向声明
template<typename KeyType, typename ValueType>
class BPlusTree;

// 内部节点
template<typename KeyType, typename ValueType>
class InternalNode : public Node<KeyType, ValueType> {
    friend class BPlusTree<KeyType, ValueType>;
    
public:
    using Base = Node<KeyType, ValueType>;
    using NodePtr = std::shared_ptr<Node<KeyType, ValueType>>;
    
    InternalNode(int order, const BPlusTreeConfig& config)
        : Base(false), order_(order), config_(config) {}
    
    size_t size() const override { return keys_.size(); }
    
    bool is_full() const override { 
        return keys_.size() >= static_cast<size_t>(order_ - 1); 
    }
    
    bool is_underflow() const override {
        // 根节点允许最小为2个孩子，非根节点需要至少一半个孩子
        if (this->parent_ == nullptr) {
            return children_.size() < 2;
        }
        return children_.size() < static_cast<size_t>((order_ + 1) / 2);
    }
    
    KeyType get_key(size_t index) const override {
        if (index < keys_.size()) {
            return keys_[index];
        }
        LOG_ERROR("BPlusTree", "Internal node key index out of range");
        throw std::out_of_range("Internal node key index out of range");
    }
    
    // 插入键和孩子
    void insert(const KeyType& key, const NodePtr& child) {
        // 找到插入位置
        auto it = std::lower_bound(keys_.begin(), keys_.end(), key);
        size_t pos = it - keys_.begin();
        
        // 插入键
        keys_.insert(it, key);
        
        // 插入孩子节点，注意孩子比键多一个
        children_.insert(children_.begin() + pos + 1, child);
        child->set_parent(this);
    }
    
    // 移除键和孩子
    void remove(const KeyType& key) {
        auto it = std::find(keys_.begin(), keys_.end(), key);
        if (it != keys_.end()) {
            size_t pos = it - keys_.begin();
            keys_.erase(it);
            // 移除对应的孩子节点
            children_.erase(children_.begin() + pos + 1);
        }
    }
    
    // 获取子节点指针
    NodePtr get_child(size_t index) const {
        if (index < children_.size()) {
            return children_[index];
        }
        return nullptr;
    }
    
    // 设置第一个子节点（用于根节点分裂）
    void set_first_child(const NodePtr& child) {
        if (children_.empty()) {
            children_.push_back(child);
        } else {
            children_[0] = child;
        }
        child->set_parent(this);
    }
    
    // 根据键查找应该去哪个子节点
    NodePtr find_child(const KeyType& key) const {
        // keys_[i] 存储的是 children_[i+1] 的最小键
        // 所以对于key，找到第一个 keys_[i] > key，然后返回 children_[i]
        for (size_t i = 0; i < keys_.size(); ++i) {
            if (key < keys_[i]) {
                return children_[i];
            }
        }
        return children_.back();
    }
    
    // 分裂节点
    std::pair<KeyType, std::shared_ptr<InternalNode>> split() {
        auto new_node = std::make_shared<InternalNode>(order_, config_);
        
        size_t split_pos = keys_.size() / 2;
        KeyType split_key = keys_[split_pos];
        
        // 移动后半部分键和孩子到新节点
        new_node->keys_.assign(keys_.begin() + split_pos + 1, keys_.end());
        new_node->children_.assign(children_.begin() + split_pos + 1, children_.end());
        
        // 更新父指针
        for (auto& child : new_node->children_) {
            child->set_parent(new_node.get());
        }
        
        // 保留前半部分
        keys_.resize(split_pos);
        children_.resize(split_pos + 1);
        
        return {split_key, new_node};
    }
    
    // 打印节点
    void print(int indent = 0) const override {
        std::string spaces(indent, ' ');
        std::cout << spaces << "InternalNode[" << this->node_id_ << "]: ";
        for (const auto& key : keys_) {
            std::cout << key << " ";
        }
        std::cout << std::endl;
        
        for (const auto& child : children_) {
            if (child) {
                child->print(indent + 2);
            }
        }
    }
    
    // 转换为字符串
    std::string to_string() const override {
        std::string result = "Internal[ID=" + std::to_string(this->node_id_) + 
                           ", Keys=";
        for (size_t i = 0; i < keys_.size(); ++i) {
            if (i > 0) result += ",";
            // 使用流输出键值（支持各种类型）
            std::ostringstream oss;
            oss << keys_[i];
            result += oss.str();
        }
        result += "]";
        return result;
    }
    
    // 序列化
    void serialize(std::ostream& os) const override {
        size_t key_count = keys_.size();
        os.write(reinterpret_cast<const char*>(&key_count), sizeof(key_count));
        for (const auto& key : keys_) {
            os.write(reinterpret_cast<const char*>(&key), sizeof(key));
        }
    }
    // 反序列化
    void deserialize(std::istream& is) override {
        size_t key_count;
        is.read(reinterpret_cast<char*>(&key_count), sizeof(key_count));
        keys_.resize(key_count);
        for (size_t i = 0; i < key_count; ++i) {
            is.read(reinterpret_cast<char*>(&keys_[i]), sizeof(KeyType));
        }
    }
    
private:
    int order_;
    BPlusTreeConfig config_;
    std::vector<KeyType> keys_;            // 键值
    std::vector<NodePtr> children_;        // 子节点指针
};

// 叶子节点
template<typename KeyType, typename ValueType>
class LeafNode : public Node<KeyType, ValueType> {
    friend class BPlusTree<KeyType, ValueType>;
    
public:
    using Base = Node<KeyType, ValueType>;
    using KeyValue = KeyValuePair<KeyType, ValueType>;
    
    LeafNode(int leaf_order, const BPlusTreeConfig& config)
        : Base(true), leaf_order_(leaf_order), config_(config) {}
    
    size_t size() const override { return entries_.size(); }
    
    bool is_full() const override {
        return entries_.size() >= static_cast<size_t>(leaf_order_);
    }
    
    bool is_underflow() const override {
        // 根节点允许最少1个条目，非根节点需要至少ceil(leaf_order/2)个条目
        if (this->parent_ == nullptr) {
            return entries_.size() < 1;
        }
        return entries_.size() < static_cast<size_t>((leaf_order_ + 1) / 2);
    }
    
    KeyType get_key(size_t index) const override {
        if (index < entries_.size()) {
            return entries_[index].key;
        }
        LOG_ERROR("BPlusTree", "Leaf node key index out of range");
        throw std::out_of_range("Leaf node key index out of range");
    }
    
    // 查找键值对
    ValueType* find(const KeyType& key) {
        // 
        auto it = std::lower_bound(entries_.begin(), entries_.end(), 
                                   KeyValue(key, ValueType()));
        if (it != entries_.end() && it->key == key) {
            return &it->value;
        }
        LOG_INFO("BPlusTree", "Leaf node key not found");
        return nullptr;
    }
    
    // 插入键值对
    bool insert(const KeyType& key, const ValueType& value) {
        auto it = std::lower_bound(entries_.begin(), entries_.end(),
                                   KeyValue(key, ValueType()));
        
        // 如果键已存在，更新值
        if (it != entries_.end() && it->key == key) {
            it->value = value;
            LOG_ERROR("BPlusTree", "Leaf node key already exists, updating value");
            return false;
        }
        
        // 插入新条目
        entries_.insert(it, KeyValue(key, value));
        LOG_DEBUG("BPlusTree", "Leaf node key inserted");
        return true;
    }
    
    // 移除键值对
    bool remove(const KeyType& key) {
        auto it = std::lower_bound(entries_.begin(), entries_.end(),
                                   KeyValue(key, ValueType()));
        // 如果键已存在，删除
        if (it != entries_.end() && it->key == key) {
            entries_.erase(it);
            LOG_DEBUG("BPlusTree", "Leaf node key removed");
            return true;
        }
        // 如果键不存在，返回false 并打印错误日志
        LOG_ERROR("BPlusTree", "Leaf node key not found");
        return false;
    }
    
    // 范围查找
    template<typename Callback>
    void range_query(const KeyType& start, const KeyType& end, Callback callback) {
        auto it = std::lower_bound(entries_.begin(), entries_.end(),
                                   KeyValue(start, ValueType()));
        while (it != entries_.end() && it->key <= end) {
            callback(it->key, it->value);
            ++it;
        }
    }
    
    // 分裂叶子节点
    std::pair<KeyType, std::shared_ptr<LeafNode>> split() {
        auto new_node = std::make_shared<LeafNode>(leaf_order_, config_);
        
        size_t split_pos = entries_.size() / 2;
        KeyType split_key = entries_[split_pos].key;
        
        // 移动后半部分条目到新节点
        new_node->entries_.assign(entries_.begin() + split_pos, entries_.end());
        entries_.resize(split_pos);
        
        // 更新链表指针
        new_node->set_next(this->next_);
        new_node->set_prev(this);
        if (this->next_) {
            this->next_->set_prev(new_node.get());
        }
        this->set_next(new_node.get());
        
        return {split_key, new_node};
    }
    
    // 从兄弟节点借用条目
    bool borrow_from_left(LeafNode* left_sibling) {
        if (left_sibling->size() <= 1) return false;
        
        // 从左兄弟借用最后一个条目
        entries_.insert(entries_.begin(), left_sibling->entries_.back());
        left_sibling->entries_.pop_back();
        
        return true;
    }
    
    bool borrow_from_right(LeafNode* right_sibling) {
        if (right_sibling->size() <= 1) return false;
        
        // 从右兄弟借用第一个条目
        entries_.push_back(right_sibling->entries_.front());
        right_sibling->entries_.erase(right_sibling->entries_.begin());
        
        return true;
    }
    
    // 合并到左兄弟节点
    void merge_to_left(LeafNode* left_sibling) {
        left_sibling->entries_.insert(left_sibling->entries_.end(),
                                      entries_.begin(), entries_.end());
        // 更新链表指针
        left_sibling->set_next(this->next_);
        if (this->next_) {
            this->next_->set_prev(left_sibling);
        }
    }
    
    // 打印节点
    void print(int indent = 0) const override {
        std::string spaces(indent, ' ');
        std::cout << spaces << "LeafNode[" << this->node_id_ << "]: ";
        for (const auto& entry : entries_) {
            std::cout << "(" << entry.key << ":" << entry.value << ") ";
        }
        std::cout << std::endl;
    }
    
    std::string to_string() const override {
        std::string result = "Leaf[ID=" + std::to_string(this->node_id_) + 
                           ", Entries=";
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (i > 0) result += ",";
            std::ostringstream oss;
            oss << entries_[i].key;
            result += oss.str() + ":" + std::to_string(entries_[i].value);
        }
        result += "]";
        return result;
    }
    
    // 获取所有条目
    const std::vector<KeyValue>& get_entries() const { return entries_; }
    
    // 序列化
    void serialize(std::ostream& os) const override {
        size_t entry_count = entries_.size();
        os.write(reinterpret_cast<const char*>(&entry_count), sizeof(entry_count));
        for (const auto& entry : entries_) {
            os.write(reinterpret_cast<const char*>(&entry.key), sizeof(KeyType));
            os.write(reinterpret_cast<const char*>(&entry.value), sizeof(ValueType));
        }
    }
    
    void deserialize(std::istream& is) override {
        size_t entry_count;
        is.read(reinterpret_cast<char*>(&entry_count), sizeof(entry_count));
        entries_.resize(entry_count);
        for (size_t i = 0; i < entry_count; ++i) {
            is.read(reinterpret_cast<char*>(&entries_[i].key), sizeof(KeyType));
            is.read(reinterpret_cast<char*>(&entries_[i].value), sizeof(ValueType));
        }
    }
    
private:
    int leaf_order_;
    BPlusTreeConfig config_;
    std::vector<KeyValue> entries_;  // 键值对列表
};

// B+树主类
template<typename KeyType, typename ValueType>
class BPlusTree {
public:
    using NodeType = Node<KeyType, ValueType>;
    using InternalNodeType = InternalNode<KeyType, ValueType>;
    using LeafNodeType = LeafNode<KeyType, ValueType>;
    using NodePtr = std::shared_ptr<NodeType>;
    using InternalNodePtr = std::shared_ptr<InternalNodeType>;
    using LeafNodePtr = std::shared_ptr<LeafNodeType>;
    
    BPlusTree(const BPlusTreeConfig& config = BPlusTreeConfig{})
        : config_(config), 
          root_(nullptr), 
          leaf_head_(nullptr),
          size_(0) {
        // 创建第一个叶子节点作为根
        root_ = std::make_shared<LeafNodeType>(config.leaf_order, config);
        leaf_head_ = std::static_pointer_cast<LeafNodeType>(root_);
    }
    
    // 插入操作
    bool insert(const KeyType& key, const ValueType& value) {
        if (config_.use_verbose_log) {
            std::cout << "[Insert] key=" << key << ", value=" << value << std::endl;
        }
        
        // 查找应该插入的叶子节点
        LeafNodePtr leaf = find_leaf(key);
        if (!leaf) {
            return false;
        }
        
        // 插入到叶子节点
        if (!leaf->insert(key, value)) {
            // 键已存在，只更新值
            if (config_.use_verbose_log) {
                std::cout << "  Key already exists, updating value" << std::endl;
            }
            return false;
        }
        
        size_++;
        
        // 如果叶子节点满了，需要分裂
        if (leaf->is_full()) {
            split_leaf(leaf);
        }
        
        return true;
    }
    
    // 查找操作
    ValueType* find(const KeyType& key) {
        LeafNodePtr leaf = find_leaf(key);
        if (!leaf) {
            return nullptr;
        }
        return leaf->find(key);
    }
    
    // 删除操作
    bool remove(const KeyType& key) {
        if (config_.use_verbose_log) {
            std::cout << "[Remove] key=" << key << std::endl;
        }
        
        // 查找键所在的叶子节点
        LeafNodePtr leaf = find_leaf(key);
        if (!leaf) {
            LOG_ERROR("BPlusTree", "Leaf node not found");
            return false;
        }
        
        // 从叶子节点中删除
        if (!leaf->remove(key)) {
            LOG_ERROR("BPlusTree", "Leaf node key not found");
            return false;  // 键不存在
        }
        
        size_--;
        
        // 如果叶子节点下溢，需要调整
        if (leaf->is_underflow() && leaf.get() != root_.get()) {
            rebalance_leaf(leaf);
        }
        
        // 如果根节点是内部节点且只有一个孩子，降低树高
        adjust_root();
        
        return true;
    }
    
    // 范围查询
    template<typename Callback>
    void range_query(const KeyType& start, const KeyType& end, Callback callback) {
        LeafNodePtr leaf = find_leaf(start);
        if (!leaf) {
            return;
        }
        
        // 遍历叶子节点链表
        while (leaf) {
            leaf->range_query(start, end, callback);
            
            // 如果当前叶子的最大键小于end，继续下一个叶子
            if (!leaf->get_entries().empty() && 
                leaf->get_entries().back().key < end) {
                NodeType* next_node = leaf->next();
                if (next_node) {
                    // 注意：这里假设next_node指向的节点也在某个shared_ptr中管理
                    // 为了编译通过，我们创建一个临时的shared_ptr，但不拥有所有权
                    leaf = std::static_pointer_cast<LeafNodeType>(
                        std::shared_ptr<NodeType>(next_node, [](NodeType*){}));
                } else {
                    break;
                }
            } else {
                break;
            }
        }
    }
    
    // 打印整棵树
    void print_tree() const {
        std::cout << "=== B+Tree (size=" << size_ << ") ===" << std::endl;
        if (root_) {
            root_->print();
        }
        std::cout << "=== Leaf List ===" << std::endl;
        LeafNodePtr leaf = leaf_head_;
        while (leaf) {
            leaf->print(2);
            NodeType* next_node = leaf->next();
            if (next_node) {
                // 注意：这里假设next_node指向的节点也在某个shared_ptr中管理
                // 为了编译通过，我们创建一个临时的shared_ptr，但不拥有所有权
                leaf = std::static_pointer_cast<LeafNodeType>(
                    std::shared_ptr<NodeType>(next_node, [](NodeType*){}));
            } else {
                break;
            }
        }
    }
    
    // 获取树高
    size_t height() const {
        size_t height = 0;
        NodePtr node = root_;
        while (node && !node->is_leaf()) {
            auto internal = std::static_pointer_cast<InternalNodeType>(node);
            if (internal->size() > 0) {
                node = internal->get_child(0);
                height++;
            } else {
                break;
            }
        }
        return height + 1;  // 加1是叶子节点层
    }
    
    // 获取大小
    size_t size() const { return size_; }
    
    // 检查树是否为空
    bool empty() const { return size_ == 0; }
    
    // 验证树的结构
    bool validate() const {
        if (!root_) {
            return size_ == 0;
        }
        
        std::queue<std::pair<NodePtr, KeyType>> q;
        q.push({root_, KeyType()});
        size_t leaf_count = 0;
        size_t total_entries = 0;
        
        while (!q.empty()) {
            auto [node, min_key] = q.front();
            q.pop();
            
            if (node->is_leaf()) {
                auto leaf = std::static_pointer_cast<LeafNodeType>(node);
                leaf_count++;
                total_entries += leaf->size();
                
                // 检查叶子节点是否有序
                if (!leaf->get_entries().empty()) {
                    for (size_t i = 1; i < leaf->size(); ++i) {
                        if (leaf->get_entries()[i].key < leaf->get_entries()[i-1].key) {
                            std::cerr << "Leaf entries not sorted" << std::endl;
                            return false;
                        }
                    }
                }
            } else {
                auto internal = std::static_pointer_cast<InternalNodeType>(node);
                
                // 检查内部节点键值是否有序
                for (size_t i = 1; i < internal->size(); ++i) {
                    if (internal->get_key(i) <= internal->get_key(i-1)) {
                        std::cerr << "Internal node keys not sorted" << std::endl;
                        return false;
                    }
                }
                
                // 将子节点加入队列
                for (size_t i = 0; i <= internal->size(); ++i) {
                    NodePtr child = internal->get_child(i);
                    if (child) {
                        KeyType child_min = (i == 0) ? min_key : internal->get_key(i-1);
                        q.push({child, child_min});
                    }
                }
            }
        }
        
        // 检查条目总数
        if (total_entries != size_) {
            std::cerr << "Entry count mismatch: total=" << total_entries 
                      << ", size=" << size_ << std::endl;
            return false;
        }
        
        return true;
    }
    
    // 序列化到文件
    bool serialize_to_file(const std::string& filename) {
        std::ofstream ofs(filename, std::ios::binary);
        if (!ofs) {
            return false;
        }
        
        // 写入元数据
        ofs.write(reinterpret_cast<const char*>(&size_), sizeof(size_));
        
        // 序列化树结构（简化版本，实际需要序列化所有节点）
        // 这里只作为示例，实际实现需要更复杂的序列化
        serialize_node(ofs, root_);
        
        return ofs.good();
    }
    
    // 从文件反序列化
    bool deserialize_from_file(const std::string& filename) {
        std::ifstream ifs(filename, std::ios::binary);
        if (!ifs) {
            return false;
        }
        
        // 读取元数据
        ifs.read(reinterpret_cast<char*>(&size_), sizeof(size_));
        
        // 反序列化树结构
        root_ = deserialize_node(ifs);
        
        // 重建叶子节点链表
        rebuild_leaf_list();
        
        return ifs.good();
    }
    
private:
    // 查找键所在的叶子节点
    LeafNodePtr find_leaf(const KeyType& key) {
        if (!root_) {
            LOG_ERROR("BPlusTree", "Root node not found");
            return nullptr;
        }
        
        NodePtr node = root_;
        while (!node->is_leaf()) {
            auto internal = std::static_pointer_cast<InternalNodeType>(node);
            if (!internal) {
                LOG_ERROR("BPlusTree", "Internal node not found");
                return nullptr;
            }
            node = internal->find_child(key);
        }
        
        return std::static_pointer_cast<LeafNodeType>(node);
    }
    
    // 分裂叶子节点
    void split_leaf(LeafNodePtr leaf) {
        if (config_.use_verbose_log) {
            std::cout << "  Splitting leaf node " << leaf->node_id() << std::endl;
        }
        
        auto [split_key, new_leaf] = leaf->split();
        
        // 如果叶子节点是根节点，需要创建新的根
        if (leaf.get() == root_.get()) {
            auto new_root = std::make_shared<InternalNodeType>(config_.order, config_);
            // 在B+树中，当根节点分裂时：
            // 1. 原来的叶子节点成为第一个子节点（children_[0]）
            // 2. 分裂键和新的叶子节点插入
            new_root->set_first_child(leaf);
            new_root->insert(split_key, new_leaf);
            
            root_ = new_root;
        } else {
            // 否则，将分裂键插入父节点
            auto parent = static_cast<InternalNodeType*>(leaf->parent());
            if (!parent) {
                LOG_ERROR("BPlusTree", "Parent node is null during leaf split");
                return;
            }
            parent->insert(split_key, new_leaf);
            
            // 如果父节点满了，继续分裂
            // 需要通过根节点查找父节点的shared_ptr
            if (parent->is_full()) {
                InternalNodePtr parent_ptr = find_internal_node_ptr(parent);
                if (parent_ptr) {
                    split_internal(parent_ptr);
                }
            }
        }
    }
    
    // 分裂内部节点
    void split_internal(InternalNodePtr internal) {
        if (config_.use_verbose_log) {
            std::cout << "  Splitting internal node " << internal->node_id() << std::endl;
        }
        
        auto [split_key, new_internal] = internal->split();
        
        if (internal.get() == root_.get()) {
            auto new_root = std::make_shared<InternalNodeType>(config_.order, config_);
            // 在B+树中，当根节点分裂时：
            // 1. 原来的内部节点成为第一个子节点（children_[0]）
            // 2. 分裂键和新的内部节点插入
            new_root->set_first_child(internal);
            new_root->insert(split_key, new_internal);
            
            root_ = new_root;
        } else {
            auto parent = static_cast<InternalNodeType*>(internal->parent());
            if (!parent) {
                LOG_ERROR("BPlusTree", "Parent node is null during internal split");
                return;
            }
            parent->insert(split_key, new_internal);
            
            if (parent->is_full()) {
                InternalNodePtr parent_ptr = find_internal_node_ptr(parent);
                if (parent_ptr) {
                    split_internal(parent_ptr);
                }
            }
        }
    }
    
    // 重新平衡叶子节点（删除后）
    void rebalance_leaf(LeafNodePtr leaf) {
        auto parent_raw = static_cast<InternalNodeType*>(leaf->parent());
        if (!parent_raw) {
            return;
        }
        
        // 获取父节点的shared_ptr
        InternalNodePtr parent = find_internal_node_ptr(parent_raw);
        if (!parent) {
            return;
        }
        
        // 找到叶子节点在父节点中的位置
        size_t leaf_index = 0;
        for (; leaf_index <= parent->size(); ++leaf_index) {
            if (parent->get_child(leaf_index).get() == leaf.get()) {
                break;
            }
        }
        
        // 尝试从左兄弟借用
        if (leaf_index > 0) {
            auto left_sibling = std::static_pointer_cast<LeafNodeType>(
                parent->get_child(leaf_index - 1));
            if (left_sibling && left_sibling->size() > static_cast<size_t>((config_.leaf_order + 1) / 2)) {
                if (leaf->borrow_from_left(left_sibling.get())) {
                    // 更新父节点中的分隔键
                    KeyType old_key = parent->get_key(leaf_index - 1);
                    parent->remove(old_key);
                    parent->insert(leaf->get_key(0), leaf);
                    return;
                }
            }
        }
        
        // 尝试从右兄弟借用
        if (leaf_index < parent->size()) {
            auto right_sibling = std::static_pointer_cast<LeafNodeType>(
                parent->get_child(leaf_index + 1));
            if (right_sibling && right_sibling->size() > static_cast<size_t>((config_.leaf_order + 1) / 2)) {
                if (leaf->borrow_from_right(right_sibling.get())) {
                    // 更新父节点中的分隔键
                    KeyType old_key = parent->get_key(leaf_index);
                    parent->remove(old_key);
                    parent->insert(right_sibling->get_key(0), right_sibling);
                    return;
                }
            }
        }
        
        // 需要合并
        if (leaf_index > 0) {
            // 合并到左兄弟
            auto left_sibling = std::static_pointer_cast<LeafNodeType>(
                parent->get_child(leaf_index - 1));
            if (left_sibling) {
            leaf->merge_to_left(left_sibling.get());
            
            // 从父节点中移除分隔键和叶子节点引用
                KeyType old_key = parent->get_key(leaf_index - 1);
                parent->remove(old_key);
            
            // 检查父节点是否需要重新平衡
            if (parent->is_underflow()) {
                    rebalance_internal(parent);
                }
            }
        } else if (leaf_index < parent->size()) {
            // 合并右兄弟到当前节点
            auto right_sibling = std::static_pointer_cast<LeafNodeType>(
                parent->get_child(leaf_index + 1));
            if (right_sibling) {
            right_sibling->merge_to_left(leaf.get());
            
            // 从父节点中移除分隔键和右兄弟引用
                KeyType old_key = parent->get_key(leaf_index);
                parent->remove(old_key);
            
            if (parent->is_underflow()) {
                    rebalance_internal(parent);
                }
            }
        }
    }
    
    // 重新平衡内部节点
    void rebalance_internal(InternalNodePtr internal) {
        if (internal.get() == root_.get()) {
            return;
        }
        
        auto parent_raw = static_cast<InternalNodeType*>(internal->parent());
        if (!parent_raw) {
            return;
        }
        
        // 获取父节点的shared_ptr以便访问私有成员
        InternalNodePtr parent_ptr = find_internal_node_ptr(parent_raw);
        if (!parent_ptr) {
            return;
        }
        
        // 找到内部节点在父节点中的位置
        size_t internal_index = 0;
        for (; internal_index <= parent_ptr->size(); ++internal_index) {
            if (parent_ptr->get_child(internal_index).get() == internal.get()) {
                break;
            }
        }
        
        // 尝试从左兄弟借用
        if (internal_index > 0) {
            auto left_sibling = std::static_pointer_cast<InternalNodeType>(
                parent_ptr->get_child(internal_index - 1));
            if (left_sibling && left_sibling->size() > static_cast<size_t>((config_.order + 1) / 2)) {
                // 从左兄弟借用最后一个键和孩子
                if (!left_sibling->keys_.empty() && !left_sibling->children_.empty()) {
                    KeyType borrowed_key = left_sibling->keys_.back();
                    NodePtr borrowed_child = left_sibling->children_.back();
                    
                    left_sibling->keys_.pop_back();
                    left_sibling->children_.pop_back();
                    
                    // 将父节点的分隔键插入到当前节点
                    KeyType parent_key = parent_ptr->get_key(internal_index - 1);
                    internal->keys_.insert(internal->keys_.begin(), parent_key);
                    internal->children_.insert(internal->children_.begin(), borrowed_child);
                    borrowed_child->set_parent(internal.get());
                    
                    // 更新父节点的分隔键
                    parent_ptr->keys_[internal_index - 1] = borrowed_key;
                    return;
                }
            }
        }
        
        // 尝试从右兄弟借用
        if (internal_index < parent_ptr->size()) {
            auto right_sibling = std::static_pointer_cast<InternalNodeType>(
                parent_ptr->get_child(internal_index + 1));
            if (right_sibling && right_sibling->size() > static_cast<size_t>((config_.order + 1) / 2)) {
                // 从右兄弟借用第一个键和孩子
                if (!right_sibling->keys_.empty() && !right_sibling->children_.empty()) {
                    KeyType borrowed_key = right_sibling->keys_.front();
                    NodePtr borrowed_child = right_sibling->children_.front();
                    
                    right_sibling->keys_.erase(right_sibling->keys_.begin());
                    right_sibling->children_.erase(right_sibling->children_.begin());
                    
                    // 将父节点的分隔键插入到当前节点
                    KeyType parent_key = parent_ptr->get_key(internal_index);
                    internal->keys_.push_back(parent_key);
                    internal->children_.push_back(borrowed_child);
                    borrowed_child->set_parent(internal.get());
                    
                    // 更新父节点的分隔键
                    parent_ptr->keys_[internal_index] = borrowed_key;
                    return;
                }
            }
        }
        
        // 需要合并
        if (internal_index > 0) {
            // 合并到左兄弟
            auto left_sibling = std::static_pointer_cast<InternalNodeType>(
                parent_ptr->get_child(internal_index - 1));
            
            if (left_sibling) {
                // 将父节点的分隔键添加到左兄弟
                KeyType parent_key = parent_ptr->get_key(internal_index - 1);
                left_sibling->keys_.push_back(parent_key);
                
                // 将当前节点的所有键和孩子添加到左兄弟
                left_sibling->keys_.insert(left_sibling->keys_.end(),
                                          internal->keys_.begin(), internal->keys_.end());
                left_sibling->children_.insert(left_sibling->children_.end(),
                                               internal->children_.begin(), internal->children_.end());
                
                // 更新子节点的父指针
                for (auto& child : internal->children_) {
                    child->set_parent(left_sibling.get());
                }
                
                // 从父节点中移除分隔键和当前节点引用
                parent_ptr->remove(parent_key);
                
                // 检查父节点是否需要重新平衡
                if (parent_ptr->is_underflow()) {
                    rebalance_internal(parent_ptr);
                }
            }
        } else if (internal_index < parent_ptr->size()) {
            // 合并右兄弟到当前节点
            auto right_sibling = std::static_pointer_cast<InternalNodeType>(
                parent_ptr->get_child(internal_index + 1));
            
            if (right_sibling) {
                // 将父节点的分隔键添加到当前节点
                KeyType parent_key = parent_ptr->get_key(internal_index);
                internal->keys_.push_back(parent_key);
                
                // 将右兄弟的所有键和孩子添加到当前节点
                internal->keys_.insert(internal->keys_.end(),
                                      right_sibling->keys_.begin(), right_sibling->keys_.end());
                internal->children_.insert(internal->children_.end(),
                                           right_sibling->children_.begin(), right_sibling->children_.end());
                
                // 更新子节点的父指针
                for (auto& child : right_sibling->children_) {
                    child->set_parent(internal.get());
                }
                
                // 从父节点中移除分隔键和右兄弟引用
                parent_ptr->remove(parent_key);
                
                // 检查父节点是否需要重新平衡
                if (parent_ptr->is_underflow()) {
                    rebalance_internal(parent_ptr);
                }
            }
        }
    }
    
    // 调整根节点
    void adjust_root() {
        if (!root_->is_leaf() && root_->size() == 0) {
            // 根节点是内部节点且没有键，降低树高
            auto old_root = std::static_pointer_cast<InternalNodeType>(root_);
            if (old_root->size() == 0) {
                root_ = old_root->get_child(0);
                if (root_) {
                    root_->set_parent(nullptr);
                }
            }
        }
    }
    
    // 序列化节点
    void serialize_node(std::ostream& os, const NodePtr& node) const {
        bool is_leaf = node->is_leaf();
        os.write(reinterpret_cast<const char*>(&is_leaf), sizeof(bool));
        node->serialize(os);
        
        if (!is_leaf) {
            auto internal = std::static_pointer_cast<InternalNodeType>(node);
            size_t child_count = internal->size() + 1;
            for (size_t i = 0; i < child_count; ++i) {
                serialize_node(os, internal->get_child(i));
            }
        }
    }
    
    // 反序列化节点
    NodePtr deserialize_node(std::istream& is) {
        bool is_leaf;
        is.read(reinterpret_cast<char*>(&is_leaf), sizeof(bool));
        
        if (is_leaf) {
            auto leaf = std::make_shared<LeafNodeType>(config_.leaf_order, config_);
            leaf->deserialize(is);
            return leaf;
        } else {
            auto internal = std::make_shared<InternalNodeType>(config_.order, config_);
            // 先读取键的数量
            size_t key_count;
            is.read(reinterpret_cast<char*>(&key_count), sizeof(key_count));
            
            // 读取所有键
            internal->keys_.resize(key_count);
            for (size_t i = 0; i < key_count; ++i) {
                is.read(reinterpret_cast<char*>(&internal->keys_[i]), sizeof(KeyType));
            }
            
            // 反序列化子节点（子节点数量 = 键数量 + 1）
            size_t child_count = key_count + 1;
            internal->children_.reserve(child_count);
            for (size_t i = 0; i < child_count; ++i) {
                NodePtr child = deserialize_node(is);
                if (child) {
                    internal->children_.push_back(child);
                    child->set_parent(internal.get());
                }
            }
            return internal;
        }
    }
    
    // 重建叶子节点链表
    void rebuild_leaf_list() {
        if (!root_) {
            leaf_head_ = nullptr;
            return;
        }
        
        // 找到最左边的叶子节点
        NodePtr node = root_;
        while (!node->is_leaf()) {
            auto internal = std::static_pointer_cast<InternalNodeType>(node);
            if (internal->size() > 0 && internal->get_child(0)) {
                node = internal->get_child(0);
            } else {
                break;
            }
        }
        
        leaf_head_ = std::static_pointer_cast<LeafNodeType>(node);
        
        // 通过中序遍历重建叶子节点链表
        std::vector<LeafNodePtr> leaf_nodes;
        collect_leaf_nodes(root_, leaf_nodes);
        
        // 建立链表连接
        for (size_t i = 0; i < leaf_nodes.size(); ++i) {
            if (i > 0) {
                leaf_nodes[i]->set_prev(leaf_nodes[i-1].get());
            }
            if (i < leaf_nodes.size() - 1) {
                leaf_nodes[i]->set_next(leaf_nodes[i+1].get());
            }
        }
        
        if (!leaf_nodes.empty()) {
            leaf_head_ = leaf_nodes[0];
        }
    }
    
    // 收集所有叶子节点（辅助函数）
    void collect_leaf_nodes(NodePtr node, std::vector<LeafNodePtr>& leaves) {
        if (!node) {
            return;
        }
        
        if (node->is_leaf()) {
            leaves.push_back(std::static_pointer_cast<LeafNodeType>(node));
        } else {
            auto internal = std::static_pointer_cast<InternalNodeType>(node);
            for (size_t i = 0; i <= internal->size(); ++i) {
                NodePtr child = internal->get_child(i);
                if (child) {
                    collect_leaf_nodes(child, leaves);
                }
            }
        }
    }
    
    // 查找内部节点的shared_ptr（辅助函数）
    InternalNodePtr find_internal_node_ptr(InternalNodeType* target) {
        if (!root_ || root_.get() == target) {
            return std::static_pointer_cast<InternalNodeType>(root_);
        }
        
        std::queue<NodePtr> q;
        q.push(root_);
        
        while (!q.empty()) {
            NodePtr node = q.front();
            q.pop();
            
            if (!node->is_leaf()) {
                auto internal = std::static_pointer_cast<InternalNodeType>(node);
                if (internal.get() == target) {
                    return internal;
                }
                
                // 继续搜索子节点
                for (size_t i = 0; i <= internal->size(); ++i) {
                    NodePtr child = internal->get_child(i);
                    if (child) {
                        q.push(child);
        }
                }
            }
        }
        
        return nullptr;
    }
    
private:
    BPlusTreeConfig config_;
    NodePtr root_;
    LeafNodePtr leaf_head_;  // 叶子节点链表头
    size_t size_;            // 总键值对数量
};

} // namespace BPlusTree

#endif // BPLUS_TREE_HPP