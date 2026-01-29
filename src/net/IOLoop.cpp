// File: src/net/IOLoop.cpp

#include "IOLoop.hpp"
#include "EpollLoop.hpp"

namespace mementodb {
namespace net {

std::unique_ptr<IOLoop> IOLoop::create_default() {
    return create("epoll");
}

std::unique_ptr<IOLoop> IOLoop::create(const std::string& type) {
    if (type == "epoll" || type.empty()) {
        return std::make_unique<EpollLoop>();
    }
    // 其他实现类型暂不支持
    return nullptr;
}

std::string io_event_to_string(IOEvent events) {
    std::string result;
    if (io_event_contains(events, IOEvent::READ))     result += "READ|";
    if (io_event_contains(events, IOEvent::WRITE))    result += "WRITE|";
    if (io_event_contains(events, IOEvent::ERROR))    result += "ERROR|";
    if (io_event_contains(events, IOEvent::HUP))      result += "HUP|";
    if (io_event_contains(events, IOEvent::PRI))      result += "PRI|";
    if (io_event_contains(events, IOEvent::RDHUP))    result += "RDHUP|";
    if (io_event_contains(events, IOEvent::ET))       result += "ET|";
    if (result.empty()) return "NONE";
    result.pop_back(); // 去掉最后一个'|'
    return result;
}

bool io_event_contains(IOEvent events, IOEvent target) {
    return (static_cast<uint32_t>(events) & static_cast<uint32_t>(target)) != 0;
}

} // namespace net
} // namespace mementodb

