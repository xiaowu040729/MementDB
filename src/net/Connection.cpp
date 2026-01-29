// File: src/net/Connection.cpp

#include "Connection.hpp"
#include "../core/DiskEngine.hpp"
#include "../utils/LoggingSystem/LogMacros.hpp"
#include "mementodb/Types.h"
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sstream>

namespace mementodb {
namespace net {

namespace {
    std::atomic<uint64_t> g_next_connection_id{1};
}

Connection::Connection(int fd,
                       std::shared_ptr<core::DiskEngineV2> engine,
                       std::weak_ptr<ConnectionManager> manager)
    : fd_(fd),
      connection_id_(g_next_connection_id.fetch_add(1, std::memory_order_relaxed)),
      state_(State::CONNECTED),
      authenticated_(false),
      in_transaction_(false),
      current_db_index_(0),
      engine_(std::move(engine)),
      manager_(std::move(manager)),
      timeout_(std::chrono::seconds(60)),
      last_activity_(std::chrono::steady_clock::now()) {
    stats_.connected_time = std::chrono::steady_clock::now();
}

Connection::~Connection() {
    close(true);
}

bool Connection::on_data_received(const char* data, size_t size) {
    if (!data || size == 0) {
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(read_buffer_mutex_);
        read_buffer_.append(data, size);
    }

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.bytes_received += size;
    }

    update_activity();
    process_buffer();
    check_timeout();

    return !is_closed();
}

std::optional<std::string> Connection::client_address() const {
    // 先检查缓存
    {
        std::lock_guard<std::mutex> lock(address_mutex_);
        if (cached_client_address_.has_value()) {
            return cached_client_address_;
        }
    }
    
    // 从socket获取客户端地址
    struct sockaddr_in addr{};
    socklen_t addr_len = sizeof(addr);
    
    if (::getpeername(fd_, reinterpret_cast<struct sockaddr*>(&addr), &addr_len) == 0) {
        char ip_str[INET_ADDRSTRLEN];
        if (::inet_ntop(AF_INET, &addr.sin_addr, ip_str, INET_ADDRSTRLEN) != nullptr) {
            int port = ntohs(addr.sin_port);
            std::string address = std::string(ip_str) + ":" + std::to_string(port);
            
            // 缓存结果
            {
                std::lock_guard<std::mutex> lock(address_mutex_);
                cached_client_address_ = address;
            }
            
            return address;
        }
    }
    
    return std::nullopt;
}

Connection::Statistics Connection::get_statistics() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void Connection::close(bool immediate) {
    (void)immediate;

    State expected = state_.load();
    if (expected == State::CLOSED) {
        return;
    }

    state_ = State::CLOSED;

    if (fd_ >= 0) {
        ::close(fd_);
    }

    if (auto mgr = manager_.lock()) {
        mgr->on_connection_closed(connection_id_);
    }
}

bool Connection::send(std::string data) {
    if (is_closed()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(write_buffer_mutex_);
        write_buffer_.emplace_back(std::move(data));
    }

    flush_write_buffer();
    return true;
}

bool Connection::send_batch(const std::vector<std::string>& responses) {
    if (is_closed()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(write_buffer_mutex_);
        write_buffer_.insert(write_buffer_.end(), responses.begin(), responses.end());
    }

    flush_write_buffer();
    return true;
}

void Connection::set_command_handler(const std::string& command, CommandHandler handler) {
    command_handlers_[command] = std::move(handler);
}

void Connection::set_default_handler(CommandHandler handler) {
    default_handler_ = std::move(handler);
}

bool Connection::authenticate(const std::string& password) {
    // 如果未设置认证密码，则不需要认证
    if (auth_password_.empty()) {
        authenticated_ = true;
        return true;
    }
    
    // 检查密码是否匹配
    if (password == auth_password_) {
        authenticated_ = true;
        return true;
    }
    
    authenticated_ = false;
    return false;
}

void Connection::set_timeout(std::chrono::milliseconds timeout) {
    timeout_ = timeout;
}

void Connection::set_auth_password(const std::string& password) {
    auth_password_ = password;
}

void Connection::process_buffer() {
    while (parse_commands()) {
        // 继续解析直到缓冲区中没有完整命令
    }
}

bool Connection::parse_commands() {
    std::string buffer_copy;
    {
        std::lock_guard<std::mutex> lock(read_buffer_mutex_);
        buffer_copy = read_buffer_;
    }

    if (buffer_copy.empty()) {
        return false;
    }

    // 简易 RESP 解析器（只处理数组 + bulk string，用于 Redis 命令）
    auto try_parse_resp = [](const std::string& buf, ParsedCommand& out) -> bool {
        size_t pos = 0;
        auto read_int = [&](std::optional<int64_t>& val) -> bool {
            if (pos >= buf.size()) return false;
            bool neg = false;
            if (buf[pos] == '-') { neg = true; ++pos; }
            int64_t num = 0;
            bool has_digit = false;
            while (pos < buf.size() && std::isdigit(static_cast<unsigned char>(buf[pos]))) {
                has_digit = true;
                num = num * 10 + (buf[pos] - '0');
                ++pos;
            }
            if (!has_digit) return false;
            if (pos + 1 >= buf.size() || buf[pos] != '\r' || buf[pos + 1] != '\n') return false;
            pos += 2;
            val = neg ? -num : num;
            return true;
        };
        auto read_bulk = [&](std::optional<std::string>& val) -> bool {
            if (pos >= buf.size() || buf[pos] != '$') return false;
            ++pos;
            std::optional<int64_t> len;
            if (!read_int(len) || !len.has_value() || len.value() < 0) return false;
            size_t n = static_cast<size_t>(len.value());
            if (pos + n + 2 > buf.size()) return false;
            val = buf.substr(pos, n);
            pos += n;
            if (pos + 1 >= buf.size() || buf[pos] != '\r' || buf[pos + 1] != '\n') return false;
            pos += 2;
            return true;
        };

        if (buf.empty() || buf[0] != '*') return false;
        ++pos;
        std::optional<int64_t> arr_sz;
        if (!read_int(arr_sz) || !arr_sz.has_value() || arr_sz.value() <= 0) return false;
        size_t cnt = static_cast<size_t>(arr_sz.value());
        std::vector<std::string> parts;
        parts.reserve(cnt);
        for (size_t i = 0; i < cnt; ++i) {
            std::optional<std::string> bulk;
            if (!read_bulk(bulk) || !bulk.has_value()) return false;
            parts.push_back(std::move(*bulk));
        }
        if (parts.empty()) return false;
        out.command = parts[0];
        std::transform(out.command.begin(), out.command.end(), out.command.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        out.args.clear();
        for (size_t i = 1; i < parts.size(); ++i) {
            out.args.emplace_back(parts[i]);
        }
        return true;
    };

    // 支持 Redis inline 协议（如 "PING\r\n"、"SET k v\r\n"）
    // 如果不是以 '*' 开头，则按空格拆分为命令和参数
    if (!buffer_copy.empty() && buffer_copy[0] != '*') {
        // 去掉结尾的 CRLF
        while (!buffer_copy.empty() &&
               (buffer_copy.back() == '\r' || buffer_copy.back() == '\n')) {
            buffer_copy.pop_back();
        }

        std::istringstream iss(buffer_copy);
        std::string token;
        ParsedCommand cmd;

        if (!(iss >> token)) {
            return false;
        }

        // 命令名转小写
        std::transform(token.begin(), token.end(), token.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        cmd.command = std::move(token);

        // 参数全部作为字符串
        while (iss >> token) {
            cmd.args.emplace_back(token);
        }

        {
            std::lock_guard<std::mutex> lock(read_buffer_mutex_);
            read_buffer_.clear();
        }

        process_command(cmd);
        return true;
    }

    ParsedCommand cmd;
    // 优先尝试简易 RESP 解析；失败则回退到原有解析（支持更多 RESP 类型）
    if (!try_parse_resp(buffer_copy, cmd)) {
        if (!Protocol::parse_request_static(buffer_copy, cmd)) {
            // 数据不完整，等待更多数据
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(read_buffer_mutex_);
        read_buffer_.clear(); // 简化实现：一次只处理一个完整命令
    }

    process_command(cmd);
    return true;
}

void Connection::process_command(const ParsedCommand& cmd) {
    // 检查认证（如果设置了密码，除了AUTH和PING命令外，其他命令都需要认证）
    if (!auth_password_.empty() && cmd.command != "auth" && cmd.command != "ping") {
        if (!authenticated_.load()) {
            send_error("NOAUTH Authentication required");
            return;
        }
    }
    
    // 事务相关命令总是立即执行
    if (cmd.command == "multi") {
        std::vector<std::string> string_args;
        for (const auto& arg : cmd.args) {
            if (auto* str = std::get_if<std::string>(&arg)) {
                string_args.push_back(*str);
            }
        }
        std::string response;
        handle_multi(string_args, response);
        if (!response.empty()) {
            send(std::move(response));
        }
        return;
    }
    
    if (cmd.command == "exec") {
        std::string response;
        handle_exec(response);
        if (!response.empty()) {
            send(std::move(response));
        }
        return;
    }
    
    if (cmd.command == "discard") {
        std::string response;
        handle_discard(response);
        if (!response.empty()) {
            send(std::move(response));
        }
        return;
    }
    
    // 如果在事务中，将命令加入事务队列
    if (in_transaction_.load() && current_transaction_.active) {
        current_transaction_.commands.push_back(cmd);
        // 返回 QUEUED 响应
        send(Protocol::build_response("QUEUED"));
        return;
    }
    
    // 正常执行命令（在 try 中捕获异常）
    try {
        std::string response;

        // 提取字符串参数（用于兼容旧的接口）
        std::vector<std::string> string_args;
        for (const auto& arg : cmd.args) {
            if (auto* str = std::get_if<std::string>(&arg)) {
                string_args.push_back(*str);
            } else {
                // 尝试转换为字符串
                string_args.push_back("");
            }
        }

        auto it = command_handlers_.find(cmd.command);
        if (it != command_handlers_.end()) {
            it->second(*this, string_args, response);
        } else if (default_handler_) {
            default_handler_(*this, string_args, response);
        } else {
            // 使用内置命令处理
            if (cmd.command == "get") {
                handle_get(string_args, response);
            } else if (cmd.command == "set") {
                handle_set(string_args, response);
            } else if (cmd.command == "del") {
                handle_del(string_args, response);
            } else if (cmd.command == "exists") {
                handle_exists(string_args, response);
            } else if (cmd.command == "ping") {
                handle_ping(response);
            } else if (cmd.command == "info") {
                handle_info(response);
            } else if (cmd.command == "auth") {
                handle_auth(string_args, response);
            } else if (cmd.command == "select") {
                handle_select(string_args, response);
            } else {
                send_error("ERR unknown command '" + cmd.command + "'");
                return;
            }
        }

        if (!response.empty()) {
            send(std::move(response));
        }
    } catch (const std::exception& e) {
        send_error(std::string("ERR ") + e.what());
    } catch (...) {
        send_error("ERR internal error");
    }

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.commands_processed++;
    }
}

void Connection::handle_get(const std::vector<std::string>& args, std::string& response) {
    if (args.size() < 1) {
        send_error("ERR wrong number of arguments for 'GET' command");
        return;
    }
    
    if (!engine_) {
        send_error("ERR engine not available");
        return;
    }
    
    try {
        Slice key(args[0]);
        auto future = engine_->get(key);
        auto result = future.get(); // 阻塞等待结果
        response = result.has_value()
            ? Protocol::build_bulk_string(result->ToString())
            : Protocol::build_null_bulk_string();
    } catch (const std::exception& e) {
        send_error("ERR " + std::string(e.what()));
    }
}

void Connection::handle_set(const std::vector<std::string>& args, std::string& response) {
    if (args.size() < 2) {
        send_error("ERR wrong number of arguments for 'SET' command");
        return;
    }
    
    if (!engine_) {
        send_error("ERR engine not available");
        return;
    }
    
    try {
        Slice key(args[0]);
        Slice value(args[1]);
        
        // 设置超时，避免无限等待
        auto future = engine_->put(key, value);
        auto status = future.wait_for(std::chrono::seconds(10));
        
        if (status == std::future_status::timeout) {
            send_error("ERR backend timeout (10s)");
            response.clear();
            return;
        }
        
        bool success = future.get(); // 阻塞等待结果
        
        if (success) {
            send_ok();
        } else {
            send_error("ERR failed to set key");
        }
        response.clear();
    } catch (const std::exception& e) {
        send_error("ERR " + std::string(e.what()));
    }
}

void Connection::handle_del(const std::vector<std::string>& args, std::string& response) {
    if (args.empty()) {
        send_error("ERR wrong number of arguments for 'DEL' command");
        return;
    }
    
    if (!engine_) {
        send_error("ERR engine not available");
        return;
    }
    
    try {
        int deleted_count = 0;
        for (const auto& arg : args) {
            Slice key(arg);
            auto future = engine_->remove(key);
            if (future.get()) { // 阻塞等待结果
                deleted_count++;
            }
        }
        response = Protocol::build_integer(deleted_count);
    } catch (const std::exception& e) {
        send_error("ERR " + std::string(e.what()));
    }
}

void Connection::handle_exists(const std::vector<std::string>& args, std::string& response) {
    if (args.empty()) {
        send_error("ERR wrong number of arguments for 'EXISTS' command");
        return;
    }
    
    if (!engine_) {
        send_error("ERR engine not available");
        return;
    }
    
    try {
        int exists_count = 0;
        for (const auto& arg : args) {
            Slice key(arg);
            auto future = engine_->get(key);
            if (future.get().has_value()) { // 阻塞等待结果
                exists_count++;
            }
        }
        response = Protocol::build_integer(exists_count);
    } catch (const std::exception& e) {
        send_error("ERR " + std::string(e.what()));
    }
}

void Connection::handle_ping(std::string& response) {
    response = Protocol::build_response("PONG");
}

void Connection::handle_info(std::string& response) {
    // 返回服务器基本信息
    std::ostringstream oss;
    oss << "# Server\r\n";
    oss << "version:MementoDB-1.0.0\r\n";
    oss << "redis_version:6.0.0\r\n"; // 兼容 Redis 客户端
    oss << "os:" << "Linux" << "\r\n";
    oss << "arch_bits:64\r\n";
    oss << "tcp_port:" << "6379" << "\r\n";
    oss << "connected_clients:1\r\n"; // 简化：返回固定值
    oss << "used_memory:0\r\n";
    oss << "used_memory_human:0B\r\n";
    oss << "db0:keys=0,expires=0\r\n";
    response = Protocol::build_bulk_string(oss.str());
}

void Connection::handle_auth(const std::vector<std::string>& args, std::string& response) {
    (void)response;
    if (args.size() < 2) {
        send_error("ERR wrong number of arguments for 'AUTH' command");
        return;
    }
    if (authenticate(args[1])) {
        send_ok();
    } else {
        send_error("ERR invalid password");
    }
}

void Connection::handle_select(const std::vector<std::string>& args, std::string& response) {
    if (args.size() < 2) {
        send_error("ERR wrong number of arguments for 'SELECT' command");
        return;
    }
    
    try {
        int db_index = std::stoi(args[1]);
        
        // Redis 默认支持 0-15 共16个数据库
        // 这里我们允许更大的范围，但设置一个合理的上限（如1024）
        if (db_index < 0 || db_index > 1024) {
            send_error("ERR DB index is out of range");
            return;
        }
        
        // 设置当前数据库索引
        current_db_index_.store(db_index);
        
        // 注意：实际的数据库隔离需要在 DiskEngineV2 层面实现
        // 这里只是记录当前选择的数据库索引
        // 如果 DiskEngineV2 不支持多数据库，所有数据库索引会共享同一个存储空间
        
        response = Protocol::build_response("OK");
    } catch (const std::exception& e) {
        send_error("ERR invalid DB index");
    }
}

void Connection::handle_multi(const std::vector<std::string>& args, std::string& response) {
    (void)args;
    (void)response;
    current_transaction_.commands.clear();
    current_transaction_.active = true;
    in_transaction_ = true;
    send_ok();
}

void Connection::handle_exec(std::string& response) {
    if (!in_transaction_.load() || !current_transaction_.active) {
        send_error("ERR EXEC without MULTI");
        return;
    }
    
    if (current_transaction_.commands.empty()) {
        response = Protocol::build_empty_array();
        current_transaction_.commands.clear();
        current_transaction_.active = false;
        in_transaction_ = false;
        return;
    }
    
    // 执行事务中的所有命令
    std::vector<std::string> results;
    bool transaction_failed = false;
    
    for (const auto& cmd : current_transaction_.commands) {
        try {
            // 提取字符串参数
            std::vector<std::string> string_args;
            for (const auto& arg : cmd.args) {
                if (auto* str = std::get_if<std::string>(&arg)) {
                    string_args.push_back(*str);
                } else {
                    string_args.push_back("");
                }
            }
            
            std::string cmd_response;
            
            // 执行命令
            auto it = command_handlers_.find(cmd.command);
            if (it != command_handlers_.end()) {
                it->second(*this, string_args, cmd_response);
            } else if (default_handler_) {
                default_handler_(*this, string_args, cmd_response);
            } else {
                // 使用内置命令处理
                if (cmd.command == "get") {
                    handle_get(string_args, cmd_response);
                } else if (cmd.command == "set") {
                    handle_set(string_args, cmd_response);
                } else if (cmd.command == "del") {
                    handle_del(string_args, cmd_response);
                } else if (cmd.command == "exists") {
                    handle_exists(string_args, cmd_response);
                } else {
                    // 未知命令，标记事务失败
                    transaction_failed = true;
                    cmd_response = Protocol::build_error_static("ERR unknown command '" + cmd.command + "'");
                }
            }
            
            results.push_back(cmd_response);
        } catch (const std::exception& e) {
            transaction_failed = true;
            results.push_back(Protocol::build_error_static("ERR " + std::string(e.what())));
        }
    }
    
    // 构建批量响应
    if (transaction_failed) {
        // 如果事务失败，返回错误
        response = Protocol::build_error_static("EXECABORT Transaction discarded because of previous errors");
    } else {
        // 返回所有命令的结果
        response = Protocol::build_array(results);
    }
    
    // 清理事务状态
    current_transaction_.commands.clear();
    current_transaction_.active = false;
    in_transaction_ = false;
}

void Connection::handle_discard(std::string& response) {
    (void)response;
    current_transaction_.commands.clear();
    current_transaction_.active = false;
    in_transaction_ = false;
    send_ok();
}

void Connection::update_activity() {
    last_activity_ = std::chrono::steady_clock::now();
}

void Connection::check_timeout() {
    auto now = std::chrono::steady_clock::now();
    if (now - last_activity_ > timeout_) {
        send_error("ERR connection timed out");
        close(true);
    }
}

void Connection::flush_write_buffer() {
    std::vector<std::string> pending;
    {
        std::lock_guard<std::mutex> lock(write_buffer_mutex_);
        pending.swap(write_buffer_);
    }

    for (const auto& chunk : pending) {
        ssize_t n = ::send(fd_, chunk.data(), chunk.size(), 0);
        if (n > 0) {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.bytes_sent += static_cast<uint64_t>(n);
        } else if (n < 0) {
            send_error("ERR send failed");
            break;
        }
    }
}

void Connection::send_error(const std::string& error_msg) {
    auto resp = Protocol::build_error_static(error_msg);
    send(std::move(resp));

    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.errors++;
}

void Connection::send_ok() {
    auto resp = Protocol::build_response("OK");
    send(std::move(resp));
}

} // namespace net
} // namespace mementodb

