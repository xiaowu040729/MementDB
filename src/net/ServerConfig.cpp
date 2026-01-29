// File: src/net/ServerConfig.cpp
#include "ServerConfig.hpp"

#include <sstream>

namespace mementodb {
namespace net {

bool ServerConfig::validate() const {
    if (!listen_configs.empty()) {
        for (const auto& lc : listen_configs) {
            if (lc.port <= 0 || lc.port > 65535) return false;
        }
    }
    if (connection_config.max_connections <= 0) return false;
    if (thread_config.worker_threads <= 0) return false;
    return true;
}

std::string ServerConfig::to_string() const {
    std::ostringstream oss;
    oss << "ServerConfig{";
    oss << "server_name=" << server_name
        << ", version=" << server_version
        << ", listen_endpoints=" << listen_configs.size()
        << ", max_connections=" << connection_config.max_connections
        << ", worker_threads=" << thread_config.worker_threads
        << "}";
    return oss.str();
}

} // namespace net
} // namespace mementodb

