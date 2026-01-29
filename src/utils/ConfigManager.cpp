// File: src/utils/ConfigManager.cpp

#include "ConfigManager.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <regex>

#ifdef _WIN32
    #include <windows.h>
#else
    extern char** environ;
#endif

namespace mementodb {
namespace utils {

// ==================== ConfigSchema 实现 ====================

ValidationResult ConfigSchema::validate(const std::map<std::string, ConfigValue>& config) const {
    // 验证必需字段
    for (const auto& field : fields) {
        if (field.required) {
            auto it = config.find(field.name);
            if (it == config.end()) {
                return ValidationResult{
                    false,
                    "必需字段缺失: " + field.name,
                    std::make_error_code(std::errc::invalid_argument)
                };
            }
            
            // 验证类型：使用 type_index 比较，不要拿 variant 的 index 和 hash_code 混淆
            std::type_index actual_type = std::visit([](const auto& v) {
                return std::type_index(typeid(v));
            }, it->second);
            if (actual_type != field.type) {
                return ValidationResult{
                    false,
                    "字段类型不匹配: " + field.name,
                    std::make_error_code(std::errc::invalid_argument)
                };
            }
            
            // 自定义验证
            if (field.validator) {
                auto result = field.validator(it->second);
                if (!result) {
                    return result;
                }
            }
        }
    }
    
    // 全局验证
    if (global_validator) {
        return global_validator(config);
    }
    
    return ValidationResult{true, "", {}};
}

// ==================== ConfigParser 实现 ====================

// KeyValueParser
class ConfigManager::KeyValueParser : public ConfigManager::ConfigParser {
public:
    bool parse(const std::string& content, std::map<std::string, ConfigValue>& result) override {
        std::istringstream iss(content);
        std::string line;
        
        while (std::getline(iss, line)) {
            // 移除前后空白
            line.erase(0, line.find_first_not_of(" \t\r\n"));
            line.erase(line.find_last_not_of(" \t\r\n") + 1);
            
            // 跳过空行和注释
            if (line.empty() || line[0] == '#' || line[0] == ';') {
                continue;
            }
            
            // 查找等号
            size_t eq_pos = line.find('=');
            if (eq_pos == std::string::npos) {
                continue;
            }
            
            std::string key = line.substr(0, eq_pos);
            std::string value = line.substr(eq_pos + 1);
            
            // 移除空白
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);
            
            // 移除引号
            if ((value.front() == '"' && value.back() == '"') ||
                (value.front() == '\'' && value.back() == '\'')) {
                value = value.substr(1, value.length() - 2);
            }
            
            if (!key.empty()) {
                result[key] = parse_value(value);
            }
        }
        
        return true;
    }
    
    std::string serialize(const std::map<std::string, ConfigValue>& config) override {
        std::ostringstream oss;
        for (const auto& pair : config) {
            oss << pair.first << " = " << value_to_string(pair.second) << "\n";
        }
        return oss.str();
    }
    
private:
    ConfigValue parse_value(const std::string& str) {
        // 尝试解析为布尔值
        std::string lower = str;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower == "true" || lower == "1" || lower == "yes" || lower == "on") {
            return ConfigValue(true);
        }
        if (lower == "false" || lower == "0" || lower == "no" || lower == "off") {
            return ConfigValue(false);
        }
        
        // 尝试解析为数字
        try {
            if (str.find('.') != std::string::npos) {
                return ConfigValue(std::stod(str));
            } else {
                return ConfigValue(static_cast<int64_t>(std::stoll(str)));
            }
        } catch (...) {
            // 默认为字符串
            return ConfigValue(str);
        }
    }
    
    std::string value_to_string(const ConfigValue& value) {
        return std::visit([](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>) {
                return v ? "true" : "false";
            } else if constexpr (std::is_arithmetic_v<T>) {
                return std::to_string(v);
            } else if constexpr (std::is_same_v<T, std::string>) {
                return "\"" + v + "\"";
            } else {
                return "";
            }
        }, value);
    }
};

// JsonParser (简化版)
class ConfigManager::JsonParser : public ConfigManager::ConfigParser {
public:
    bool parse(const std::string& content, std::map<std::string, ConfigValue>& result) override {
        // 简化的JSON解析（仅支持简单的键值对）
        std::regex pair_regex("\"([^\"]+)\"\\s*:\\s*\"([^\"]+)\"|\"([^\"]+)\"\\s*:\\s*(\\d+\\.?\\d*)|\"([^\"]+)\"\\s*:\\s*(true|false)");
        std::sregex_iterator iter(content.begin(), content.end(), pair_regex);
        std::sregex_iterator end;
        
        for (; iter != end; ++iter) {
            std::smatch match = *iter;
            std::string key = match[1].str() + match[3].str() + match[5].str();
            std::string value_str = match[2].str() + match[4].str() + match[6].str();
            
            if (!key.empty() && !value_str.empty()) {
                result[key] = parse_json_value(value_str);
            }
        }
        
        return true;
    }
    
    std::string serialize(const std::map<std::string, ConfigValue>& config) override {
        std::ostringstream oss;
        oss << "{\n";
        bool first = true;
        for (const auto& pair : config) {
            if (!first) oss << ",\n";
            first = false;
            oss << "  \"" << pair.first << "\": " << value_to_json(pair.second);
        }
        oss << "\n}\n";
        return oss.str();
    }
    
private:
    ConfigValue parse_json_value(const std::string& str) {
        if (str == "true") return ConfigValue(true);
        if (str == "false") return ConfigValue(false);
        try {
            if (str.find('.') != std::string::npos) {
                return ConfigValue(std::stod(str));
            } else {
                return ConfigValue(static_cast<int64_t>(std::stoll(str)));
            }
        } catch (...) {
            return ConfigValue(str);
        }
    }
    
    std::string value_to_json(const ConfigValue& value) {
        return std::visit([](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>) {
                return v ? "true" : "false";
            } else if constexpr (std::is_arithmetic_v<T>) {
                return std::to_string(v);
            } else if constexpr (std::is_same_v<T, std::string>) {
                return "\"" + v + "\"";
            } else {
                return "null";
            }
        }, value);
    }
};

// IniParser
class ConfigManager::IniParser : public ConfigManager::ConfigParser {
public:
    bool parse(const std::string& content, std::map<std::string, ConfigValue>& result) override {
        std::istringstream iss(content);
        std::string line;
        std::string current_section;
        
        while (std::getline(iss, line)) {
            line.erase(0, line.find_first_not_of(" \t"));
            line.erase(line.find_last_not_of(" \t\r\n") + 1);
            
            if (line.empty() || line[0] == '#' || line[0] == ';') {
                continue;
            }
            
            // 处理节
            if (line[0] == '[' && line.back() == ']') {
                current_section = line.substr(1, line.length() - 2);
                continue;
            }
            
            // 处理键值对
            size_t eq_pos = line.find('=');
            if (eq_pos != std::string::npos) {
                std::string key = line.substr(0, eq_pos);
                std::string value = line.substr(eq_pos + 1);
                
                key.erase(0, key.find_first_not_of(" \t"));
                key.erase(key.find_last_not_of(" \t") + 1);
                value.erase(0, value.find_first_not_of(" \t"));
                value.erase(value.find_last_not_of(" \t") + 1);
                
                if (!current_section.empty()) {
                    key = current_section + "." + key;
                }
                
                if (!key.empty()) {
                    result[key] = ConfigValue(value);
                }
            }
        }
        
        return true;
    }
    
    std::string serialize(const std::map<std::string, ConfigValue>& config) override {
        std::map<std::string, std::map<std::string, ConfigValue>> sections;
        
        for (const auto& pair : config) {
            size_t dot_pos = pair.first.find('.');
            if (dot_pos != std::string::npos) {
                std::string section = pair.first.substr(0, dot_pos);
                std::string key = pair.first.substr(dot_pos + 1);
                sections[section][key] = pair.second;
            } else {
                sections[""][pair.first] = pair.second;
            }
        }
        
        std::ostringstream oss;
        for (const auto& section_pair : sections) {
            if (!section_pair.first.empty()) {
                oss << "[" << section_pair.first << "]\n";
            }
            for (const auto& kv : section_pair.second) {
                std::string value_str = std::visit([](const auto& v) -> std::string {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, std::string>) {
                        return v;
                    } else if constexpr (std::is_arithmetic_v<T>) {
                        return std::to_string(v);
                    } else if constexpr (std::is_same_v<T, bool>) {
                        return v ? "true" : "false";
                    } else {
                        return "";
                    }
                }, kv.second);
                oss << kv.first << " = " << value_str << "\n";
            }
            oss << "\n";
        }
        
        return oss.str();
    }
};

// EnvVarsParser
class ConfigManager::EnvVarsParser : public ConfigManager::ConfigParser {
public:
    bool parse(const std::string& content, std::map<std::string, ConfigValue>& result) override {
        // 不支持从字符串解析环境变量，由load_from_env处理
        return false;
    }
    
    std::string serialize(const std::map<std::string, ConfigValue>& config) override {
        std::ostringstream oss;
        for (const auto& pair : config) {
            std::string env_key = pair.first;
            std::transform(env_key.begin(), env_key.end(), env_key.begin(), ::toupper);
            std::replace(env_key.begin(), env_key.end(), '.', '_');
            std::string value_str = std::visit([](const auto& v) -> std::string {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::string>) {
                    return v;
                } else if constexpr (std::is_arithmetic_v<T>) {
                    return std::to_string(v);
                } else if constexpr (std::is_same_v<T, bool>) {
                    return v ? "true" : "false";
                } else {
                    return "";
                }
            }, pair.second);
            oss << env_key << "=" << value_str << "\n";
        }
        return oss.str();
    }
};

// CommandLineParser
class ConfigManager::CommandLineParser : public ConfigManager::ConfigParser {
public:
    bool parse(const std::string& content, std::map<std::string, ConfigValue>& result) override {
        // 不支持从字符串解析命令行
        return false;
    }
    
    std::string serialize(const std::map<std::string, ConfigValue>& config) override {
        std::ostringstream oss;
        for (const auto& pair : config) {
            std::string value_str = std::visit([](const auto& v) -> std::string {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::string>) {
                    return v;
                } else if constexpr (std::is_arithmetic_v<T>) {
                    return std::to_string(v);
                } else if constexpr (std::is_same_v<T, bool>) {
                    return v ? "true" : "false";
                } else {
                    return "";
                }
            }, pair.second);
            oss << "--" << pair.first << "=" << value_str << " ";
        }
        return oss.str();
    }
};

// ==================== ConfigManager::Builder 实现 ====================

ConfigManager::Builder::Builder() = default;

ConfigManager::Builder& ConfigManager::Builder::add_file(const std::string& filepath, ConfigFormat format) {
    ConfigSource source;
    source.format = format;
    source.source = filepath;
    source.priority = static_cast<int>(sources_.size());
    source.watch_for_changes = watch_enabled_;
    source.watch_interval = watch_interval_;
    sources_.push_back(source);
    return *this;
}

ConfigManager::Builder& ConfigManager::Builder::add_env_vars(const std::string& prefix) {
    ConfigSource source;
    source.format = ConfigFormat::ENV_VARS;
    source.source = prefix;
    source.priority = static_cast<int>(sources_.size());
    source.watch_for_changes = false;
    sources_.push_back(source);
    return *this;
}

ConfigManager::Builder& ConfigManager::Builder::add_command_line(int argc, char* argv[], const std::string& prefix) {
    ConfigSource source;
    source.format = ConfigFormat::COMMAND_LINE;
    source.source = prefix;
    source.priority = static_cast<int>(sources_.size());
    source.watch_for_changes = false;
    // 存储命令行参数（简化处理）
    sources_.push_back(source);
    return *this;
}

ConfigManager::Builder& ConfigManager::Builder::add_source(const ConfigSource& source) {
    sources_.push_back(source);
    return *this;
}

ConfigManager::Builder& ConfigManager::Builder::set_default(const std::string& key, const ConfigValue& value) {
    defaults_[key] = value;
    return *this;
}

ConfigManager::Builder& ConfigManager::Builder::set_schema(const ConfigSchema& schema) {
    schema_ = schema;
    return *this;
}

ConfigManager::Builder& ConfigManager::Builder::set_environment(const std::string& env) {
    environment_ = env;
    return *this;
}

ConfigManager::Builder& ConfigManager::Builder::set_watch_enabled(bool enabled) {
    watch_enabled_ = enabled;
    return *this;
}

ConfigManager::Builder& ConfigManager::Builder::set_watch_interval(std::chrono::milliseconds interval) {
    watch_interval_ = interval;
    return *this;
}

ConfigManager::Builder& ConfigManager::Builder::on_error(ErrorCallback callback) {
    error_callback_ = callback;
    return *this;
}

std::shared_ptr<ConfigManager> ConfigManager::Builder::build() {
    auto manager = std::shared_ptr<ConfigManager>(new ConfigManager());
    
    manager->sources_ = sources_;
    manager->defaults_ = defaults_;
    manager->schema_ = schema_;
    manager->environment_ = environment_;
    manager->error_callback_ = error_callback_;
    
    manager->init_parsers();
    
    // 按优先级排序
    std::sort(manager->sources_.begin(), manager->sources_.end(),
              [](const ConfigSource& a, const ConfigSource& b) {
                  return a.priority < b.priority;
              });
    
    return manager;
}

// ==================== ConfigManager 静态方法 ====================

ConfigManager::Builder ConfigManager::create() {
    return Builder();
}

std::shared_ptr<ConfigManager> ConfigManager::load_default() {
    return Builder()
        .add_env_vars("MEMENTODB_")
        .add_file("config.json", ConfigFormat::JSON)
        .add_file("config.conf", ConfigFormat::KEY_VALUE)
        .build();
}

// ==================== ConfigManager 实现 ====================

ConfigManager::~ConfigManager() {
    stop_watching();
}

void ConfigManager::init_parsers() {
    parsers_[ConfigFormat::KEY_VALUE] = std::make_unique<KeyValueParser>();
    parsers_[ConfigFormat::JSON] = std::make_unique<JsonParser>();
    parsers_[ConfigFormat::INI] = std::make_unique<IniParser>();
    parsers_[ConfigFormat::ENV_VARS] = std::make_unique<EnvVarsParser>();
    parsers_[ConfigFormat::COMMAND_LINE] = std::make_unique<CommandLineParser>();
}

bool ConfigManager::load() {
    std::unique_lock lock(config_mutex_);
    
    config_map_.clear();
    source_info_.clear();
    
    // 先加载默认值
    config_map_ = defaults_;
    
    // 按优先级加载各个来源
    for (const auto& source : sources_) {
        SourceInfo info;
        info.source = source;
        info.load_time = std::chrono::system_clock::now();
        
        if (load_source(source)) {
            info.loaded = true;
            stats_.loaded_sources++;
        } else {
            info.loaded = false;
            info.error = std::make_error_code(std::errc::io_error);
            stats_.failed_sources++;
            if (error_callback_) {
                error_callback_("加载配置源失败: " + source.source, info.error);
            }
        }
        
        source_info_.push_back(info);
    }
    
    // 验证配置
    if (schema_) {
        auto result = validate();
        if (!result) {
            report_error("配置验证失败: " + result.message, result.error_code);
            return false;
        }
    }
    
    stats_.total_keys = config_map_.size();
    stats_.last_reload = std::chrono::system_clock::now();
    
    return true;
}

bool ConfigManager::reload() {
    stats_.reload_count++;
    return load();
}

bool ConfigManager::load_source(const ConfigSource& source) {
    switch (source.format) {
        case ConfigFormat::ENV_VARS:
            return load_from_env(source.source) > 0;
            
        case ConfigFormat::KEY_VALUE:
        case ConfigFormat::JSON:
        case ConfigFormat::INI: {
            std::ifstream file(source.source);
            if (!file.is_open()) {
                return false;
            }
            
            std::string content((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
            file.close();
            
            std::map<std::string, ConfigValue> parsed;
            if (parse_content(content, source.format, parsed)) {
                // 合并到主配置
                for (const auto& pair : parsed) {
                    config_map_[pair.first] = pair.second;
                }
                
                // 记录文件监视信息
                if (source.watch_for_changes) {
                    FileWatchInfo watch_info;
                    watch_info.path = source.source;
                    watch_info.format = source.format;
                    try {
                        watch_info.last_write_time = std::filesystem::last_write_time(source.source);
                    } catch (...) {
                        // 忽略错误
                    }
                    watched_files_.push_back(watch_info);
                }
                
                return true;
            }
            return false;
        }
        
        default:
            return false;
    }
}

bool ConfigManager::parse_content(const std::string& content, ConfigFormat format,
                                  std::map<std::string, ConfigValue>& result) {
    auto it = parsers_.find(format);
    if (it == parsers_.end()) {
        return false;
    }
    
    return it->second->parse(content, result);
}

size_t ConfigManager::load_from_env(const std::string& prefix) {
    size_t count = 0;
    
#ifdef _WIN32
    LPCH env_strings = GetEnvironmentStrings();
    if (env_strings != nullptr) {
        LPCH env = env_strings;
        while (*env != '\0') {
            std::string env_str(env);
            size_t eq_pos = env_str.find('=');
            if (eq_pos != std::string::npos) {
                std::string key = env_str.substr(0, eq_pos);
                std::string value = env_str.substr(eq_pos + 1);
                
                if (key.length() >= prefix.length() && 
                    key.substr(0, prefix.length()) == prefix) {
                    std::string config_key = key.substr(prefix.length());
                    std::transform(config_key.begin(), config_key.end(), config_key.begin(), ::tolower);
                    std::replace(config_key.begin(), config_key.end(), '_', '.');
                    
                    config_map_[config_key] = ConfigValue(value);
                    count++;
                }
            }
            env += env_str.length() + 1;
        }
        FreeEnvironmentStrings(env_strings);
    }
#else
    if (environ != nullptr) {
        for (char** env = environ; *env != nullptr; ++env) {
            std::string env_str(*env);
            size_t eq_pos = env_str.find('=');
            if (eq_pos == std::string::npos) {
                continue;
            }
            
            std::string key = env_str.substr(0, eq_pos);
            std::string value = env_str.substr(eq_pos + 1);
            
            if (key.length() >= prefix.length() && 
                key.substr(0, prefix.length()) == prefix) {
                std::string config_key = key.substr(prefix.length());
                std::transform(config_key.begin(), config_key.end(), config_key.begin(), ::tolower);
                std::replace(config_key.begin(), config_key.end(), '_', '.');
                
                config_map_[config_key] = ConfigValue(value);
                count++;
            }
        }
    }
#endif
    
    return count;
}

bool ConfigManager::load_from_command_line(int argc, char* argv[], const std::string& prefix) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.length() > prefix.length() && arg.substr(0, prefix.length()) == prefix) {
            std::string key_value = arg.substr(prefix.length());
            size_t eq_pos = key_value.find('=');
            if (eq_pos != std::string::npos) {
                std::string key = key_value.substr(0, eq_pos);
                std::string value = key_value.substr(eq_pos + 1);
                std::replace(key.begin(), key.end(), '-', '.');
                config_map_[key] = ConfigValue(value);
            }
        }
    }
    return true;
}

std::optional<ConfigValue> ConfigManager::get_value(const std::string& key) const {
    std::shared_lock lock(config_mutex_);
    
    auto it = config_map_.find(key);
    if (it != config_map_.end()) {
        return it->second;
    }
    
    auto default_it = defaults_.find(key);
    if (default_it != defaults_.end()) {
        return default_it->second;
    }
    
    return std::nullopt;
}

std::string ConfigManager::get_string(const std::string& key, const std::string& default_value) const {
    auto value = get_value(key);
    if (value && std::holds_alternative<std::string>(*value)) {
        return std::get<std::string>(*value);
    }
    return default_value;
}

bool ConfigManager::has(const std::string& key) const {
    std::shared_lock lock(config_mutex_);
    return config_map_.find(key) != config_map_.end() || 
           defaults_.find(key) != defaults_.end();
}

bool ConfigManager::remove(const std::string& key) {
    std::unique_lock lock(config_mutex_);
    auto it = config_map_.find(key);
    if (it != config_map_.end()) {
        ConfigValue old_value = it->second;
        config_map_.erase(it);
        notify_watchers(key, old_value, ConfigValue(std::monostate{}));
        stats_.total_keys = config_map_.size();
        return true;
    }
    return false;
}

std::vector<std::string> ConfigManager::keys() const {
    std::shared_lock lock(config_mutex_);
    std::vector<std::string> result;
    result.reserve(config_map_.size());
    for (const auto& pair : config_map_) {
        result.push_back(pair.first);
    }
    return result;
}

std::map<std::string, ConfigValue> ConfigManager::get_subset(const std::string& prefix) const {
    std::shared_lock lock(config_mutex_);
    std::map<std::string, ConfigValue> result;
    
    for (const auto& pair : config_map_) {
        if (pair.first.length() >= prefix.length() && 
            pair.first.substr(0, prefix.length()) == prefix) {
            std::string suffix = pair.first.substr(prefix.length());
            if (!suffix.empty() && suffix[0] == '.') {
                suffix = suffix.substr(1);
            }
            result[suffix] = pair.second;
        }
    }
    
    return result;
}

ValidationResult ConfigManager::validate() const {
    if (schema_) {
        return schema_->validate(config_map_);
    }
    return ValidationResult{true, "", {}};
}

bool ConfigManager::export_to_file(const std::string& filepath, ConfigFormat format) const {
    std::shared_lock lock(config_mutex_);
    
    auto it = parsers_.find(format);
    if (it == parsers_.end()) {
        return false;
    }
    
    std::string content = it->second->serialize(config_map_);
    
    std::ofstream file(filepath);
    if (!file.is_open()) {
        return false;
    }
    
    file << content;
    file.close();
    
    return true;
}

std::string ConfigManager::export_to_string(ConfigFormat format) const {
    std::shared_lock lock(config_mutex_);
    
    auto it = parsers_.find(format);
    if (it == parsers_.end()) {
        return "";
    }
    
    return it->second->serialize(config_map_);
}

ConfigManager::WatchHandle ConfigManager::watch(const std::string& key_pattern, ConfigCallback callback) {
    std::unique_lock lock(config_mutex_);
    
    Watcher watcher;
    watcher.id = next_watch_id_++;
    watcher.pattern = std::regex(key_pattern);
    watcher.callback = callback;
    
    watchers_.push_back(watcher);
    stats_.watch_callbacks++;
    
    return watcher.id;
}

bool ConfigManager::unwatch(WatchHandle handle) {
    std::unique_lock lock(config_mutex_);
    
    auto it = std::remove_if(watchers_.begin(), watchers_.end(),
                             [handle](const Watcher& w) { return w.id == handle; });
    
    if (it != watchers_.end()) {
        watchers_.erase(it, watchers_.end());
        return true;
    }
    
    return false;
}

bool ConfigManager::start_watching() {
    if (watching_.exchange(true)) {
        return false; // 已经在监视
    }
    
    watch_thread_ = std::thread(&ConfigManager::watch_loop, this);
    return true;
}

void ConfigManager::stop_watching() {
    if (watching_.exchange(false)) {
        if (watch_thread_.joinable()) {
            watch_thread_.join();
        }
    }
}

void ConfigManager::watch_loop() {
    while (watching_) {
        check_for_changes();
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

void ConfigManager::check_for_changes() {
    std::unique_lock lock(config_mutex_);
    
    for (auto& watch_info : watched_files_) {
        try {
            if (!std::filesystem::exists(watch_info.path)) {
                continue;
            }
            
            auto current_time = std::filesystem::last_write_time(watch_info.path);
            if (current_time != watch_info.last_write_time) {
                watch_info.last_write_time = current_time;
                
                // 重新加载文件
                std::map<std::string, ConfigValue> old_config = config_map_;
                ConfigSource source;
                source.format = watch_info.format;
                source.source = watch_info.path;
                load_source(source);
                
                // 通知变化
                for (const auto& pair : config_map_) {
                    auto old_it = old_config.find(pair.first);
                    if (old_it == old_config.end() || old_it->second != pair.second) {
                        notify_watchers(pair.first, 
                                       old_it != old_config.end() ? old_it->second : ConfigValue(std::monostate{}),
                                       pair.second);
                    }
                }
            }
        } catch (...) {
            // 忽略错误
        }
    }
}

void ConfigManager::notify_watchers(const std::string& key, 
                                   const ConfigValue& old_value,
                                   const ConfigValue& new_value) {
    for (const auto& watcher : watchers_) {
        if (std::regex_match(key, watcher.pattern)) {
            watcher.callback(key, old_value, new_value);
        }
    }
}

std::vector<ConfigManager::SourceInfo> ConfigManager::get_source_info() const {
    std::shared_lock lock(config_mutex_);
    return source_info_;
}

ConfigManager::Statistics ConfigManager::get_statistics() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

std::vector<std::string> ConfigManager::find_keys(const std::string& pattern) const {
    std::shared_lock lock(config_mutex_);
    std::vector<std::string> result;
    
    std::regex regex_pattern(pattern);
    for (const auto& pair : config_map_) {
        if (std::regex_match(pair.first, regex_pattern)) {
            result.push_back(pair.first);
        }
    }
    
    return result;
}

void ConfigManager::merge(const ConfigManager& other, bool overwrite) {
    std::unique_lock lock(config_mutex_);
    std::shared_lock other_lock(other.config_mutex_);
    
    for (const auto& pair : other.config_map_) {
        if (overwrite || config_map_.find(pair.first) == config_map_.end()) {
            config_map_[pair.first] = pair.second;
        }
    }
    
    stats_.total_keys = config_map_.size();
}

void ConfigManager::clear() {
    std::unique_lock lock(config_mutex_);
    config_map_.clear();
    stats_.total_keys = 0;
}

void ConfigManager::normalize_key(std::string& key) const {
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    key.erase(0, key.find_first_not_of(" \t"));
    key.erase(key.find_last_not_of(" \t") + 1);
}

std::vector<std::string> ConfigManager::split_key(const std::string& key) const {
    std::vector<std::string> parts;
    std::istringstream iss(key);
    std::string part;
    
    while (std::getline(iss, part, '.')) {
        parts.push_back(part);
    }
    
    return parts;
}

ValidationResult ConfigManager::validate_field(const ConfigSchema::Field& field, 
                                               const ConfigValue& value) const {
    if (field.validator) {
        return field.validator(value);
    }
    return ValidationResult{true, "", {}};
}

void ConfigManager::report_error(const std::string& message, const std::error_code& ec) {
    if (error_callback_) {
        error_callback_(message, ec);
    }
}

// ==================== 类型转换特化 ====================

template<>
std::optional<bool> ConfigManager::convert_value<bool>(const ConfigValue& value) const {
    if (auto p = std::get_if<bool>(&value)) return *p;
    if (auto p = std::get_if<std::string>(&value)) {
        std::string lower = *p;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        return (lower == "true" || lower == "1" || lower == "yes" || lower == "on");
    }
    return std::nullopt;
}

template<>
std::optional<uint32_t> ConfigManager::convert_value<uint32_t>(const ConfigValue& value) const {
    if (auto p = std::get_if<uint32_t>(&value)) return *p;
    if (auto p = std::get_if<uint64_t>(&value)) return static_cast<uint32_t>(*p);
    if (auto p = std::get_if<int32_t>(&value))  return *p >= 0 ? std::optional<uint32_t>(static_cast<uint32_t>(*p)) : std::nullopt;
    if (auto p = std::get_if<int64_t>(&value))  return *p >= 0 ? std::optional<uint32_t>(static_cast<uint32_t>(*p)) : std::nullopt;
    if (auto p = std::get_if<std::string>(&value)) {
        try {
            auto v = std::stoul(*p);
            return static_cast<uint32_t>(v);
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

template<>
std::optional<uint64_t> ConfigManager::convert_value<uint64_t>(const ConfigValue& value) const {
    if (auto p = std::get_if<uint64_t>(&value)) return *p;
    if (auto p = std::get_if<uint32_t>(&value)) return static_cast<uint64_t>(*p);
    if (auto p = std::get_if<int64_t>(&value))  return *p >= 0 ? std::optional<uint64_t>(static_cast<uint64_t>(*p)) : std::nullopt;
    if (auto p = std::get_if<int32_t>(&value))  return *p >= 0 ? std::optional<uint64_t>(static_cast<uint64_t>(*p)) : std::nullopt;
    if (auto p = std::get_if<std::string>(&value)) {
        try {
            return std::stoull(*p);
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

template<>
std::optional<float> ConfigManager::convert_value<float>(const ConfigValue& value) const {
    if (auto p = std::get_if<float>(&value)) return *p;
    if (auto p = std::get_if<double>(&value)) return static_cast<float>(*p);
    if (auto p = std::get_if<std::string>(&value)) {
        try {
            return std::stof(*p);
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

template<>
std::optional<int64_t> ConfigManager::convert_value<int64_t>(const ConfigValue& value) const {
    if (auto p = std::get_if<int64_t>(&value)) return *p;
    if (auto p = std::get_if<int32_t>(&value)) return static_cast<int64_t>(*p);
    if (auto p = std::get_if<std::string>(&value)) {
        try {
            return std::stoll(*p);
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

template<>
std::optional<double> ConfigManager::convert_value<double>(const ConfigValue& value) const {
    if (auto p = std::get_if<double>(&value)) return *p;
    if (auto p = std::get_if<float>(&value)) return static_cast<double>(*p);
    if (auto p = std::get_if<std::string>(&value)) {
        try {
            return std::stod(*p);
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

template<>
std::optional<std::string> ConfigManager::convert_value<std::string>(const ConfigValue& value) const {
    if (auto p = std::get_if<std::string>(&value)) return *p;
    // 尝试转换为字符串
    std::ostringstream oss;
    std::visit([&oss](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_arithmetic_v<T>) {
            oss << v;
        } else if constexpr (std::is_same_v<T, bool>) {
            oss << (v ? "true" : "false");
        }
    }, value);
    return oss.str();
}

template<>
ConfigValue ConfigManager::to_config_value<bool>(const bool& value) const {
    return ConfigValue(value);
}

template<>
ConfigValue ConfigManager::to_config_value<uint32_t>(const uint32_t& value) const {
    return ConfigValue(value);
}

template<>
ConfigValue ConfigManager::to_config_value<uint64_t>(const uint64_t& value) const {
    return ConfigValue(value);
}

template<>
ConfigValue ConfigManager::to_config_value<float>(const float& value) const {
    return ConfigValue(value);
}

template<>
ConfigValue ConfigManager::to_config_value<int64_t>(const int64_t& value) const {
    return ConfigValue(value);
}

template<>
ConfigValue ConfigManager::to_config_value<double>(const double& value) const {
    return ConfigValue(value);
}

template<>
ConfigValue ConfigManager::to_config_value<std::string>(const std::string& value) const {
    return ConfigValue(value);
}

// ==================== ConfigFileWatcher 实现 ====================

ConfigFileWatcher::ConfigFileWatcher() = default;

ConfigFileWatcher::~ConfigFileWatcher() {
    stop();
}

bool ConfigFileWatcher::add_watch(const std::filesystem::path& path, 
                                  std::chrono::milliseconds interval) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    WatchEntry entry;
    entry.path = path;
    entry.interval = interval;
    
    try {
        if (std::filesystem::exists(path)) {
            entry.last_write_time = std::filesystem::last_write_time(path);
            entry.exists = true;
        } else {
            entry.exists = false;
        }
    } catch (...) {
        return false;
    }
    
    watches_.push_back(entry);
    return true;
}

bool ConfigFileWatcher::remove_watch(const std::filesystem::path& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = std::remove_if(watches_.begin(), watches_.end(),
                             [&path](const WatchEntry& e) { return e.path == path; });
    
    if (it != watches_.end()) {
        watches_.erase(it, watches_.end());
        return true;
    }
    
    return false;
}

void ConfigFileWatcher::start() {
    if (running_.exchange(true)) {
        return; // 已经在运行
    }
    
    watch_thread_ = std::thread(&ConfigFileWatcher::watch_loop, this);
}

void ConfigFileWatcher::stop() {
    if (running_.exchange(false)) {
        if (watch_thread_.joinable()) {
            watch_thread_.join();
        }
    }
}

void ConfigFileWatcher::set_callback(EventCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = callback;
}

void ConfigFileWatcher::watch_loop() {
    while (running_) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            
            for (auto& entry : watches_) {
                try {
                    bool exists = std::filesystem::exists(entry.path);
                    
                    if (!entry.exists && exists) {
                        // 文件创建
                        entry.exists = true;
                        entry.last_write_time = std::filesystem::last_write_time(entry.path);
                        
                        if (callback_) {
                            FileEvent event;
                            event.type = FileEvent::Type::CREATED;
                            event.path = entry.path;
                            event.time = std::chrono::system_clock::now();
                            callback_(event);
                        }
                    } else if (entry.exists && !exists) {
                        // 文件删除
                        entry.exists = false;
                        
                        if (callback_) {
                            FileEvent event;
                            event.type = FileEvent::Type::DELETED;
                            event.path = entry.path;
                            event.time = std::chrono::system_clock::now();
                            callback_(event);
                        }
                    } else if (entry.exists && exists) {
                        // 检查修改时间
                        auto current_time = std::filesystem::last_write_time(entry.path);
                        if (current_time != entry.last_write_time) {
                            entry.last_write_time = current_time;
                            
                            if (callback_) {
                                FileEvent event;
                                event.type = FileEvent::Type::MODIFIED;
                                event.path = entry.path;
                                event.time = std::chrono::system_clock::now();
                                callback_(event);
                            }
                        }
                    }
                } catch (...) {
                    // 忽略错误
                }
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

// ==================== config_utils 实现 ====================

namespace config_utils {

std::string get_env(const std::string& name, const std::string& default_value) {
    const char* value = std::getenv(name.c_str());
    return value ? std::string(value) : default_value;
}

std::map<std::string, std::string> get_env_with_prefix(const std::string& prefix) {
    std::map<std::string, std::string> result;
    
#ifdef _WIN32
    LPCH env_strings = GetEnvironmentStrings();
    if (env_strings != nullptr) {
        LPCH env = env_strings;
        while (*env != '\0') {
            std::string env_str(env);
            size_t eq_pos = env_str.find('=');
            if (eq_pos != std::string::npos) {
                std::string key = env_str.substr(0, eq_pos);
                std::string value = env_str.substr(eq_pos + 1);
                
                if (key.length() >= prefix.length() && 
                    key.substr(0, prefix.length()) == prefix) {
                    result[key] = value;
                }
            }
            env += env_str.length() + 1;
        }
        FreeEnvironmentStrings(env_strings);
    }
#else
    if (environ != nullptr) {
        for (char** env = environ; *env != nullptr; ++env) {
            std::string env_str(*env);
            size_t eq_pos = env_str.find('=');
            if (eq_pos != std::string::npos) {
                std::string key = env_str.substr(0, eq_pos);
                std::string value = env_str.substr(eq_pos + 1);
                
                if (key.length() >= prefix.length() && 
                    key.substr(0, prefix.length()) == prefix) {
                    result[key] = value;
                }
            }
        }
    }
#endif
    
    return result;
}

std::map<std::string, std::string> parse_command_line(int argc, char* argv[], 
                                                      const std::vector<CommandLineOption>& options) {
    std::map<std::string, std::string> result;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.length() > 2 && arg.substr(0, 2) == "--") {
            std::string key_value = arg.substr(2);
            size_t eq_pos = key_value.find('=');
            
            if (eq_pos != std::string::npos) {
                std::string key = key_value.substr(0, eq_pos);
                std::string value = key_value.substr(eq_pos + 1);
                result[key] = value;
            } else {
                // 检查是否有值
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    result[key_value] = argv[++i];
                } else {
                    result[key_value] = "true";
                }
            }
        }
    }
    
    // 填充默认值
    for (const auto& option : options) {
        if (result.find(option.name) == result.end() && !option.default_value.empty()) {
            result[option.name] = option.default_value;
        }
    }
    
    return result;
}

void merge_configs(std::map<std::string, ConfigValue>& target,
                  const std::map<std::string, ConfigValue>& source,
                  bool overwrite) {
    for (const auto& pair : source) {
        if (overwrite || target.find(pair.first) == target.end()) {
            target[pair.first] = pair.second;
        }
    }
}

ValidationResult validate_range(int64_t value, int64_t min, int64_t max) {
    if (value < min || value > max) {
        return ValidationResult{
            false,
            "值 " + std::to_string(value) + " 超出范围 [" + 
            std::to_string(min) + ", " + std::to_string(max) + "]",
            std::make_error_code(std::errc::result_out_of_range)
        };
    }
    return ValidationResult{true, "", {}};
}

ValidationResult validate_regex(const std::string& value, const std::string& pattern) {
    std::regex regex_pattern(pattern);
    if (!std::regex_match(value, regex_pattern)) {
        return ValidationResult{
            false,
            "值 \"" + value + "\" 不匹配模式 \"" + pattern + "\"",
            std::make_error_code(std::errc::invalid_argument)
        };
    }
    return ValidationResult{true, "", {}};
}

ValidationResult validate_one_of(const ConfigValue& value, 
                                const std::vector<ConfigValue>& allowed_values) {
    for (const auto& allowed : allowed_values) {
        if (value == allowed) {
            return ValidationResult{true, "", {}};
        }
    }
    
    return ValidationResult{
        false,
        "值不在允许的列表中",
        std::make_error_code(std::errc::invalid_argument)
    };
}

std::string expand_template(const std::string& template_str,
                           const std::map<std::string, std::string>& variables) {
    std::string result = template_str;
    
    for (const auto& pair : variables) {
        std::string placeholder = "${" + pair.first + "}";
        size_t pos = 0;
        while ((pos = result.find(placeholder, pos)) != std::string::npos) {
            result.replace(pos, placeholder.length(), pair.second);
            pos += pair.second.length();
        }
    }
    
    return result;
}

std::string encrypt_value(const std::string& value, const std::string& key) {
    // 简单的XOR加密（实际应用中应使用更安全的加密方法）
    std::string encrypted = value;
    for (size_t i = 0; i < encrypted.length(); ++i) {
        encrypted[i] ^= key[i % key.length()];
    }
    return encrypted;
}

std::string decrypt_value(const std::string& encrypted_value, const std::string& key) {
    // XOR解密（对称加密）
    return encrypt_value(encrypted_value, key);
}

} // namespace config_utils

} // namespace utils
} // namespace mementodb
