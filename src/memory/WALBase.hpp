// File: src/memory/WALBase.hpp
// WAL 基础类模板，提供公共的 WAL 功能
#pragma once

#include <string>
#include <fstream>
#include <filesystem>
#include <chrono>
#include "../utils/LoggingSystem/LogMacros.hpp"

namespace mementodb {
namespace memory {

/**
 * WALBase - WAL 基础类模板
 * 
 * 提供公共的 WAL 功能：
 * - 状态持久化/加载
 * - 目录管理
 * - 时间戳获取
 * 
 * @tparam LogRecordType 日志记录类型（由子类定义）
 */
template<typename LogRecordType>
class WALBase {
protected:
    std::string data_dir_;
    uint64_t next_lsn_{1};
    
    WALBase(const std::string& data_dir) : data_dir_(data_dir) {}
    virtual ~WALBase() = default;
    
    // ==================== 公共工具方法 ====================
    
    /**
     * ensure_directory - 确保目录存在
     */
    static void ensure_directory(const std::string& path) {
        std::filesystem::create_directories(path);
    }
    
    /**
     * get_current_time - 获取当前时间戳（毫秒）
     */
    static uint64_t get_current_time() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
    
    /**
     * get_wal_state_path - 获取 WAL 状态文件路径
     */
    std::string get_wal_state_path() const {
        return data_dir_ + "/wal_state.bin";
    }
    
    /**
     * load_wal_state_base - 加载 WAL 基础状态（next_lsn）
     * 子类可以重写以加载额外状态
     */
    virtual void load_wal_state_base() {
        std::string state_path = get_wal_state_path();
        std::ifstream file(state_path, std::ios::binary);
        
        if (!file.is_open()) {
            LOG_INFO("WAL", "WAL state file not found, starting fresh: " + state_path);
            return;
        }
        
        uint64_t next_lsn;
        file.read(reinterpret_cast<char*>(&next_lsn), sizeof(next_lsn));
        next_lsn_ = next_lsn;
        
        LOG_INFO("WAL", "Loaded WAL state: NextLSN=" + std::to_string(next_lsn) +
                        ", File=" + state_path);
    }
    
    /**
     * persist_wal_state_base - 持久化 WAL 基础状态（next_lsn）
     * 子类可以重写以持久化额外状态
     */
    virtual void persist_wal_state_base() {
        std::string state_path = get_wal_state_path();
        std::ofstream file(state_path, std::ios::binary);
        
        if (!file.is_open()) {
            LOG_ERROR("WAL", "Failed to open WAL state file for writing: " + state_path);
            return;
        }
        
        uint64_t next_lsn = next_lsn_;
        file.write(reinterpret_cast<const char*>(&next_lsn), sizeof(next_lsn));
        
        LOG_DEBUG("WAL", "Persisted WAL state: NextLSN=" + std::to_string(next_lsn) +
                         ", File=" + state_path);
    }
    
    /**
     * get_next_lsn - 获取下一个 LSN 并递增
     */
    uint64_t get_next_lsn() {
        return next_lsn_++;
    }
    
    /**
     * set_next_lsn - 设置下一个 LSN（用于恢复）
     */
    void set_next_lsn(uint64_t lsn) {
        next_lsn_ = lsn;
    }
    
    /**
     * get_data_dir - 获取数据目录
     */
    const std::string& get_data_dir() const {
        return data_dir_;
    }
};

} // namespace memory
} // namespace mementodb

