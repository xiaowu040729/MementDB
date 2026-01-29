#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <cstdint>
#include <variant>
#include <optional>
#include <system_error>

namespace mementodb {
namespace net {

// 协议版本
enum class ProtocolVersion {
    RESP2,     // Redis RESP2协议
    RESP3,     // Redis RESP3协议
    CUSTOM,    // 自定义协议
};

// RESP数据类型
enum class RespType : char {
    SIMPLE_STRING = '+',    // 简单字符串
    ERROR = '-',            // 错误
    INTEGER = ':',          // 整数
    BULK_STRING = '$',      // 批量字符串
    ARRAY = '*',            // 数组
    BOOLEAN = '#',          // RESP3: 布尔值
    DOUBLE = ',',           // RESP3: 双精度浮点数
    BIG_NUMBER = '(',       // RESP3: 大数
    BULK_ERROR = '!',       // RESP3: 批量错误
    VERBATIM_STRING = '=',  // RESP3: 原样字符串
    MAP = '%',              // RESP3: 映射
    SET = '~',              // RESP3: 集合
    PUSH = '>',             // RESP3: 推送
    ATTRIBUTE = '|',        // RESP3: 属性
};

// RESP值类型
using RespValue = std::variant<
    std::monostate,         // 空值
    int64_t,                // 整数
    double,                 // 双精度浮点数
    bool,                   // 布尔值
    std::string,            // 字符串
    std::vector<char>       // 二进制数据
    // 注意：嵌套值使用 std::vector<RespValue> 而不是 unique_ptr
>;

// 解析结果
struct ParsedCommand {
    std::string command;                     // 命令名（小写）
    std::vector<RespValue> args;             // 参数列表
    ProtocolVersion version;                 // 协议版本
    uint64_t request_id;                     // 请求ID（用于追踪）
    bool pipeline = false;                   // 是否管道请求
    bool transaction = false;                // 是否事务命令
    std::optional<std::string> client_info;  // 客户端信息
    
    // 辅助方法
    bool has_arg(size_t index) const { return index < args.size(); }
    
    template<typename T>
    std::optional<T> get_arg(size_t index) const {
        if (index >= args.size()) return std::nullopt;
        try {
            return std::get<T>(args[index]);
        } catch (const std::bad_variant_access&) {
            return std::nullopt;
        }
    }
    
    std::string arg_as_string(size_t index, const std::string& default_val = "") const {
        auto val = get_arg<std::string>(index);
        return val ? *val : default_val;
    }
    
    int64_t arg_as_int(size_t index, int64_t default_val = 0) const {
        auto val = get_arg<int64_t>(index);
        return val ? *val : default_val;
    }
};

// 解析状态
struct ParseState {
    size_t bytes_processed = 0;              // 已处理的字节数
    size_t bytes_remaining = 0;              // 剩余需要字节数
    bool need_more_data = false;             // 是否需要更多数据
    std::string partial_data;                // 部分数据（用于流式解析）
    ProtocolVersion detected_version;        // 检测到的协议版本
    std::error_code error;                   // 错误码
    std::string error_message;               // 错误信息
};

// 协议配置
struct ProtocolConfig {
    ProtocolVersion version = ProtocolVersion::RESP2;
    bool enable_resp3 = false;               // 是否启用RESP3
    bool enable_pipeline = true;             // 是否启用管道
    bool enable_transaction = true;          // 是否启用事务
    size_t max_request_size = 1024 * 1024;   // 最大请求大小（1MB）
    size_t max_array_size = 1024;            // 最大数组大小
    size_t max_bulk_size = 512 * 1024;       // 最大批量字符串大小（512KB）
    bool strict_mode = false;                // 严格模式（检查协议合规性）
    bool auto_lowercase = true;              // 自动将命令转为小写
};

/**
 * Protocol - 协议解析器（支持Redis RESP2/RESP3协议）
 * 
 * 支持Redis RESP (REdis Serialization Protocol) 协议
 * 支持流式解析、管道、事务等高级特性
 */
class Protocol {
public:
    using CommandCallback = std::function<void(const ParsedCommand&)>;
    using ErrorCallback = std::function<void(const std::error_code&, const std::string&)>;
    
    Protocol(const ProtocolConfig& config = {});
    ~Protocol();
    
    // 禁止拷贝
    Protocol(const Protocol&) = delete;
    Protocol& operator=(const Protocol&) = delete;
    
    // 允许移动
    Protocol(Protocol&&) noexcept;
    Protocol& operator=(Protocol&&) noexcept;
    
    /**
     * 流式解析：解析可能不完整的数据
     * @param data 接收到的数据
     * @param state 解析状态（输入输出参数）
     * @param callback 解析完成的回调
     * @return 是否成功开始解析
     */
    bool parse_stream(const char* data, size_t length, 
                      ParseState& state, CommandCallback callback);
    
    /**
     * 解析完整请求（传统方法）
     * @param data 完整请求数据
     * @param parsed 解析结果输出
     * @return 解析状态
     */
    ParseState parse_request(const std::string& data, ParsedCommand& parsed);
    
    /**
     * 解析管道请求（多个命令）
     * @param data 请求数据
     * @param commands 解析结果输出
     * @return 解析状态
     */
    ParseState parse_pipeline(const std::string& data, 
                              std::vector<ParsedCommand>& commands);
    
    /**
     * 重置解析状态
     * @param state 要重置的状态
     */
    void reset_state(ParseState& state);
    
    // 响应构建方法（完整集合）
    
    /**
     * 构建简单字符串响应
     */
    static std::string build_simple_string(const std::string& str);
    
    /**
     * 构建错误响应
     */
    static std::string build_error(const std::string& error_type, 
                                   const std::string& message);
    
    /**
     * 构建整数响应
     */
    static std::string build_integer(int64_t value);
    
    /**
     * 构建批量字符串响应
     */
    static std::string build_bulk_string(const std::string& str);
    static std::string build_bulk_string(const char* data, size_t length);
    
    /**
     * 构建空批量字符串响应
     */
    static std::string build_null_bulk_string();
    
    /**
     * 构建数组响应
     */
    static std::string build_array(const std::vector<std::string>& elements);
    static std::string build_array(size_t count);
    
    /**
     * 构建空数组响应
     */
    static std::string build_empty_array();
    
    // RESP3扩展方法
    static std::string build_boolean(bool value);
    static std::string build_double(double value);
    static std::string build_big_number(const std::string& number);
    static std::string build_bulk_error(const std::string& error_type,
                                        const std::string& message);
    static std::string build_verbatim_string(const std::string& format,
                                             const std::string& str);
    
    /**
     * 构建属性响应（RESP3）
     */
    static std::string build_attribute(const std::string& key, 
                                       const std::string& value);
    
    /**
     * 构建映射响应（RESP3）
     */
    static std::string build_map(const std::unordered_map<std::string, std::string>& map);
    
    /**
     * 构建集合响应（RESP3）
     */
    static std::string build_set(const std::vector<std::string>& elements);
    
    /**
     * 构建推送响应（RESP3）
     */
    static std::string build_push(const std::string& type,
                                  const std::vector<std::string>& data);
    
    /**
     * 构建协议错误响应（根据配置的协议版本）
     */
    std::string build_protocol_error(const std::string& message) const;
    
    /**
     * 构建命令错误响应
     */
    std::string build_command_error(const std::string& command,
                                    const std::string& message) const;
    
    /**
     * 构建语法错误响应
     */
    static std::string build_syntax_error(const std::string& message = "syntax error");
    
    /**
     * 构建类型错误响应
     */
    static std::string build_type_error(const std::string& message = "wrong type");
    
    /**
     * 构建参数数量错误响应
     */
    static std::string build_arg_count_error(const std::string& command);
    
    /**
     * 批量构建响应（用于管道）
     */
    static std::string build_batch_responses(const std::vector<std::string>& responses);
    
    /**
     * 获取协议配置
     */
    const ProtocolConfig& config() const { return config_; }
    
    /**
     * 更新协议配置
     */
    void update_config(const ProtocolConfig& config);
    
    /**
     * 检查是否支持指定协议特性
     */
    bool supports(ProtocolVersion version) const;
    bool supports_pipeline() const { return config_.enable_pipeline; }
    bool supports_transaction() const { return config_.enable_transaction; }
    
    /**
     * 验证命令参数
     */
    struct ValidationResult {
        bool valid;
        std::string error_message;
        std::error_code error_code;
    };
    
    ValidationResult validate_command(const ParsedCommand& cmd) const;
    
    /**
     * 注册自定义命令处理器
     */
    using CommandValidator = std::function<ValidationResult(const ParsedCommand&)>;
    void register_command_validator(const std::string& command, CommandValidator validator);
    
    /**
     * 获取协议统计信息
     */
    struct Statistics {
        uint64_t requests_parsed = 0;
        uint64_t bytes_parsed = 0;
        uint64_t commands_processed = 0;
        uint64_t pipeline_requests = 0;
        uint64_t parse_errors = 0;
        uint64_t protocol_errors = 0;
    };
    
    Statistics get_statistics() const;
    
    /**
     * 重置统计信息
     */
    void reset_statistics();
    
    /**
     * 设置调试回调
     */
    using DebugCallback = std::function<void(const std::string&)>;
    void set_debug_callback(DebugCallback callback);
    
    /**
     * 协议协商（用于客户端握手）
     */
    static std::string negotiate_protocol(ProtocolVersion client_version,
                                          ProtocolVersion server_version);
    
    /**
     * 协议升级（从RESP2升级到RESP3）
     */
    static bool upgrade_to_resp3(std::string& handshake_response);
    
    // 向后兼容的静态方法（简化接口）
    static bool parse_request_static(const std::string& data, ParsedCommand& parsed);
    static std::string build_response(const std::string& result);
    static std::string build_error_static(const std::string& error);
    static std::string build_null();

private:
    // 内部解析状态
    class ParserImpl;
    std::unique_ptr<ParserImpl> impl_;
    
    ProtocolConfig config_;
    Statistics stats_;
    
    // 命令验证器注册表
    std::unordered_map<std::string, CommandValidator> command_validators_;
    
    // 调试回调
    DebugCallback debug_callback_;
    
    // 私有方法
    bool parse_single_request(const char* data, size_t length,
                              size_t& pos, ParsedCommand& parsed,
                              ParseState& state);
    
    bool parse_resp_type(RespType type, const char* data, size_t length,
                         size_t& pos, RespValue& value, ParseState& state);
    
    bool parse_bulk_string(const char* data, size_t length,
                           size_t& pos, RespValue& value, ParseState& state);
    
    bool parse_array(const char* data, size_t length,
                     size_t& pos, RespValue& value, ParseState& state);
    
    bool parse_integer(const char* data, size_t length,
                       size_t& pos, RespValue& value, ParseState& state);
    
    bool parse_simple_string(const char* data, size_t length,
                             size_t& pos, RespValue& value, ParseState& state);
    
    bool parse_error(const char* data, size_t length,
                     size_t& pos, RespValue& value, ParseState& state);
    
    // RESP3解析方法
    bool parse_boolean(const char* data, size_t length,
                       size_t& pos, RespValue& value, ParseState& state);
    
    bool parse_double(const char* data, size_t length,
                      size_t& pos, RespValue& value, ParseState& state);
    
    bool parse_big_number(const char* data, size_t length,
                          size_t& pos, RespValue& value, ParseState& state);
    
    bool parse_verbatim_string(const char* data, size_t length,
                               size_t& pos, RespValue& value, ParseState& state);
    
    // 验证方法
    bool validate_request_size(size_t size) const;
    bool validate_array_size(size_t size) const;
    bool validate_bulk_size(size_t size) const;
    
    // 辅助方法
    void log_debug(const std::string& message) const;
    void update_statistics(const ParseState& state);
    
    // 构建辅助方法
    static std::string build_length_prefixed(char type, const std::string& content);
    static std::string build_length_prefixed(char type, int64_t length);
    static std::string escape_string(const std::string& str);
};

// 协议工具函数
namespace protocol_utils {
    
    /**
     * 检测协议版本
     */
    ProtocolVersion detect_protocol_version(const std::string& data);
    
    /**
     * 提取客户端信息
     */
    std::optional<std::string> extract_client_info(const std::string& data);
    
    /**
     * 判断是否为管道请求
     */
    bool is_pipeline_request(const std::string& data);
    
    /**
     * 判断是否为事务命令
     */
    bool is_transaction_command(const ParsedCommand& cmd);
    
    /**
     * 解析命令行的字符串到RESP格式
     */
    std::string command_line_to_resp(const std::vector<std::string>& args);
    
    /**
     * RESP格式到可读字符串的转换
     */
    std::string resp_to_readable_string(const std::string& resp);
    
    /**
     * 压缩RESP响应（去除不必要的空格和换行）
     */
    std::string compress_resp(const std::string& resp, bool keep_newline = true);
    
    /**
     * 计算RESP消息的预期长度
     */
    std::optional<size_t> calculate_resp_length(const std::string& data, size_t start_pos = 0);
    
} // namespace protocol_utils

} // namespace net
} // namespace mementodb

#endif // PROTOCOL_H
