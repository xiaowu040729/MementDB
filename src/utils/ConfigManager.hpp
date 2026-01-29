#ifndef CONFIG_MANAGER_HPP
#define CONFIG_MANAGER_HPP

#include <string>
#include <unordered_map>
#include <map>
#include <memory>
#include <functional>
#include <vector>
#include <optional>
#include <variant>
#include <atomic>
#include <shared_mutex>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <regex>
#include <algorithm>
#include <system_error>
#include <any>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <iostream>

// 可选：如果支持C++17，可以包含以下头文件
#include <chrono>
#include <thread>

namespace mementodb {
namespace utils {

// 配置值类型
using ConfigValue = std::variant<
    std::monostate,        // 未设置
    bool,                  // 布尔值
    int32_t,               // 32位整数
    int64_t,               // 64位整数
    uint32_t,              // 32位无符号整数
    uint64_t,              // 64位无符号整数
    float,                 // 单精度浮点数
    double,                // 双精度浮点数
    std::string,           // 字符串
    std::vector<std::string>, // 字符串列表
    std::vector<int64_t>,  // 整数列表
    std::vector<double>    // 浮点数列表
>;

// 配置格式
enum class ConfigFormat {
    KEY_VALUE,      // 键值对格式：key=value
    JSON,           // JSON格式
    YAML,           // YAML格式（可选）
    XML,            // XML格式（可选）
    INI,            // INI格式
    ENV_VARS,       // 环境变量
    COMMAND_LINE    // 命令行参数
};

// 配置来源
struct ConfigSource {
    ConfigFormat format;
    std::string source;  // 文件路径、环境变量前缀等
    int priority;        // 加载优先级（数值越小优先级越高）
    bool watch_for_changes; // 是否监视变化
    std::chrono::milliseconds watch_interval; // 监视间隔
};

// 验证结果
struct ValidationResult {
    bool valid;
    std::string message;
    std::error_code error_code;
    
    operator bool() const { return valid; }
};

// 配置模式定义
struct ConfigSchema {
    // 字段定义
    struct Field {
        // 字段名称
        std::string name;
        // 字段类型
        std::type_index type;
        // 字段默认值
        ConfigValue default_value;
        // 是否必需
        bool required;
        // 自定义验证函数
        std::function<ValidationResult(const ConfigValue&)> validator;
        // 字段描述
        std::string description;
        
        template<typename T>
        static Field create(const std::string& name, T default_value, 
                           bool required = false, 
                           std::function<ValidationResult(const ConfigValue&)> validator = nullptr,
                           std::string description = "") {
            return Field{
                name,
                typeid(T),
                ConfigValue(default_value),
                required,
                validator,
                description
            };
        }
    };
    
    // 模式名称
    std::string name;
    // 字段列表
    std::vector<Field> fields;
    // 全局验证函数
    std::function<ValidationResult(const std::map<std::string, ConfigValue>&)> global_validator;
    // 验证配置
    
    ValidationResult validate(const std::map<std::string, ConfigValue>& config) const;
};

/**
 * @brief 高级配置管理器
 * @details 提供统一的配置管理功能，支持多种配置格式和来源
 *          支持热重载、配置验证、类型安全访问等高级特性
 */
class ConfigManager : public std::enable_shared_from_this<ConfigManager> {
public:
    // 配置回调
    using ConfigCallback = std::function<void(const std::string&, const ConfigValue&, const ConfigValue&)>;
    // 错误回调
    using ErrorCallback = std::function<void(const std::string&, const std::error_code&)>;
    
    // 构建器模式，用于创建配置管理器
    class Builder {
    public:
        Builder();
        // 添加文件
        Builder& add_file(const std::string& filepath, ConfigFormat format = ConfigFormat::JSON);
        // 添加环境变量
        Builder& add_env_vars(const std::string& prefix = "MEMENTODB_");
        // 添加命令行参数
        Builder& add_command_line(int argc, char* argv[], const std::string& prefix = "--");
        // 添加配置来源
        Builder& add_source(const ConfigSource& source);
        // 设置默认值
        Builder& set_default(const std::string& key, const ConfigValue& value);
        // 设置模式
        Builder& set_schema(const ConfigSchema& schema);
        // 设置环境
        Builder& set_environment(const std::string& env);
        // 设置监视是否启用
        Builder& set_watch_enabled(bool enabled);
        // 设置监视间隔
        Builder& set_watch_interval(std::chrono::milliseconds interval);
        
        // 设置错误回调
        Builder& on_error(ErrorCallback callback);
        // 构建配置管理器
        std::shared_ptr<ConfigManager> build();
        
    private:
        // 配置来源
        std::vector<ConfigSource> sources_;
        // 默认值
        std::map<std::string, ConfigValue> defaults_;
        // 模式
        std::optional<ConfigSchema> schema_;
        // 环境
        std::string environment_;
        // 监视是否启用
        bool watch_enabled_ = false;
        // 监视间隔
        std::chrono::milliseconds watch_interval_{1000};
        // 错误回调
        ErrorCallback error_callback_;
    };
    
    /**
     * @brief 创建构建器
     */
    static Builder create();
    
    /**
     * @brief 从默认位置加载配置（环境变量 + 默认配置文件）
     */
    static std::shared_ptr<ConfigManager> load_default();
    
    ~ConfigManager();
    
    // 禁止拷贝
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;
    
    // 加载配置
    bool load();
    bool reload();
    
    // 配置访问（类型安全）
    template<typename T>
    std::optional<T> get(const std::string& key) const;
    
    template<typename T>
    T get(const std::string& key, const T& default_value) const;
    
    // 获取配置值（返回ConfigValue）
    std::optional<ConfigValue> get_value(const std::string& key) const;
    
    // 获取配置值（字符串形式）
    std::string get_string(const std::string& key, const std::string& default_value = "") const;
    
    // 检查配置是否存在
    bool has(const std::string& key) const;
    
    // 设置配置值
    template<typename T>
    bool set(const std::string& key, const T& value);
    
    // 删除配置项
    bool remove(const std::string& key);
    
    // 获取所有配置键
    std::vector<std::string> keys() const;
    
    // 获取配置子集
    std::map<std::string, ConfigValue> get_subset(const std::string& prefix) const;
    
    // 获取原始配置映射
    const std::map<std::string, ConfigValue>& all() const { return config_map_; }
    
    // 验证配置
    ValidationResult validate() const;
    
    // 导出配置
    bool export_to_file(const std::string& filepath, ConfigFormat format = ConfigFormat::JSON) const;
    std::string export_to_string(ConfigFormat format = ConfigFormat::JSON) const;
    
    // 监视配置变化
    using WatchHandle = uint64_t;
    WatchHandle watch(const std::string& key_pattern, ConfigCallback callback);
    bool unwatch(WatchHandle handle);
    
    // 启动/停止监视
    bool start_watching();
    void stop_watching();
    
    // 获取配置来源信息
    struct SourceInfo {
        ConfigSource source;
        bool loaded;
        std::chrono::system_clock::time_point load_time;
        std::error_code error;
    };
    
    std::vector<SourceInfo> get_source_info() const;
    
    // 获取统计信息
    struct Statistics {
        size_t total_keys = 0;
        size_t loaded_sources = 0;
        size_t failed_sources = 0;
        uint64_t reload_count = 0;
        uint64_t watch_callbacks = 0;
        std::chrono::system_clock::time_point last_reload;
    };
    
    Statistics get_statistics() const;
    
    // 环境相关
    std::string get_environment() const { return environment_; }
    bool is_development() const { return environment_ == "development"; }
    bool is_testing() const { return environment_ == "testing"; }
    bool is_production() const { return environment_ == "production"; }
    
    // 查找配置键（支持通配符）
    std::vector<std::string> find_keys(const std::string& pattern) const;
    
    // 配置合并
    void merge(const ConfigManager& other, bool overwrite = true);
    
    // 清空配置
    void clear();
    
private:
    ConfigManager() = default;
    
    // 配置解析器接口
    class ConfigParser {
    public:
        virtual ~ConfigParser() = default;
        virtual bool parse(const std::string& content, 
                          std::map<std::string, ConfigValue>& result) = 0;
        virtual std::string serialize(const std::map<std::string, ConfigValue>& config) = 0;
    };
    
    // 特定格式解析器  这里只是前向声明，在 .cpp 文件中定义
    class KeyValueParser;
    class JsonParser;
    class IniParser;
    class EnvVarsParser;
    class CommandLineParser;
    
    // 内部方法
    bool load_source(const ConfigSource& source);
    bool parse_content(const std::string& content, ConfigFormat format, 
                      std::map<std::string, ConfigValue>& result);
    
    void normalize_key(std::string& key) const;
    std::vector<std::string> split_key(const std::string& key) const;
    
    void notify_watchers(const std::string& key, 
                        const ConfigValue& old_value,
                        const ConfigValue& new_value);
    
    void watch_loop();
    void check_for_changes();
    
    // 配置存储（层次化）
    std::map<std::string, ConfigValue> config_map_;
    
    // 配置来源
    std::vector<ConfigSource> sources_;
    std::vector<SourceInfo> source_info_;
    
    // 默认值和模式
    std::map<std::string, ConfigValue> defaults_;
    std::optional<ConfigSchema> schema_;
    
    // 环境
    std::string environment_ = "development";
    
    // 监视相关
    struct Watcher {
        WatchHandle id;
        std::regex pattern;
        ConfigCallback callback;
    };
    
    std::atomic<WatchHandle> next_watch_id_{1};
    std::vector<Watcher> watchers_;
    std::atomic<bool> watching_{false};
    std::thread watch_thread_;
    
    // 解析器缓存
    std::unordered_map<ConfigFormat, std::unique_ptr<ConfigParser>> parsers_;
    
    // 错误处理
    ErrorCallback error_callback_;
    
    // 线程安全
    mutable std::shared_mutex config_mutex_;
    
    // 统计信息
    mutable std::mutex stats_mutex_;
    Statistics stats_;
    
    // 文件系统监视
    struct FileWatchInfo {
        std::string path;
        std::filesystem::file_time_type last_write_time;
        ConfigFormat format;
    };
    
    std::vector<FileWatchInfo> watched_files_;
    
    // 初始化解析器
    void init_parsers();
    
    // 加载环境变量
    size_t load_from_env(const std::string& prefix);
    
    // 加载命令行参数
    bool load_from_command_line(int argc, char* argv[], const std::string& prefix);
    
    // 类型转换辅助
    template<typename T>
    std::optional<T> convert_value(const ConfigValue& value) const;
    
    template<typename T>
    ConfigValue to_config_value(const T& value) const;
    
    // 验证辅助
    ValidationResult validate_field(const ConfigSchema::Field& field, 
                                   const ConfigValue& value) const;
    
    // 错误报告
    void report_error(const std::string& message, const std::error_code& ec = {});
};

// 类型转换特化
template<>
inline std::optional<int32_t> ConfigManager::convert_value<int32_t>(const ConfigValue& value) const {
    if (auto p = std::get_if<int32_t>(&value)) return *p;
    if (auto p = std::get_if<int64_t>(&value)) return static_cast<int32_t>(*p);
    return std::nullopt;
}

template<>
inline ConfigValue ConfigManager::to_config_value<int32_t>(const int32_t& value) const {
    return ConfigValue(value);
}

// 模板方法实现
template<typename T>
std::optional<T> ConfigManager::get(const std::string& key) const {
    std::shared_lock lock(config_mutex_);
    
    // 先查找配置
    auto it = config_map_.find(key);
    if (it != config_map_.end()) {
        return convert_value<T>(it->second);
    }
    
    // 查找默认值
    auto default_it = defaults_.find(key);
    if (default_it != defaults_.end()) {
        return convert_value<T>(default_it->second);
    }
    
    // 尝试分割键进行层次查找
    auto parts = split_key(key);
    if (parts.size() > 1) {
        // 处理层次化键
        // 这里可以支持类似 "server.port" 的键
    }
    
    return std::nullopt;
}

template<typename T>
T ConfigManager::get(const std::string& key, const T& default_value) const {
    auto value = get<T>(key);
    return value ? *value : default_value;
}

template<typename T>
bool ConfigManager::set(const std::string& key, const T& value) {
    // 验证
    if (schema_) {
        // 查找对应的模式字段
        for (const auto& field : schema_->fields) {
            if (field.name == key) {
                auto config_value = to_config_value(value);
                auto result = validate_field(field, config_value);
                if (!result) {
                    report_error("验证失败: " + result.message);
                    return false;
                }
            }
        }
    }
    
    std::unique_lock lock(config_mutex_);
    
    ConfigValue new_value = to_config_value(value);
    ConfigValue old_value = ConfigValue(std::monostate{});
    
    auto it = config_map_.find(key);
    const bool existed = it != config_map_.end();
    if (existed) {
        old_value = it->second;
        it->second = new_value;
    } else {
        config_map_[key] = new_value;
    }
    
    // 通知观察者（新键也会通知，old_value 为 monostate）
    notify_watchers(key, old_value, new_value);
    
    // 更新统计
    stats_.total_keys = config_map_.size();
    
    return true;
}

// 配置文件监视器（独立类）
class ConfigFileWatcher {
public:
    struct FileEvent {
        enum class Type { CREATED, MODIFIED, DELETED } type;
        std::filesystem::path path;
        std::chrono::system_clock::time_point time;
    };
    
    using EventCallback = std::function<void(const FileEvent&)>;
    
    ConfigFileWatcher();
    ~ConfigFileWatcher();
    
    bool add_watch(const std::filesystem::path& path, 
                  std::chrono::milliseconds interval = std::chrono::seconds(1));
    bool remove_watch(const std::filesystem::path& path);
    
    void start();
    void stop();
    
    void set_callback(EventCallback callback);
    
private:
    struct WatchEntry {
        std::filesystem::path path;
        std::filesystem::file_time_type last_write_time;
        std::chrono::milliseconds interval;
        bool exists;
    };
    
    std::vector<WatchEntry> watches_;
    std::atomic<bool> running_{false};
    std::thread watch_thread_;
    EventCallback callback_;
    mutable std::mutex mutex_;
    
    void watch_loop();
};

// 配置工具函数
namespace config_utils {
    
    // 环境变量工具
    std::string get_env(const std::string& name, const std::string& default_value = "");
    std::map<std::string, std::string> get_env_with_prefix(const std::string& prefix);
    
    // 命令行解析工具
    struct CommandLineOption {
        std::string name;
        std::string description;
        bool has_value;
        std::string default_value;
    };
    
    std::map<std::string, std::string> parse_command_line(int argc, char* argv[], 
                                                         const std::vector<CommandLineOption>& options);
    
    // 配置合并工具
    void merge_configs(std::map<std::string, ConfigValue>& target,
                      const std::map<std::string, ConfigValue>& source,
                      bool overwrite = true);
    
    // 配置验证工具
    ValidationResult validate_range(int64_t value, int64_t min, int64_t max);
    ValidationResult validate_regex(const std::string& value, const std::string& pattern);
    ValidationResult validate_one_of(const ConfigValue& value, 
                                    const std::vector<ConfigValue>& allowed_values);
    
    // 配置模板工具
    std::string expand_template(const std::string& template_str,
                               const std::map<std::string, std::string>& variables);
    
    // 配置加密工具（可选）
    std::string encrypt_value(const std::string& value, const std::string& key);
    std::string decrypt_value(const std::string& encrypted_value, const std::string& key);
    
} // namespace config_utils

} // namespace utils
} // namespace mementodb

#endif // CONFIG_MANAGER_HPP
