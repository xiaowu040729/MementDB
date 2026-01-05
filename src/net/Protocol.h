#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <string>
#include <vector>
#include <memory>

namespace mementodb {
namespace net {

/**
 * Protocol - 协议解析器（Redis协议兼容）
 * 
 * 支持Redis RESP (REdis Serialization Protocol) 协议
 */
class Protocol {
public:
    // 命令类型
    enum class CommandType {
        GET,
        SET,
        DEL,
        EXISTS,
        PING,
        INFO,
        UNKNOWN
    };
    
    // 解析结果
    struct ParsedCommand {
        CommandType type;
        std::vector<std::string> args;
    };
    
    /**
     * 解析客户端请求
     * @param data 接收到的数据
     * @param parsed 解析结果输出
     * @return 是否解析成功
     */
    static bool parse_request(const std::string& data, ParsedCommand& parsed);
    
    /**
     * 构建响应
     * @param result 响应结果
     * @return RESP格式的响应字符串
     */
    static std::string build_response(const std::string& result);
    
    /**
     * 构建错误响应
     * @param error 错误消息
     * @return RESP格式的错误响应字符串
     */
    static std::string build_error(const std::string& error);
    
    /**
     * 构建整数响应
     * @param value 整数值
     * @return RESP格式的整数响应字符串
     */
    static std::string build_integer(int64_t value);
    
    /**
     * 构建空响应
     * @return RESP格式的空响应字符串
     */
    static std::string build_null();

private:
    // 解析RESP数组
    static bool parse_array(const std::string& data, size_t& pos, ParsedCommand& parsed);
    
    // 解析RESP字符串
    static bool parse_string(const std::string& data, size_t& pos, std::string& result);
    
    // 解析RESP整数
    static bool parse_integer(const std::string& data, size_t& pos, int64_t& result);
    
    // 将命令字符串转换为CommandType
    static CommandType string_to_command(const std::string& cmd);
};

} // namespace net
} // namespace mementodb

#endif // PROTOCOL_H

