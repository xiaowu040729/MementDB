// File: src/net/Protocol.cpp

#include "Protocol.hpp"
#include "../utils/LoggingSystem/LogMacros.hpp"
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cctype>

namespace mementodb {
namespace net {

// ParserImpl 实现
class Protocol::ParserImpl {
public:
    ParseState state;
    ProtocolConfig config;
    
    ParserImpl(const ProtocolConfig& cfg) : config(cfg) {
        state.detected_version = cfg.version;
    }
};

Protocol::Protocol(const ProtocolConfig& config)
    : impl_(std::make_unique<ParserImpl>(config)), config_(config) {
}

Protocol::~Protocol() = default;

Protocol::Protocol(Protocol&&) noexcept = default;
Protocol& Protocol::operator=(Protocol&&) noexcept = default;

bool Protocol::parse_stream(const char* data, size_t length,
                            ParseState& state, CommandCallback callback) {
    // TODO: 实现流式解析
    (void)data;
    (void)length;
    (void)state;
    (void)callback;
    return false;
}

ParseState Protocol::parse_request(const std::string& data, ParsedCommand& parsed) {
    ParseState state;
    state.detected_version = config_.version;
    
    if (data.empty()) {
        state.error = std::make_error_code(std::errc::invalid_argument);
        state.error_message = "Empty request";
        return state;
    }
    
    size_t pos = 0;
    if (!parse_single_request(data.data(), data.length(), pos, parsed, state)) {
        return state;
    }
    
    state.bytes_processed = pos;
    stats_.requests_parsed++;
    stats_.bytes_parsed += pos;
    
    return state;
}

ParseState Protocol::parse_pipeline(const std::string& data,
                                    std::vector<ParsedCommand>& commands) {
    ParseState state;
    state.detected_version = config_.version;
    
    // TODO: 实现管道解析
    (void)data;
    (void)commands;
    return state;
}

void Protocol::reset_state(ParseState& state) {
    state.bytes_processed = 0;
    state.bytes_remaining = 0;
    state.need_more_data = false;
    state.partial_data.clear();
    state.error.clear();
    state.error_message.clear();
}

std::string Protocol::build_simple_string(const std::string& str) {
    return "+" + str + "\r\n";
}

std::string Protocol::build_error(const std::string& error_type,
                                  const std::string& message) {
    return "-" + error_type + " " + message + "\r\n";
}

std::string Protocol::build_integer(int64_t value) {
    return ":" + std::to_string(value) + "\r\n";
}

std::string Protocol::build_bulk_string(const std::string& str) {
    return build_bulk_string(str.data(), str.length());
}

std::string Protocol::build_bulk_string(const char* data, size_t length) {
    return "$" + std::to_string(length) + "\r\n" +
           std::string(data, length) + "\r\n";
}

std::string Protocol::build_null_bulk_string() {
    return "$-1\r\n";
}

std::string Protocol::build_array(const std::vector<std::string>& elements) {
    std::string result = "*" + std::to_string(elements.size()) + "\r\n";
    for (const auto& elem : elements) {
        result += build_bulk_string(elem);
    }
    return result;
}

std::string Protocol::build_array(size_t count) {
    return "*" + std::to_string(count) + "\r\n";
}

std::string Protocol::build_empty_array() {
    return "*0\r\n";
}

std::string Protocol::build_boolean(bool value) {
    return value ? "#t\r\n" : "#f\r\n";
}

std::string Protocol::build_double(double value) {
    std::ostringstream oss;
    oss.precision(17);
    oss << value;
    return "," + oss.str() + "\r\n";
}

std::string Protocol::build_big_number(const std::string& number) {
    return "(" + number + "\r\n";
}

std::string Protocol::build_bulk_error(const std::string& error_type,
                                        const std::string& message) {
    std::string content = error_type + " " + message;
    return "!" + std::to_string(content.length()) + "\r\n" + content + "\r\n";
}

std::string Protocol::build_verbatim_string(const std::string& format,
                                             const std::string& str) {
    return "=" + std::to_string(format.length() + 1 + str.length()) + "\r\n" +
           format + ":" + str + "\r\n";
}

std::string Protocol::build_attribute(const std::string& key,
                                      const std::string& value) {
    return "|2\r\n" + build_bulk_string(key) + build_bulk_string(value);
}

std::string Protocol::build_map(const std::unordered_map<std::string, std::string>& map) {
    std::string result = "%" + std::to_string(map.size()) + "\r\n";
    for (const auto& pair : map) {
        result += build_bulk_string(pair.first);
        result += build_bulk_string(pair.second);
    }
    return result;
}

std::string Protocol::build_set(const std::vector<std::string>& elements) {
    std::string result = "~" + std::to_string(elements.size()) + "\r\n";
    for (const auto& elem : elements) {
        result += build_bulk_string(elem);
    }
    return result;
}

std::string Protocol::build_push(const std::string& type,
                                  const std::vector<std::string>& data) {
    std::string result = ">" + std::to_string(data.size() + 1) + "\r\n";
    result += build_bulk_string(type);
    for (const auto& elem : data) {
        result += build_bulk_string(elem);
    }
    return result;
}

std::string Protocol::build_protocol_error(const std::string& message) const {
    return build_error("PROTOERR", message);
}

std::string Protocol::build_command_error(const std::string& command,
                                          const std::string& message) const {
    return build_error("ERR", "unknown command '" + command + "': " + message);
}

std::string Protocol::build_syntax_error(const std::string& message) {
    return build_error("ERR", message);
}

std::string Protocol::build_type_error(const std::string& message) {
    return build_error("WRONGTYPE", message);
}

std::string Protocol::build_arg_count_error(const std::string& command) {
    return build_error("ERR", "wrong number of arguments for '" + command + "' command");
}

std::string Protocol::build_batch_responses(const std::vector<std::string>& responses) {
    std::string result;
    for (const auto& resp : responses) {
        result += resp;
    }
    return result;
}

void Protocol::update_config(const ProtocolConfig& config) {
    config_ = config;
    if (impl_) {
        impl_->config = config;
    }
}

bool Protocol::supports(ProtocolVersion version) const {
    if (version == ProtocolVersion::RESP2) return true;
    if (version == ProtocolVersion::RESP3) return config_.enable_resp3;
    return false;
}

Protocol::ValidationResult Protocol::validate_command(const ParsedCommand& cmd) const {
    ValidationResult result;
    result.valid = true;
    
    if (cmd.command.empty()) {
        result.valid = false;
        result.error_message = "Empty command";
        result.error_code = std::make_error_code(std::errc::invalid_argument);
        return result;
    }
    
    auto it = command_validators_.find(cmd.command);
    if (it != command_validators_.end()) {
        return it->second(cmd);
    }
    
    return result;
}

void Protocol::register_command_validator(const std::string& command,
                                          CommandValidator validator) {
    command_validators_[command] = std::move(validator);
}

Protocol::Statistics Protocol::get_statistics() const {
    return stats_;
}

void Protocol::reset_statistics() {
    stats_ = Statistics();
}

void Protocol::set_debug_callback(DebugCallback callback) {
    debug_callback_ = std::move(callback);
}

std::string Protocol::negotiate_protocol(ProtocolVersion client_version,
                                         ProtocolVersion server_version) {
    (void)client_version;
    (void)server_version;
    // TODO: 实现协议协商
    return "+OK\r\n";
}

bool Protocol::upgrade_to_resp3(std::string& handshake_response) {
    (void)handshake_response;
    // TODO: 实现协议升级
    return false;
}

// 向后兼容的静态方法
bool Protocol::parse_request_static(const std::string& data, ParsedCommand& parsed) {
    ProtocolConfig config;
    Protocol proto(config);
    ParseState state = proto.parse_request(data, parsed);
    return state.error.value() == 0 && !state.need_more_data;
}

std::string Protocol::build_response(const std::string& result) {
    return build_simple_string(result);
}

std::string Protocol::build_error_static(const std::string& error) {
    return build_error("ERR", error);
}

std::string Protocol::build_null() {
    return build_null_bulk_string();
}

// 私有方法实现
bool Protocol::parse_single_request(const char* data, size_t length,
                                     size_t& pos, ParsedCommand& parsed,
                                     ParseState& state) {
    if (pos >= length) {
        state.need_more_data = true;
        return false;
    }
    
    if (data[pos] != '*') {
        state.error = std::make_error_code(std::errc::protocol_error);
        state.error_message = "Expected array";
        return false;
    }
    
    pos++; // 跳过 '*'
    
    int64_t array_size = 0;
    RespValue array_size_value;
    if (!parse_integer(data, length, pos, array_size_value, state)) {
        return false;
    }
    array_size = std::get<int64_t>(array_size_value);
    
    if (array_size < 0 || static_cast<size_t>(array_size) > config_.max_array_size) {
        state.error = std::make_error_code(std::errc::invalid_argument);
        state.error_message = "Invalid array size";
        return false;
    }
    
    if (array_size == 0) {
        parsed.command = "";
        return true;
    }
    
    // 解析命令名
    RespValue cmd_value;
    if (!parse_bulk_string(data, length, pos, cmd_value, state)) {
        return false;
    }
    
    if (auto* str = std::get_if<std::string>(&cmd_value)) {
        parsed.command = *str;
        if (config_.auto_lowercase) {
            std::transform(parsed.command.begin(), parsed.command.end(),
                          parsed.command.begin(), ::tolower);
        }
    }
    
    // 解析参数
    parsed.args.reserve(array_size - 1);
    for (int64_t i = 1; i < array_size; ++i) {
        RespValue arg_value;
        if (!parse_bulk_string(data, length, pos, arg_value, state)) {
            return false;
        }
        parsed.args.push_back(std::move(arg_value));
    }
    
    parsed.version = state.detected_version;
    return true;
}

bool Protocol::parse_resp_type(RespType type, const char* data, size_t length,
                               size_t& pos, RespValue& value, ParseState& state) {
    switch (type) {
        case RespType::BULK_STRING:
            return parse_bulk_string(data, length, pos, value, state);
        case RespType::INTEGER:
            return parse_integer(data, length, pos, value, state);
        case RespType::SIMPLE_STRING:
            return parse_simple_string(data, length, pos, value, state);
        case RespType::ERROR:
            return parse_error(data, length, pos, value, state);
        case RespType::ARRAY:
            return parse_array(data, length, pos, value, state);
        default:
            state.error = std::make_error_code(std::errc::not_supported);
            state.error_message = "Unsupported RESP type";
            return false;
    }
}

bool Protocol::parse_bulk_string(const char* data, size_t length,
                                  size_t& pos, RespValue& value, ParseState& state) {
    if (pos >= length || data[pos] != '$') {
        state.error = std::make_error_code(std::errc::protocol_error);
        state.error_message = "Expected bulk string";
        return false;
    }
    
    pos++; // 跳过 '$'
    
    int64_t str_len = 0;
    RespValue str_len_value;
    if (!parse_integer(data, length, pos, str_len_value, state)) {
        return false;
    }
    str_len = std::get<int64_t>(str_len_value);
    
    if (str_len == -1) {
        value = std::monostate{};
        return true;
    }
    
    if (str_len < 0 || static_cast<size_t>(str_len) > config_.max_bulk_size) {
        state.error = std::make_error_code(std::errc::invalid_argument);
        state.error_message = "Invalid bulk string length";
        return false;
    }
    
    if (pos + 2 + str_len > length) {
        state.need_more_data = true;
        state.bytes_remaining = pos + 2 + str_len - length;
        return false;
    }
    
    std::string str(data + pos, str_len);
    pos += str_len;
    
    if (pos + 2 > length || data[pos] != '\r' || data[pos + 1] != '\n') {
        state.error = std::make_error_code(std::errc::protocol_error);
        state.error_message = "Missing CRLF";
        return false;
    }
    
    pos += 2;
    value = std::move(str);
    return true;
}

bool Protocol::parse_array(const char* data, size_t length,
                          size_t& pos, RespValue& value, ParseState& state) {
    // TODO: 实现数组解析
    (void)data;
    (void)length;
    (void)pos;
    (void)value;
    (void)state;
    return false;
}

// 辅助函数：解析整数到 int64_t（在命名空间内，供 Protocol 类使用）
namespace {
bool parse_integer_impl(const char* data, size_t length,
                        size_t& pos, int64_t& result, ParseState& state) {
    if (pos >= length) {
        state.need_more_data = true;
        return false;
    }
    
    bool negative = false;
    if (data[pos] == '-') {
        negative = true;
        pos++;
    }
    
    int64_t num = 0;
    bool has_digit = false;
    
    while (pos < length && std::isdigit(data[pos])) {
        num = num * 10 + (data[pos] - '0');
        has_digit = true;
        pos++;
    }
    
    if (!has_digit) {
        state.error = std::make_error_code(std::errc::protocol_error);
        state.error_message = "Invalid integer";
        return false;
    }
    
    if (pos + 2 > length || data[pos] != '\r' || data[pos + 1] != '\n') {
        state.error = std::make_error_code(std::errc::protocol_error);
        state.error_message = "Missing CRLF";
        return false;
    }
    
    pos += 2;
    result = negative ? -num : num;
    return true;
}
} // anonymous namespace

bool Protocol::parse_integer(const char* data, size_t length,
                            size_t& pos, RespValue& value, ParseState& state) {
    int64_t result = 0;
    if (!parse_integer_impl(data, length, pos, result, state)) {
        return false;
    }
    value = result;
    return true;
}

bool Protocol::parse_simple_string(const char* data, size_t length,
                                   size_t& pos, RespValue& value, ParseState& state) {
    if (pos >= length || data[pos] != '+') {
        state.error = std::make_error_code(std::errc::protocol_error);
        state.error_message = "Expected simple string";
        return false;
    }
    
    pos++; // 跳过 '+'
    
    size_t start = pos;
    while (pos < length && data[pos] != '\r') {
        pos++;
    }
    
    if (pos + 1 >= length || data[pos] != '\r' || data[pos + 1] != '\n') {
        state.error = std::make_error_code(std::errc::protocol_error);
        state.error_message = "Missing CRLF";
        return false;
    }
    
    std::string str(data + start, pos - start);
    pos += 2;
    value = std::move(str);
    return true;
}

bool Protocol::parse_error(const char* data, size_t length,
                           size_t& pos, RespValue& value, ParseState& state) {
    if (pos >= length || data[pos] != '-') {
        state.error = std::make_error_code(std::errc::protocol_error);
        state.error_message = "Expected error";
        return false;
    }
    
    pos++; // 跳过 '-'
    
    size_t start = pos;
    while (pos < length && data[pos] != '\r') {
        pos++;
    }
    
    if (pos + 1 >= length || data[pos] != '\r' || data[pos + 1] != '\n') {
        state.error = std::make_error_code(std::errc::protocol_error);
        state.error_message = "Missing CRLF";
        return false;
    }
    
    std::string str(data + start, pos - start);
    pos += 2;
    value = std::move(str);
    return true;
}

bool Protocol::parse_boolean(const char* data, size_t length,
                             size_t& pos, RespValue& value, ParseState& state) {
    (void)data;
    (void)length;
    (void)pos;
    (void)value;
    (void)state;
    // TODO: 实现布尔值解析
    return false;
}

bool Protocol::parse_double(const char* data, size_t length,
                            size_t& pos, RespValue& value, ParseState& state) {
    (void)data;
    (void)length;
    (void)pos;
    (void)value;
    (void)state;
    // TODO: 实现双精度浮点数解析
    return false;
}

bool Protocol::parse_big_number(const char* data, size_t length,
                                 size_t& pos, RespValue& value, ParseState& state) {
    (void)data;
    (void)length;
    (void)pos;
    (void)value;
    (void)state;
    // TODO: 实现大数解析
    return false;
}

bool Protocol::parse_verbatim_string(const char* data, size_t length,
                                     size_t& pos, RespValue& value, ParseState& state) {
    (void)data;
    (void)length;
    (void)pos;
    (void)value;
    (void)state;
    // TODO: 实现原样字符串解析
    return false;
}

bool Protocol::validate_request_size(size_t size) const {
    return size <= config_.max_request_size;
}

bool Protocol::validate_array_size(size_t size) const {
    return size <= config_.max_array_size;
}

bool Protocol::validate_bulk_size(size_t size) const {
    return size <= config_.max_bulk_size;
}

void Protocol::log_debug(const std::string& message) const {
    if (debug_callback_) {
        debug_callback_(message);
    }
}

void Protocol::update_statistics(const ParseState& state) {
    stats_.bytes_parsed += state.bytes_processed;
    if (state.error) {
        stats_.parse_errors++;
    }
}

std::string Protocol::build_length_prefixed(char type, const std::string& content) {
    return std::string(1, type) + std::to_string(content.length()) + "\r\n" + content + "\r\n";
}

std::string Protocol::build_length_prefixed(char type, int64_t length) {
    return std::string(1, type) + std::to_string(length) + "\r\n";
}

std::string Protocol::escape_string(const std::string& str) {
    std::string result;
    for (char c : str) {
        if (c == '\r') {
            result += "\\r";
        } else if (c == '\n') {
            result += "\\n";
        } else if (c == '\t') {
            result += "\\t";
        } else {
            result += c;
        }
    }
    return result;
}

// protocol_utils 实现
namespace protocol_utils {

ProtocolVersion detect_protocol_version(const std::string& data) {
    // TODO: 实现协议版本检测
    (void)data;
    return ProtocolVersion::RESP2;
}

std::optional<std::string> extract_client_info(const std::string& data) {
    // TODO: 实现客户端信息提取
    (void)data;
    return std::nullopt;
}

bool is_pipeline_request(const std::string& data) {
    // TODO: 实现管道请求检测
    (void)data;
    return false;
}

bool is_transaction_command(const ParsedCommand& cmd) {
    return cmd.command == "multi" || cmd.command == "exec" ||
           cmd.command == "discard" || cmd.transaction;
}

std::string command_line_to_resp(const std::vector<std::string>& args) {
    return Protocol::build_array(args);
}

std::string resp_to_readable_string(const std::string& resp) {
    // TODO: 实现RESP到可读字符串转换
    return resp;
}

std::string compress_resp(const std::string& resp, bool keep_newline) {
    // TODO: 实现RESP压缩
    (void)keep_newline;
    return resp;
}

std::optional<size_t> calculate_resp_length(const std::string& data, size_t start_pos) {
    // TODO: 实现RESP长度计算
    (void)data;
    (void)start_pos;
    return std::nullopt;
}

} // namespace protocol_utils

} // namespace net
} // namespace mementodb
