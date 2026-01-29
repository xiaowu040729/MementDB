#include "BloomFilter.hpp"
#include <algorithm>
#include <fstream>
#include <cstring>
#include <cmath>

namespace mementodb {
namespace index {

BloomFilter::BloomFilter(const BloomFilterConfig& config) 
    : config_(config), bit_array_size_(0), byte_array_size_(0), 
      num_hash_functions_(0), element_count_(0) {
    
    if (!config_.validate()) {
        throw std::invalid_argument("Invalid BloomFilterConfig");
    }
    
    // 计算最优参数
    config_.calculate_optimal_parameters();
    
    bit_array_size_ = config_.bit_array_size;
    // 防止异常大的位数组导致内存分配失败，设置一个上限（约 64 MB）
    const size_t kMaxBits = 512ull * 1024 * 1024; // 512M bits ~ 64MB
    if (bit_array_size_ == 0 || bit_array_size_ > kMaxBits) {
        bit_array_size_ = std::min(bit_array_size_ == 0 ? size_t(8192) : bit_array_size_, kMaxBits);
        config_.bit_array_size = bit_array_size_;
    }
    byte_array_size_ = (bit_array_size_ + 7) / 8;
    num_hash_functions_ = config_.num_hash_functions;
    
    // 初始化位数组
    bit_array_.resize(byte_array_size_, 0);
    
    // 初始化计数数组（如果使用计数布隆过滤器）
    if (config_.counting_bloom) {
        size_t counter_count = bit_array_size_;
        size_t bytes_per_counter = (config_.counting_bits + 7) / 8;
        count_array_.resize(counter_count * bytes_per_counter, 0);
    }
    
    // 初始化哈希种子
    init_hash_seeds();
}

bool BloomFilter::insert(const Slice& key, uint64_t value) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    try {
        auto positions = compute_hash_positions(key);
        
        if (config_.counting_bloom) {
            // 计数布隆过滤器：增加计数器
            for (size_t pos : positions) {
                increment_counter(pos);
            }
        } else {
            // 标准布隆过滤器：设置位
            for (size_t pos : positions) {
                set_bit(pos);
            }
        }
        
        element_count_++;
        record_insert();
        return true;
    } catch (...) {
        return false;
    }
}

bool BloomFilter::remove(const Slice& key) {
    if (!config_.counting_bloom) {
        return false; // 标准布隆过滤器不支持删除
    }
    
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    try {
        auto positions = compute_hash_positions(key);
        
        // 检查所有位置是否都已设置
        for (size_t pos : positions) {
            if (get_counter(pos) == 0) {
                return false; // 键不存在
            }
        }
        
        // 减少计数器
        for (size_t pos : positions) {
            decrement_counter(pos);
        }
        
        element_count_--;
        record_delete();
        return true;
    } catch (...) {
        return false;
    }
}

bool BloomFilter::contains(const Slice& key) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    stats_.lookup_count++;
    
    auto positions = compute_hash_positions(key);
    
    if (config_.counting_bloom) {
        // 计数布隆过滤器：检查计数器是否都大于0
        for (size_t pos : positions) {
            if (get_counter(pos) == 0) {
                record_lookup(false);
                return false;
            }
        }
    } else {
        // 标准布隆过滤器：检查位是否都已设置
        for (size_t pos : positions) {
            if (!test_bit(pos)) {
                record_lookup(false);
                return false;
            }
        }
    }
    
    // 所有位置都已设置，可能为真阳性或假阳性
    bool is_false_positive = (element_count_.load() == 0);
    record_lookup(is_false_positive);
    return true;
}

void BloomFilter::clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    std::fill(bit_array_.begin(), bit_array_.end(), 0);
    if (config_.counting_bloom) {
        std::fill(count_array_.begin(), count_array_.end(), 0);
    }
    element_count_ = 0;
    lock.unlock();
    reset_statistics();
}

size_t BloomFilter::size() const {
    return element_count_.load();
}

bool BloomFilter::empty() const {
    return size() == 0;
}

IndexStatistics BloomFilter::get_statistics() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    IndexStatistics stats;
    stats.total_keys = element_count_.load();
    stats.memory_usage_bytes = byte_array_size_ + 
                               (config_.counting_bloom ? count_array_.size() : 0);
    stats.insert_count = stats_.insert_count.load();
    stats.lookup_count = stats_.lookup_count.load();
    stats.delete_count = stats_.delete_count.load();
    stats.hit_count = stats_.lookup_count.load() - stats_.false_positive_count.load();
    stats.miss_count = 0; // 无法区分未命中，保留为0
    
    return stats;
}

void BloomFilter::reset_statistics() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    stats_.insert_count.store(0);
    stats_.lookup_count.store(0);
    stats_.false_positive_count.store(0);
    stats_.delete_count.store(0);
    stats_.actual_inserts.store(0);
}

bool BloomFilter::persist(const std::string& path) const {
    std::string filepath = path.empty() ? config_.name + ".bf" : path;
    return save_to_file(filepath);
}

bool BloomFilter::load(const std::string& path) {
    return load_from_file(path);
}

std::pair<size_t, std::vector<std::pair<Slice, uint64_t>>> 
BloomFilter::batch_insert(const std::vector<std::pair<Slice, uint64_t>>& entries) {
    size_t count = 0;
    for (const auto& entry : entries) {
        if (insert(entry.first, entry.second)) {
            count++;
        }
    }
    return {count, {}};
}

double BloomFilter::get_actual_false_positive_rate() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    size_t n = element_count_.load();
    if (n == 0) return 0.0;
    
    // 计算实际假阳性率：p = (1 - e^(-k*n/m))^k
    double m = static_cast<double>(bit_array_size_);
    double k = static_cast<double>(num_hash_functions_);
    
    double exponent = -k * n / m;
    double base = 1.0 - std::exp(exponent);
    return std::pow(base, k);
}

double BloomFilter::get_bit_usage_ratio() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    size_t set_bits = 0;
    for (size_t i = 0; i < bit_array_size_; ++i) {
        if (test_bit(i)) {
            set_bits++;
        }
    }
    
    return static_cast<double>(set_bits) / bit_array_size_;
}

size_t BloomFilter::get_estimated_element_count() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    // 基于位使用率估算元素数量
    double bit_ratio = get_bit_usage_ratio();
    if (bit_ratio == 0.0) return 0;
    
    // n ≈ -m * ln(1 - p) / k
    double m = static_cast<double>(bit_array_size_);
    double k = static_cast<double>(num_hash_functions_);
    double p = bit_ratio;
    
    double n = -m * std::log(1.0 - p) / k;
    return static_cast<size_t>(std::round(n));
}

bool BloomFilter::merge(const BloomFilter& other) {
    if (bit_array_size_ != other.bit_array_size_ ||
        num_hash_functions_ != other.num_hash_functions_ ||
        config_.counting_bloom != other.config_.counting_bloom) {
        return false; // 配置不匹配
    }
    
    std::unique_lock<std::shared_mutex> lock(mutex_);
    std::shared_lock<std::shared_mutex> other_lock(other.mutex_);
    
    if (config_.counting_bloom) {
        // 计数布隆过滤器：合并计数器
        for (size_t i = 0; i < count_array_.size(); ++i) {
            count_array_[i] = std::min(255, 
                static_cast<int>(count_array_[i]) + 
                static_cast<int>(other.count_array_[i]));
        }
    } else {
        // 标准布隆过滤器：按位或
        for (size_t i = 0; i < byte_array_size_; ++i) {
            bit_array_[i] |= other.bit_array_[i];
        }
    }
    
    element_count_ += other.element_count_.load();
    return true;
}

// 私有辅助方法

void BloomFilter::init_hash_seeds() {
    hash_seeds_.clear();
    hash_seeds_.reserve(num_hash_functions_);
    
    // 为每个哈希函数生成不同的种子
    uint64_t base_seed = 0x9747b28c;
    for (size_t i = 0; i < num_hash_functions_; ++i) {
        hash_seeds_.push_back(base_seed + i * 0x9e3779b9);
    }
}

void BloomFilter::set_bit(size_t bit_index) {
    if (bit_index >= bit_array_size_) return;
    
    size_t byte_index = bit_index / 8;
    size_t bit_offset = bit_index % 8;
    bit_array_[byte_index] |= (1 << bit_offset);
}

void BloomFilter::clear_bit(size_t bit_index) {
    if (bit_index >= bit_array_size_) return;
    
    size_t byte_index = bit_index / 8;
    size_t bit_offset = bit_index % 8;
    bit_array_[byte_index] &= ~(1 << bit_offset);
}

bool BloomFilter::test_bit(size_t bit_index) const {
    if (bit_index >= bit_array_size_) return false;
    
    size_t byte_index = bit_index / 8;
    size_t bit_offset = bit_index % 8;
    return (bit_array_[byte_index] & (1 << bit_offset)) != 0;
}

void BloomFilter::increment_counter(size_t counter_index) {
    if (!config_.counting_bloom || counter_index >= bit_array_size_) return;
    
    size_t bytes_per_counter = (config_.counting_bits + 7) / 8;
    size_t offset = counter_index * bytes_per_counter;
    
    // 简单的计数器增加（实际应该考虑溢出）
    uint8_t max_value = (1 << config_.counting_bits) - 1;
    if (count_array_[offset] < max_value) {
        count_array_[offset]++;
    }
}

void BloomFilter::decrement_counter(size_t counter_index) {
    if (!config_.counting_bloom || counter_index >= bit_array_size_) return;
    
    size_t bytes_per_counter = (config_.counting_bits + 7) / 8;
    size_t offset = counter_index * bytes_per_counter;
    
    if (count_array_[offset] > 0) {
        count_array_[offset]--;
    }
}

uint8_t BloomFilter::get_counter(size_t counter_index) const {
    if (!config_.counting_bloom || counter_index >= bit_array_size_) return 0;
    
    size_t bytes_per_counter = (config_.counting_bits + 7) / 8;
    size_t offset = counter_index * bytes_per_counter;
    
    return count_array_[offset];
}

std::vector<size_t> BloomFilter::compute_hash_positions(const Slice& key) const {
    std::vector<size_t> positions;
    positions.reserve(num_hash_functions_);
    
    if (config_.enable_fast_hash) {
        // 使用双哈希法模拟多个哈希函数
        auto [h1, h2] = double_hash(key);
        
        for (size_t i = 0; i < num_hash_functions_; ++i) {
            size_t pos = (h1 + i * h2) % bit_array_size_;
            positions.push_back(pos);
        }
    } else {
        // 使用多个独立的哈希函数
        for (size_t i = 0; i < num_hash_functions_; ++i) {
            uint64_t hash = utils::Hash::ComputeHash64(
                key.data(), key.size(), hash_seeds_[i]);
            size_t pos = hash % bit_array_size_;
            positions.push_back(pos);
        }
    }
    
    return positions;
}

std::pair<uint64_t, uint64_t> BloomFilter::double_hash(const Slice& key) const {
    // 使用两个不同的哈希函数
    uint64_t h1 = utils::Hash::ComputeHash64(key.data(), key.size(), 0x9747b28c);
    uint64_t h2 = utils::Hash::ComputeHash64(key.data(), key.size(), 0x9e3779b9);
    
    // 确保 h2 是奇数（用于更好的分布）
    if (h2 % 2 == 0) {
        h2++;
    }
    
    return {h1, h2};
}

void BloomFilter::record_insert() const {
    stats_.insert_count++;
    stats_.actual_inserts++;
}

void BloomFilter::record_lookup(bool is_false_positive) const {
    if (is_false_positive) {
        stats_.false_positive_count++;
    }
}

void BloomFilter::record_delete() const {
    stats_.delete_count++;
}

bool BloomFilter::save_to_file(const std::string& path) const {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) {
        return false;
    }
    
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    // 写入魔数
    const char magic[] = "BLOOM1.0";
    ofs.write(magic, 8);
    
    // 写入版本
    uint64_t version = 1;
    ofs.write(reinterpret_cast<const char*>(&version), sizeof(version));
    
    // 写入配置
    ofs.write(reinterpret_cast<const char*>(&config_.expected_element_count), 
              sizeof(config_.expected_element_count));
    ofs.write(reinterpret_cast<const char*>(&config_.false_positive_rate), 
              sizeof(config_.false_positive_rate));
    ofs.write(reinterpret_cast<const char*>(&config_.num_hash_functions), 
              sizeof(config_.num_hash_functions));
    ofs.write(reinterpret_cast<const char*>(&config_.bit_array_size), 
              sizeof(config_.bit_array_size));
    ofs.write(reinterpret_cast<const char*>(&config_.counting_bloom), 
              sizeof(config_.counting_bloom));
    
    // 写入位数组
    ofs.write(reinterpret_cast<const char*>(bit_array_.data()), byte_array_size_);
    
    // 写入计数数组（如果使用）
    if (config_.counting_bloom) {
        ofs.write(reinterpret_cast<const char*>(count_array_.data()), count_array_.size());
    }
    
    // 写入元素数量
    uint64_t count = element_count_.load();
    ofs.write(reinterpret_cast<const char*>(&count), sizeof(count));
    
    return ofs.good();
}

bool BloomFilter::load_from_file(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return false;
    }
    
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    // 读取魔数
    char magic[8];
    ifs.read(magic, 8);
    if (std::memcmp(magic, "BLOOM1.0", 8) != 0) {
        return false;
    }
    
    // 读取版本
    uint64_t version;
    ifs.read(reinterpret_cast<char*>(&version), sizeof(version));
    
    // 读取配置
    size_t expected_count, num_hash, bit_size;
    double false_rate;
    bool counting;
    
    ifs.read(reinterpret_cast<char*>(&expected_count), sizeof(expected_count));
    ifs.read(reinterpret_cast<char*>(&false_rate), sizeof(false_rate));
    ifs.read(reinterpret_cast<char*>(&num_hash), sizeof(num_hash));
    ifs.read(reinterpret_cast<char*>(&bit_size), sizeof(bit_size));
    ifs.read(reinterpret_cast<char*>(&counting), sizeof(counting));
    
    // 验证配置
    if (bit_size != bit_array_size_ || num_hash != num_hash_functions_ || 
        counting != config_.counting_bloom) {
        return false;
    }
    
    // 读取位数组
    ifs.read(reinterpret_cast<char*>(bit_array_.data()), byte_array_size_);
    
    // 读取计数数组（如果使用）
    if (config_.counting_bloom) {
        ifs.read(reinterpret_cast<char*>(count_array_.data()), count_array_.size());
    }
    
    // 读取元素数量
    uint64_t count;
    ifs.read(reinterpret_cast<char*>(&count), sizeof(count));
    element_count_ = count;
    
    return ifs.good();
}

} // namespace index
} // namespace mementodb

