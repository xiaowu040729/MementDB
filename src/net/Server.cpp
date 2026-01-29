// File: src/net/Server.cpp

#include "Server.hpp"
#include "EpollLoop.hpp"
#include "Connection.hpp"
#include "IOLoop.hpp"
#include "../core/DiskEngine.hpp"
#include "../utils/LoggingSystem/LogMacros.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <fstream>
#include <signal.h>
#include <sstream>
#include <iomanip>
#include <random>
#include <cstring>

namespace mementodb {
namespace net {

// ===== Server::Statistics =====

std::string Server::Statistics::to_string() const {
    std::ostringstream oss;
    oss << "Statistics{"
        << "total_connections=" << total_connections
        << ", active_connections=" << active_connections
        << ", total_requests=" << total_requests
        << ", failed_requests=" << failed_requests
        << ", bytes_received=" << bytes_received
        << ", bytes_sent=" << bytes_sent
        << "}";
    return oss.str();
}

// ===== Server::Monitor (简单占位实现) =====

class Server::Monitor {
public:
    explicit Monitor(Server* server) : server_(server) {}
private:
    Server* server_;
};

// ===== Server::Listener =====

Server::Listener::Listener(Server* server, const ServerConfig::ListenConfig& config)
    : server_(server),
      config_(config),
      fd_(-1),
      listening_(false) {}

Server::Listener::~Listener() {
    stop();
}

bool Server::Listener::start() {
    if (listening_.load()) {
        return true;
    }
    
    if (!create_socket()) {
        return false;
    }
    if (!setup_socket_options()) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    if (!bind_and_listen()) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    
    // 注册到 Server 的 ConnectionManager 的 IOLoop
    // 注意：io_loop_ 在 start() 时被移交给 ConnectionManager，所以需要通过 ConnectionManager 访问
    if (server_->connection_manager_) {
        auto* io_loop = server_->connection_manager_->io_loop();
        if (io_loop) {
        auto callback = [this](int fd, IOEvent event, void*) {
            if (io_event_contains(event, IOEvent::READ)) {
                // 接受新连接
                struct sockaddr_in client_addr{};
                socklen_t addr_len = sizeof(client_addr);
                int client_fd = ::accept(fd, reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);
                if (client_fd >= 0) {
                    char client_ip[INET_ADDRSTRLEN];
                    ::inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
                    int client_port = ntohs(client_addr.sin_port);
                    server_->on_accept(this, client_fd, std::string(client_ip), client_port);
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    LOG_ERROR("NET", "accept failed: " + std::string(std::strerror(errno)));
                }
            }
        };
        
            if (!io_loop->add_fd(fd_, IOEvent::READ, callback, nullptr)) {
                ::close(fd_);
                fd_ = -1;
                return false;
            }
        } else {
            LOG_ERROR("NET", "IOLoop not available when starting listener");
            return false;
        }
    } else {
        // 如果 ConnectionManager 还未创建，先创建临时的 IOLoop
        // 这种情况不应该发生，因为 start() 会先 init()
        LOG_ERROR("NET", "ConnectionManager not initialized when starting listener");
        return false;
    }
    
    listening_.store(true);
    return true;
}

void Server::Listener::stop() {
    if (!listening_.load()) {
        return;
    }
    
    listening_.store(false);
    
    if (server_->connection_manager_ && fd_ >= 0) {
        auto* io_loop = server_->connection_manager_->io_loop();
        if (io_loop) {
            io_loop->remove_fd(fd_);
        }
    }
    
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

std::string Server::Listener::endpoint() const {
    std::ostringstream oss;
    oss << config_.host << ":" << config_.port;
    return oss.str();
}

bool Server::Listener::create_socket() {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        LOG_ERROR("NET", "Failed to create socket: " + std::string(std::strerror(errno)));
        return false;
    }
    return true;
}

bool Server::Listener::setup_socket_options() {
    int opt = 1;
    
    // SO_REUSEADDR
    if (config_.reuse_addr) {
        if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            LOG_ERROR("NET", "Failed to set SO_REUSEADDR: " + std::string(std::strerror(errno)));
            return false;
        }
    }
    
    // TCP_NODELAY
    if (config_.tcp_no_delay) {
        if (::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
            LOG_ERROR("NET", "Failed to set TCP_NODELAY: " + std::string(std::strerror(errno)));
            return false;
        }
    }
    
    // TCP_KEEPALIVE
    if (config_.tcp_keepalive) {
        if (::setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt)) < 0) {
            LOG_ERROR("NET", "Failed to set SO_KEEPALIVE: " + std::string(std::strerror(errno)));
            return false;
        }
    }
    
    // 设置非阻塞
    int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        LOG_ERROR("NET", "Failed to set non-blocking: " + std::string(std::strerror(errno)));
        return false;
    }
    
    return true;
}

bool Server::Listener::bind_and_listen() {
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.port);
    
    if (config_.host == "0.0.0.0" || config_.host.empty()) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (::inet_pton(AF_INET, config_.host.c_str(), &addr.sin_addr) <= 0) {
            LOG_ERROR("NET", "Invalid host address: " + config_.host);
            return false;
        }
    }
    
    if (::bind(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("NET", "Failed to bind socket: " + std::string(std::strerror(errno)));
        return false;
    }
    
    if (::listen(fd_, config_.backlog) < 0) {
        LOG_ERROR("NET", "Failed to listen: " + std::string(std::strerror(errno)));
        return false;
    }
    
    return true;
}

// ===== Server::ConnectionManager =====

Server::ConnectionManager::ConnectionManager(Server* server,
                                             const ServerConfig::ConnectionConfig& config,
                                             std::unique_ptr<IOLoop> io_loop)
    : server_(server),
      config_(config),
      io_loop_(std::move(io_loop)),
      next_connection_id_(1) {
    // 启动 IO 事件循环线程
    if (io_loop_) {
        io_thread_ = std::thread([loop = io_loop_.get()]() {
            loop->run();
        });
    }
    // 启动清理任务
    schedule_cleanup();
}

Server::ConnectionManager::~ConnectionManager() {
    if (io_loop_) {
        io_loop_->stop(true);
    }
    if (io_thread_.joinable()) {
        io_thread_.join();
    }
    remove_all_connections(false);
}

bool Server::ConnectionManager::add_connection(int fd,
                                               const std::string& client_ip,
                                               int client_port,
                                               const std::string& server_ip,
                                               int server_port) {
    // 如果没有提供 Connection，创建一个新的
    if (!server_ || !server_->engine_) {
        return false;
    }
    auto conn = std::make_shared<Connection>(fd, server_->engine_);
    return add_connection(fd, conn, client_ip, client_port, server_ip, server_port);
}

bool Server::ConnectionManager::add_connection(int fd,
                                               std::shared_ptr<Connection> conn,
                                               const std::string& client_ip,
                                               int client_port,
                                               const std::string& server_ip,
                                               int server_port) {
    if (!conn || fd < 0) {
        return false;
    }
    
    auto ctx = std::make_shared<ConnectionContext>();
    ctx->connection = conn;
    ctx->id = next_connection_id_++;
    ctx->client_ip = client_ip;
    ctx->client_port = client_port;
    ctx->server_ip = server_ip;
    ctx->server_port = server_port;
    ctx->create_time = std::chrono::steady_clock::now();
    ctx->last_activity = ctx->create_time;
    ctx->bytes_received = 0;
    ctx->bytes_sent = 0;
    ctx->closing.store(false);

    {
        std::unique_lock<std::shared_mutex> lock(connections_mutex_);
        connections_[fd] = ctx;
        connection_id_map_[ctx->id] = fd;
    }

    // 注册到事件循环，监听读/挂起/错误事件
    if (io_loop_) {
        io_loop_->add_fd(fd, IOEvent::READ | IOEvent::HUP | IOEvent::ERROR,
                         [this](int fd, IOEvent event, void* ud) {
                             (void)ud;
                             on_connection_data(fd, event, nullptr);
                         });
    }

    return true;
}

bool Server::ConnectionManager::remove_connection(int fd) {
    if (io_loop_) {
        io_loop_->remove_fd(fd);
    }

    std::unique_lock<std::shared_mutex> lock(connections_mutex_);
    auto it = connections_.find(fd);
    if (it == connections_.end()) return false;
    connection_id_map_.erase(it->second->id);
    connections_.erase(it);
    return true;
}

void Server::ConnectionManager::remove_all_connections(bool /*graceful*/) {
    std::unique_lock<std::shared_mutex> lock(connections_mutex_);
    for (const auto& kv : connections_) {
        if (io_loop_) {
            io_loop_->remove_fd(kv.first);
        }
    }
    connections_.clear();
    connection_id_map_.clear();
}

std::vector<Server::ConnectionInfo> Server::ConnectionManager::get_connections() const {
    std::vector<ConnectionInfo> result;
    std::shared_lock<std::shared_mutex> lock(connections_mutex_);
    result.reserve(connections_.size());
    for (const auto& kv : connections_) {
        const auto& ctx = kv.second;
        ConnectionInfo info{};
        info.fd = kv.first;
        info.client_ip = ctx->client_ip;
        info.client_port = ctx->client_port;
        info.server_ip = ctx->server_ip;
        info.server_port = ctx->server_port;
        info.connection_time = 0;
        info.last_activity = 0;
        info.bytes_received = ctx->bytes_received;
        info.bytes_sent = ctx->bytes_sent;
        info.state = ctx->closing ? "CLOSING" : "ACTIVE";
        info.protocol_version = "resp2";
        result.push_back(std::move(info));
    }
    return result;
}

size_t Server::ConnectionManager::connection_count() const {
    std::shared_lock<std::shared_mutex> lock(connections_mutex_);
    return connections_.size();
}

void Server::ConnectionManager::update_connection_stats() {
    // 由 Server 调用，当前实现不做详细统计
}

void Server::ConnectionManager::cleanup_idle_connections() {
    // 占位实现
}

void Server::ConnectionManager::check_connection_timeouts() {
    // 占位实现
}

void Server::ConnectionManager::schedule_cleanup() {
    // 占位实现
}

void Server::ConnectionManager::on_connection_data(int fd, IOEvent event, void* user_data) {
    std::shared_ptr<ConnectionContext> ctx;
    {
        std::shared_lock<std::shared_mutex> lock(connections_mutex_);
        auto it = connections_.find(fd);
        if (it == connections_.end()) {
            return;
        }
        ctx = it->second;
    }
    
    if (!ctx || !ctx->connection || ctx->closing.load()) {
        return;
    }

    // 如果是错误或挂起事件，直接关闭
    if (io_event_contains(event, IOEvent::ERROR) || io_event_contains(event, IOEvent::HUP)) {
        remove_connection(fd);
        if (server_) {
            server_->on_connection_closed(fd, ctx->id);
        }
        return;
    }
    
    // 读取数据
    if (io_event_contains(event, IOEvent::READ)) {
        char buffer[4096];
        ssize_t n = ::read(fd, buffer, sizeof(buffer));
        
        if (n > 0) {
            // 更新活动时间
            ctx->last_activity = std::chrono::steady_clock::now();
            ctx->bytes_received += n;
            
            // 处理接收到的数据
            if (!ctx->connection->on_data_received(buffer, n)) {
                // 连接已关闭
                remove_connection(fd);
                if (server_) {
                    server_->on_connection_closed(fd, ctx->id);
                }
                return;
            }
            
            // 更新统计
            if (server_) {
                server_->stats_.bytes_received += n;
            }
        } else if (n == 0) {
            // 对端关闭连接
            remove_connection(fd);
            if (server_) {
                server_->on_connection_closed(fd, ctx->id);
            }
        } else {
            // 错误处理
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::error_code ec(errno, std::generic_category());
                on_connection_error(fd, event, user_data);
            }
        }
    }
}

void Server::ConnectionManager::on_connection_error(int fd, IOEvent event, void* user_data) {
    std::shared_ptr<ConnectionContext> ctx;
    {
        std::shared_lock<std::shared_mutex> lock(connections_mutex_);
        auto it = connections_.find(fd);
        if (it == connections_.end()) {
            return;
        }
        ctx = it->second;
    }
    
    if (!ctx) {
        return;
    }
    
    std::error_code ec(errno, std::generic_category());
    
    // 关闭连接
    if (ctx->connection) {
        ctx->connection->close(true);
    }
    
    // 从 IOLoop 移除
    if (io_loop_) {
        io_loop_->remove_fd(fd);
    }
    
    // 移除连接
    remove_connection(fd);
    
    // 通知 Server
    if (server_) {
        server_->on_connection_error(fd, ec);
        server_->stats_.network_errors++;
    }
}

void Server::ConnectionManager::on_connection_timeout(int /*fd*/) {
    // 占位实现
}

// ===== Server =====

Server::Server(std::shared_ptr<core::DiskEngineV2> engine, 
               const ServerConfig& config)
    : engine_(std::move(engine)),
      io_loop_(nullptr),
      thread_pool_(nullptr),
      connection_manager_(nullptr),
      monitor_(nullptr),
      config_(config),
      state_(State::STOPPED),
      stopping_(false),
      server_id_(generate_server_id()),
      stats_running_(false) {
}

Server::~Server() {
    stop(true, 0);
}

std::shared_ptr<Server> Server::create(std::shared_ptr<core::DiskEngineV2> engine,
                                       const ServerConfig& config) {
    return std::make_shared<Server>(std::move(engine), config);
}

bool Server::init() {
    if (!config_.validate()) {
        LOG_ERROR("NET", "Server config validate failed");
        return false;
    }
    if (!init_io_loop()) return false;
    
    // 将 IOLoop 移交给 ConnectionManager，供监听器注册使用
    if (!connection_manager_) {
        connection_manager_ = std::make_unique<ConnectionManager>(
            this, config_.connection_config, std::move(io_loop_));
    }
    if (!connection_manager_ || !connection_manager_->io_loop()) {
        LOG_ERROR("NET", "ConnectionManager initialization failed");
        return false;
    }
    
    if (!init_thread_pool()) return false;
    if (!init_listeners()) return false;
    if (!init_monitor()) return false;
    return true;
}

bool Server::init_io_loop() {
    if (!io_loop_) {
        io_loop_ = IOLoop::create_default();
        if (!io_loop_) {
            LOG_ERROR("NET", "Failed to create default IOLoop");
            return false;
        }
    }
    
    IOLoop::Options opts;
    opts.name = "server-io-loop";
    if (!io_loop_->init(opts)) {
        LOG_ERROR("NET", "Failed to init IOLoop");
        return false;
    }
    return true;
}

bool Server::init_thread_pool() {
    if (thread_pool_) return true;
    
    ThreadPoolConfig cfg = ThreadPoolConfig::default_config();
    int workers = config_.thread_config.worker_threads;
    if (workers <= 0) {
        workers = static_cast<int>(std::thread::hardware_concurrency());
        if (workers <= 0) workers = 4;
    }
    cfg.min_threads = static_cast<size_t>(workers);
    cfg.max_threads = static_cast<size_t>(workers);
    cfg.init_threads = cfg.min_threads;
    
    thread_pool_ = std::make_unique<ThreadPool>(cfg);
    if (!thread_pool_->start()) {
        LOG_ERROR("NET", "Failed to start ThreadPool");
        thread_pool_.reset();
        return false;
    }
    return true;
}

bool Server::init_listeners() {
    std::unique_lock<std::shared_mutex> lock(listeners_mutex_);
    if (config_.listen_configs.empty()) {
        ServerConfig::ListenConfig lc;
        config_.listen_configs.push_back(lc);
    }
    for (const auto& lc : config_.listen_configs) {
        auto listener = std::make_unique<Listener>(this, lc);
        if (!listener->start()) {
            LOG_ERROR("NET", "Failed to start listener");
            return false;
        }
        listeners_.push_back(std::move(listener));
    }
    return true;
}

bool Server::init_monitor() {
    if (!monitor_) {
        monitor_ = std::make_unique<Monitor>(this);
    }
    return true;
}

void Server::cleanup() {
    {
        std::unique_lock<std::shared_mutex> lock(listeners_mutex_);
        for (auto& l : listeners_) {
            l->stop();
        }
        listeners_.clear();
    }
    if (connection_manager_) {
        connection_manager_->remove_all_connections(true);
        connection_manager_.reset();
    }
    if (io_loop_) {
        io_loop_.reset();
    }
    if (thread_pool_) {
        thread_pool_->stop(true, 0);
        thread_pool_.reset();
    }
    if (monitor_) {
        monitor_.reset();
    }
    // 停止统计线程
    stats_running_.store(false);
    if (stats_thread_.joinable()) {
        stats_thread_.join();
    }
}

bool Server::start(bool wait_for_ready, uint64_t timeout_ms) {
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (state_ == State::RUNNING || state_ == State::STARTING) {
        return true;
    }
    if (!init()) {
        set_state(State::ERROR);
        return false;
    }

    start_time_ = std::chrono::steady_clock::now();
    
    // 启动统计信息收集线程
    if (config_.monitor_config.enable_stats) {
        stats_running_.store(true);
        stats_thread_ = std::thread([this]() {
            stats_collection_loop();
        });
    }
    
    set_state(State::RUNNING);
    // 释放锁后再等待状态，避免自锁
    lock.unlock();
    
    if (wait_for_ready) {
        return wait_for_state(State::RUNNING, timeout_ms);
    }
    return true;
}

void Server::stop(bool graceful, uint64_t /*timeout_ms*/) {
    {
        std::unique_lock<std::mutex> lock(state_mutex_);
        if (state_ == State::STOPPED || state_ == State::STOPPING) {
            return;
        }
        stopping_.store(true);
        set_state(State::STOPPING);
    }

    if (connection_manager_) {
        connection_manager_->remove_all_connections(graceful);
    }

    cleanup();

    {
        std::unique_lock<std::mutex> lock(state_mutex_);
        set_state(State::STOPPED);
    }
}

bool Server::restart(uint64_t timeout_ms) {
    stop(true, timeout_ms);
    return start(true, timeout_ms);
}

void Server::run(bool foreground) {
    if (!start(true)) {
        return;
    }
    if (foreground) {
        // 当前没有单独的事件循环线程，这里简单阻塞等待停止信号
        wait_for_state(State::STOPPED, 0);
    }
}

bool Server::wait_for_state(State target_state, uint64_t timeout_ms) {
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (timeout_ms == 0) {
        state_cv_.wait(lock, [&] { return state_ == target_state; });
        return true;
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    return state_cv_.wait_until(lock, deadline, [&] { return state_ == target_state; });
}

bool Server::update_config(const ServerConfig& new_config) {
    if (!new_config.validate()) return false;
    std::lock_guard<std::mutex> lock(state_mutex_);
    config_ = new_config;
    return true;
}

Server::Statistics Server::get_statistics() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void Server::reset_statistics() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = Statistics{};
}

std::vector<Server::EndpointInfo> Server::get_listen_endpoints() const {
    std::vector<EndpointInfo> endpoints;
    std::shared_lock<std::shared_mutex> lock(listeners_mutex_);
    endpoints.reserve(listeners_.size());
    for (const auto& l : listeners_) {
        EndpointInfo ei{};
        ei.host = l->config().host;
        ei.port = l->config().port;
        ei.fd = l->fd();
        ei.listening = l->is_listening();
        ei.protocol = "tcp";
        endpoints.push_back(ei);
    }
    return endpoints;
}

bool Server::add_listen_endpoint(const std::string& host, int port) {
    ServerConfig::ListenConfig lc;
    lc.host = host;
    lc.port = port;
    config_.listen_configs.push_back(lc);
    auto listener = std::make_unique<Listener>(this, lc);
    if (!listener->start()) {
        return false;
    }
    std::unique_lock<std::shared_mutex> lock(listeners_mutex_);
    listeners_.push_back(std::move(listener));
    return true;
}

bool Server::remove_listen_endpoint(const std::string& host, int port) {
    std::unique_lock<std::shared_mutex> lock(listeners_mutex_);
    for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
        if ((*it)->config().host == host && (*it)->config().port == port) {
            (*it)->stop();
            listeners_.erase(it);
            return true;
        }
    }
    return false;
}

std::vector<Server::ConnectionInfo> Server::get_connections() const {
    if (!connection_manager_) return {};
    return connection_manager_->get_connections();
}

bool Server::disconnect(int fd, bool /*graceful*/) {
    if (!connection_manager_) return false;
    return connection_manager_->remove_connection(fd);
}

void Server::disconnect_all(bool graceful) {
    if (connection_manager_) {
        connection_manager_->remove_all_connections(graceful);
    }
}

void Server::set_connection_factory(ConnectionFactory factory) {
    connection_factory_ = std::move(factory);
}

void Server::set_connection_filter(ConnectionFilter filter) {
    connection_filter_ = std::move(filter);
}

void Server::set_event_callback(ServerEventCallback callback) {
    event_callback_ = std::move(callback);
}

void Server::set_exception_handler(ExceptionHandler handler) {
    exception_handler_ = std::move(handler);
}

void Server::setup_signal_handlers() {
    ::signal(SIGINT, &Server::handle_signal);
    ::signal(SIGTERM, &Server::handle_signal);
}

void Server::handle_signal(int /*signal*/) {
    // 实际关闭逻辑由外部协调，这里不做直接处理
}

bool Server::save_state(const std::string& path) {
    try {
        std::ofstream file(path, std::ios::binary);
        if (!file.is_open()) {
            LOG_ERROR("NET", "Failed to open file for saving state: " + path);
            return false;
        }
        
        // 保存基本状态信息
        std::string server_id = server_id_;
        file.write(server_id.c_str(), server_id.size());
        file.put('\n');
        
        // 保存配置信息（简化版本）
        std::string config_str = config_.to_string();
        file.write(config_str.c_str(), config_str.size());
        file.put('\n');
        
        // 保存统计信息
        auto stats = get_statistics();
        std::string stats_str = stats.to_string();
        file.write(stats_str.c_str(), stats_str.size());
        
        file.close();
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("NET", "Exception while saving state: " + std::string(e.what()));
        return false;
    }
}

bool Server::load_state(const std::string& path) {
    try {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            LOG_ERROR("NET", "Failed to open file for loading state: " + path);
            return false;
        }
        
        // 读取状态信息（简化版本，实际应该解析JSON或其他格式）
        std::string line;
        if (std::getline(file, line)) {
            // 可以在这里恢复 server_id_ 等状态
            // 当前实现只做基本验证
        }
        
        file.close();
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("NET", "Exception while loading state: " + std::string(e.what()));
        return false;
    }
}

Server::CommandResult Server::execute_admin_command(const std::string& command,
                                                   const std::vector<std::string>& args) {
    if (command == "INFO") return admin_info(args);
    if (command == "CLIENTS") return admin_clients(args);
    if (command == "STATS") return admin_stats(args);
    if (command == "CONFIG") return admin_config(args);
    if (command == "SHUTDOWN") return admin_shutdown(args);
    if (command == "RELOAD") return admin_reload(args);
    if (command == "DIAG") return admin_diagnostics(args);
    return {false, "Unknown command", ""};
}

Server::HealthStatus Server::health_check() const {
    HealthStatus hs;
    hs.healthy = (state_ == State::RUNNING);
    hs.status = hs.healthy ? "RUNNING" : "NOT_RUNNING";
    hs.details["config"] = config_.to_string();
    hs.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    return hs;
}

void Server::notify_event(const std::string& event, const std::string& data) {
    if (event_callback_) {
        safe_invoke_callback([this, event, data] { event_callback_(event, data); });
    }
}

void Server::on_accept(Listener* listener, int fd,
                       const std::string& client_ip, int client_port) {
    if (!listener || fd < 0 || !connection_manager_) {
        if (fd >= 0) ::close(fd);
        return;
    }
    
    // 验证连接
    if (!validate_connection(client_ip, client_port)) {
        LOG_ERROR("NET", "Connection rejected from " + client_ip + ":" + std::to_string(client_port));
        ::close(fd);
        stats_.rejected_connections++;
        return;
    }
    
    // 检查速率限制
    if (!check_rate_limit(client_ip)) {
        LOG_ERROR("NET", "Rate limit exceeded for " + client_ip);
        ::close(fd);
        stats_.rejected_connections++;
        return;
    }
    
    // 检查最大连接数
    if (connection_manager_->connection_count() >= static_cast<size_t>(config_.connection_config.max_connections)) {
        LOG_ERROR("NET", "Max connections reached");
        ::close(fd);
        stats_.rejected_connections++;
        return;
    }
    
    // 设置 socket 为非阻塞
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        LOG_ERROR("NET", "Failed to set client socket non-blocking");
        ::close(fd);
        return;
    }
    
    // 获取服务器地址
    std::string server_ip = listener->config().host;
    int server_port = listener->config().port;
    
    // 创建 Connection 对象
    std::shared_ptr<Connection> conn;
    if (connection_factory_) {
        conn = connection_factory_(fd, client_ip, client_port, engine_, config_.protocol_config);
    } else {
        // 使用默认 Connection 构造函数
        conn = std::make_shared<Connection>(fd, engine_);
    }
    
    if (!conn) {
        LOG_ERROR("NET", "Failed to create Connection");
        ::close(fd);
        return;
    }
    
    // 设置认证密码（如果启用认证）
    if (config_.security_config.enable_auth) {
        conn->set_auth_password(config_.security_config.auth_password);
    }
    
    // 添加到 ConnectionManager（使用已创建的 Connection）
    if (!connection_manager_->add_connection(fd, conn, client_ip, client_port, server_ip, server_port)) {
        LOG_ERROR("NET", "Failed to add connection to ConnectionManager");
        conn->close(true);
        return;
    }
    
    // 注册到 IOLoop 进行数据监听（通过 ConnectionManager 内部方法）
    // 注意：需要在 ConnectionManager 中添加公共方法来注册连接
    // 临时方案：在 add_connection 中已经处理了注册，这里不需要重复注册
    
    // 更新每IP连接计数
    {
        std::lock_guard<std::mutex> lock(ip_connections_mutex_);
        ip_connections_[client_ip]++;
    }
    
    // 更新统计
    stats_.total_connections++;
    stats_.active_connections++;
    if (stats_.active_connections > stats_.peak_connections) {
        stats_.peak_connections = stats_.active_connections;
    }
    
    notify_event("connection.accepted", client_ip + ":" + std::to_string(client_port));
}

void Server::on_connection_closed(int fd, uint64_t connection_id) {
    // 查找连接的 IP 地址并更新计数
    if (connection_manager_) {
        auto connections = connection_manager_->get_connections();
        for (const auto& conn_info : connections) {
            if (conn_info.fd == fd) {
                std::lock_guard<std::mutex> lock(ip_connections_mutex_);
                auto it = ip_connections_.find(conn_info.client_ip);
                if (it != ip_connections_.end()) {
                    if (it->second > 0) {
                        it->second--;
                    }
                    if (it->second == 0) {
                        ip_connections_.erase(it);
                    }
                }
                break;
            }
        }
        stats_.active_connections = connection_manager_->connection_count();
    }
    notify_event("connection.closed", std::to_string(connection_id));
}

void Server::on_connection_error(int fd, const std::error_code& ec) {
    stats_.network_errors++;
    notify_event("connection.error", std::to_string(fd) + ":" + ec.message());
}

void Server::on_request_completed(int fd, const std::string& command,
                                  uint64_t duration_us, bool success) {
    stats_.total_requests++;
    if (!success) {
        stats_.failed_requests++;
    }
    
    // 更新命令统计
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.command_stats[command]++;
        if (duration_us > static_cast<uint64_t>(config_.monitor_config.slow_log_threshold * 1000)) {
            stats_.slow_commands++;
        }
    }
}

void Server::update_connection_stats() {
    if (connection_manager_) {
        stats_.active_connections = connection_manager_->connection_count();
        if (stats_.active_connections > stats_.peak_connections) {
            stats_.peak_connections = stats_.active_connections;
        }
    }
}

void Server::update_request_stats() {
    // 计算每秒请求数（简化实现）
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - start_time_).count();
    if (elapsed > 0) {
        stats_.requests_per_second = stats_.total_requests / elapsed;
        if (stats_.requests_per_second > stats_.peak_requests_per_second) {
            stats_.peak_requests_per_second = stats_.requests_per_second;
        }
    }
}

void Server::update_system_stats() {
    // 简化实现：不实际获取系统资源信息
    // 未来可以集成系统监控库
}

void Server::stats_collection_loop() {
    while (stats_running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(config_.monitor_config.stats_interval));
        
        if (!stats_running_.load()) {
            break;
        }
        
        // 更新各种统计信息
        update_connection_stats();
        update_request_stats();
        update_system_stats();
        
        // 清理旧的速率限制记录（超过1分钟未活动的IP）
        {
            auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lock(rate_limit_mutex_);
            for (auto it = rate_limit_map_.begin(); it != rate_limit_map_.end();) {
                auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(
                    now - it->second.window_start).count();
                if (elapsed > 1) {
                    it = rate_limit_map_.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
}

bool Server::validate_connection(const std::string& client_ip, int client_port) const {
    (void)client_port;
    
    // 检查 IP 白名单
    if (!config_.security_config.allow_ips.empty()) {
        bool allowed = false;
        for (const auto& allowed_ip : config_.security_config.allow_ips) {
            if (client_ip == allowed_ip) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            return false;
        }
    }
    
    // 检查 IP 黑名单
    for (const auto& denied_ip : config_.security_config.deny_ips) {
        if (client_ip == denied_ip) {
            return false;
        }
    }
    
    // 检查每IP最大连接数
    if (config_.security_config.max_connections_per_ip > 0) {
        std::lock_guard<std::mutex> lock(ip_connections_mutex_);
        auto it = ip_connections_.find(client_ip);
        size_t current_connections = (it != ip_connections_.end()) ? it->second : 0;
        if (current_connections >= static_cast<size_t>(config_.security_config.max_connections_per_ip)) {
            return false;
        }
    }
    
    // 使用连接过滤器
    if (connection_filter_) {
        return connection_filter_(client_ip, client_port);
    }
    
    return true;
}

bool Server::check_rate_limit(const std::string& client_ip) const {
    if (!config_.security_config.enable_ratelimit) {
        return true;
    }
    
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(rate_limit_mutex_);
    
    auto it = rate_limit_map_.find(client_ip);
    if (it == rate_limit_map_.end()) {
        // 创建新条目
        RateLimitEntry entry;
        entry.request_count = 1;
        entry.window_start = now;
        rate_limit_map_[client_ip] = entry;
        return true;
    }
    
    auto& entry = it->second;
    
    // 检查时间窗口（每秒）
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - entry.window_start).count();
    
    if (elapsed >= 1) {
        // 新的一秒，重置计数
        entry.request_count = 0;
        entry.window_start = now;
    }
    
    // 检查是否超过限制
    if (entry.request_count >= static_cast<uint64_t>(config_.security_config.max_requests_per_second)) {
        return false;
    }
    
    entry.request_count++;
    return true;
}

Server::CommandResult Server::admin_info(const std::vector<std::string>& /*args*/) {
    CommandResult cr;
    cr.success = true;
    cr.message = "OK";
    cr.data = config_.to_string();
    return cr;
}

Server::CommandResult Server::admin_clients(const std::vector<std::string>& /*args*/) {
    CommandResult cr;
    cr.success = true;
    cr.message = "OK";
    cr.data = "clients=" + std::to_string(
        connection_manager_ ? connection_manager_->connection_count() : 0);
    return cr;
}

Server::CommandResult Server::admin_stats(const std::vector<std::string>& /*args*/) {
    CommandResult cr;
    cr.success = true;
    cr.message = "OK";
    cr.data = get_statistics().to_string();
    return cr;
}

Server::CommandResult Server::admin_config(const std::vector<std::string>& /*args*/) {
    CommandResult cr;
    cr.success = true;
    cr.message = "OK";
    cr.data = config_.to_string();
    return cr;
}

Server::CommandResult Server::admin_shutdown(const std::vector<std::string>& /*args*/) {
    CommandResult cr;
    cr.success = true;
    cr.message = "SHUTTING_DOWN";
    cr.data = "";
    stop(true, 0);
    return cr;
}

Server::CommandResult Server::admin_reload(const std::vector<std::string>& /*args*/) {
    CommandResult cr;
    cr.success = true;
    cr.message = "RELOAD_NOT_IMPLEMENTED";
    cr.data = "";
    return cr;
}

Server::CommandResult Server::admin_diagnostics(const std::vector<std::string>& /*args*/) {
    CommandResult cr;
    cr.success = true;
    cr.message = "OK";
    cr.data = "health=" + health_check().status;
    return cr;
}

std::string Server::generate_server_id() const {
    std::ostringstream oss;
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    oss << std::hex << dist(gen);
    return oss.str();
}

void Server::set_state(State new_state) {
    state_.store(new_state);
    state_cv_.notify_all();
}

void Server::safe_invoke_callback(const std::function<void()>& callback) {
    try {
        callback();
    } catch (const std::exception& e) {
        LOG_ERROR("NET", std::string("Server callback threw exception: ") + e.what());
        if (exception_handler_) {
            try {
                exception_handler_(e);
            } catch (...) {
                // ignore
            }
        }
    } catch (...) {
        LOG_ERROR("NET", "Server callback threw unknown exception");
    }
}

} // namespace net
} // namespace mementodb


