// File: src/net/EpollLoop.cpp

#include "EpollLoop.hpp"
#include "../utils/LoggingSystem/LogMacros.hpp"
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>

namespace mementodb {
namespace net {

EpollLoop::EpollLoop()
    : epoll_fd_(-1),
      wakeup_fd_(-1),
      running_(false),
      stopping_(false),
      timer_fd_(-1),
      next_timer_id_(1),
      signal_fd_(-1),
      max_events_per_cycle_(64),
      epoll_timeout_ms_(1000),
      priority_(0) {
    last_cycle_time_ = std::chrono::steady_clock::now();
    stats_ = Statistics{};
}

EpollLoop::~EpollLoop() {
    stop();
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
    }
    if (wakeup_fd_ >= 0) {
        ::close(wakeup_fd_);
    }
    if (timer_fd_ >= 0) {
        ::close(timer_fd_);
    }
    if (signal_fd_ >= 0) {
        ::close(signal_fd_);
    }
}

bool EpollLoop::init(const Options& options) {
    name_ = options.name;
    max_events_per_cycle_ = options.max_events_per_cycle > 0 ? options.max_events_per_cycle : 64;
    epoll_timeout_ms_ = options.default_timeout_ms > 0 ? options.default_timeout_ms : 1000;

    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        LOG_ERROR("NET", "Failed to create epoll instance: " + std::string(std::strerror(errno)));
        last_error_ = std::error_code(errno, std::generic_category());
        return false;
    }

    loop_thread_id_ = std::this_thread::get_id();

    if (!init_wakeup_fd()) return false;
    if (options.enable_timers && !init_timer_fd()) return false;
    if (options.enable_signals && !init_signal_fd()) return false;

    if (options.enable_timers) {
        timer_manager_ = std::make_unique<TimerManager>(this);
    }

    running_.store(false);
    stopping_.store(false);
    return true;
}

bool EpollLoop::add_fd(int fd, IOEvent events, IOEventCallback callback, void* user_data) {
    if (fd < 0) return false;

    uint32_t ev = to_epoll_events(events);
    struct epoll_event e{};
    e.events = ev;
    e.data.fd = fd;

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &e) != 0) {
        LOG_ERROR("NET", "epoll_ctl ADD failed: " + std::string(std::strerror(errno)));
        last_error_ = std::error_code(errno, std::generic_category());
        return false;
    }

    auto ctx = std::make_shared<FdContext>();
    ctx->events = events;
    ctx->callback = std::move(callback);
    ctx->user_data = user_data;
    ctx->removed = false;
    ctx->last_active = now_ms();

    {
        std::unique_lock lock(fd_mutex_);
        fd_contexts_[fd] = std::move(ctx);
    }

    return true;
}

bool EpollLoop::modify_fd(int fd, IOEvent events) {
    if (fd < 0) return false;

    uint32_t ev = to_epoll_events(events);
    struct epoll_event e{};
    e.events = ev;
    e.data.fd = fd;

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &e) != 0) {
        LOG_ERROR("NET", "epoll_ctl MOD failed: " + std::string(std::strerror(errno)));
        last_error_ = std::error_code(errno, std::generic_category());
        return false;
    }

    std::shared_lock lock(fd_mutex_);
    auto it = fd_contexts_.find(fd);
    if (it != fd_contexts_.end()) {
        it->second->events = events;
    }

    return true;
}

bool EpollLoop::remove_fd(int fd) {
    if (fd < 0) return false;

    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);

    std::unique_lock lock(fd_mutex_);
    auto it = fd_contexts_.find(fd);
    if (it != fd_contexts_.end()) {
        it->second->removed = true;
        fd_contexts_.erase(it);
    }

    return true;
}

bool EpollLoop::run(int /*timeout_ms*/) {
    if (running_.exchange(true)) {
        // 已经在运行
        return false;
    }

    stopping_.store(false);

    const int max_events = static_cast<int>(max_events_per_cycle_);
    std::vector<struct epoll_event> events(max_events);

    while (!stopping_.load()) {
        int timeout = epoll_timeout_ms_;
        int n = ::epoll_wait(epoll_fd_, events.data(), max_events, timeout);

        auto start = std::chrono::steady_clock::now();
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG_ERROR("NET", "epoll_wait error: " + std::string(std::strerror(errno)));
            last_error_ = std::error_code(errno, std::generic_category());
            break;
        }

        if (n > 0) {
            handle_events(n, events.data());
        }

        process_pending_tasks();

        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        update_cycle_statistics(duration);
    }

    running_.store(false);
    return !static_cast<bool>(last_error_);
}

int EpollLoop::run_once(int timeout_ms) {
    const int max_events = static_cast<int>(max_events_per_cycle_);
    std::vector<struct epoll_event> events(max_events);

    int n = ::epoll_wait(epoll_fd_, events.data(), max_events,
                         timeout_ms > 0 ? timeout_ms : epoll_timeout_ms_);
    if (n < 0) {
        if (errno != EINTR) {
            last_error_ = std::error_code(errno, std::generic_category());
        }
        return -1;
    }

    if (n > 0) {
        handle_events(n, events.data());
    }
    process_pending_tasks();
    return n;
}

void EpollLoop::stop(bool /*graceful*/) {
    stopping_.store(true);
    wakeup();
}

bool EpollLoop::is_running() const {
    return running_.load();
}

bool EpollLoop::run_in_loop(TaskCallback task) {
    if (!task) return false;

    {
        std::lock_guard<std::mutex> lock(task_mutex_);
        pending_tasks_.push(std::move(task));
    }
    wakeup();
    return true;
}

bool EpollLoop::run_after(TaskCallback task, uint64_t delay_ms) {
    TimerOptions opt;
    return add_timer(delay_ms, 0, [t = std::move(task)](void*) { if (t) t(); }, opt) != 0;
}

EpollLoop::TimerId EpollLoop::run_every(TaskCallback task, uint64_t interval_ms) {
    TimerOptions opt;
    opt.repeated = true;
    return add_timer(interval_ms, interval_ms, [t = std::move(task)](void*) { if (t) t(); }, opt);
}

EpollLoop::TimerId EpollLoop::add_timer(uint64_t delay_ms,
                                        uint64_t interval_ms,
                                        TimerCallback callback,
                                        const TimerOptions& options) {
    if (!callback || timer_fd_ < 0 || !timer_manager_) {
        return 0;
    }

    auto info = std::make_shared<TimerInfo>();
    info->id = next_timer_id_.fetch_add(1, std::memory_order_relaxed);
    info->callback = std::move(callback);
    info->interval = interval_ms;
    info->repeated = options.repeated || (interval_ms > 0);
    info->user_data = options.user_data;
    info->cancelled = false;

    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    info->expire_time = now_ms + delay_ms;

    {
        std::lock_guard<std::mutex> lock(timer_mutex_);
        timers_[info->id] = info;
    }

    timer_manager_->add_timer(*info);
    wakeup();
    return info->id;
}

bool EpollLoop::cancel_timer(TimerId timer_id) {
    if (timer_id == 0) return false;

    std::lock_guard<std::mutex> lock(timer_mutex_);
    auto it = timers_.find(timer_id);
    if (it != timers_.end()) {
        it->second->cancelled = true;
        timers_.erase(it);
        return true;
    }
    return false;
}

EpollLoop::SignalId EpollLoop::add_signal(int signum,
                           std::function<void(int, void*)> callback,
                           const SignalOptions& options) {
    if (signum <= 0 || !callback) return false;

    std::lock_guard<std::mutex> lock(signal_mutex_);
    signal_handlers_[signum] = SignalInfo{signum, [cb = std::move(callback), ud = options.user_data](int s) {
        cb(s, ud);
    }, options};
    return static_cast<SignalId>(signum);
}

bool EpollLoop::remove_signal(SignalId signal_id) {
    std::lock_guard<std::mutex> lock(signal_mutex_);
    return signal_handlers_.erase(static_cast<int>(signal_id)) > 0;
}

EpollLoop::Statistics EpollLoop::get_statistics() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

std::string EpollLoop::get_name() const {
    return name_;
}

bool EpollLoop::set_priority(int priority) {
    priority_ = priority;
    // TODO: 设置线程实时优先级
    return true;
}

void EpollLoop::set_max_events_per_cycle(size_t max_events) {
    max_events_per_cycle_ = max_events > 0 ? max_events : 64;
}

void EpollLoop::set_epoll_timeout(int timeout_ms) {
    epoll_timeout_ms_ = timeout_ms;
}

bool EpollLoop::is_in_loop_thread() const {
    return std::this_thread::get_id() == loop_thread_id_;
}

void EpollLoop::wakeup() {
    if (wakeup_fd_ < 0) return;
    uint64_t one = 1;
    ::write(wakeup_fd_, &one, sizeof(one));
}

void EpollLoop::set_exception_handler(ExceptionHandler handler) {
    exception_handler_ = std::move(handler);
}

bool EpollLoop::init_wakeup_fd() {
    wakeup_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeup_fd_ < 0) {
        LOG_ERROR("NET", "Failed to create eventfd: " + std::string(std::strerror(errno)));
        return false;
    }

    struct epoll_event e{};
    e.events = EPOLLIN;
    e.data.fd = wakeup_fd_;

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wakeup_fd_, &e) != 0) {
        LOG_ERROR("NET", "Failed to add wakeup fd to epoll: " + std::string(std::strerror(errno)));
        return false;
    }

    return true;
}

bool EpollLoop::init_timer_fd() {
    timer_fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd_ < 0) {
        LOG_ERROR("NET", "Failed to create timerfd: " + std::string(std::strerror(errno)));
        return false;
    }

    struct epoll_event e{};
    e.events = EPOLLIN;
    e.data.fd = timer_fd_;

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, timer_fd_, &e) != 0) {
        LOG_ERROR("NET", "Failed to add timer fd to epoll: " + std::string(std::strerror(errno)));
        return false;
    }

    return true;
}

bool EpollLoop::init_signal_fd() {
    // 简化：暂不真正初始化 signalfd，只保留接口
    signal_fd_ = -1;
    return true;
}

void EpollLoop::process_events() {
    // 目前逻辑直接在 run() 中处理，此处预留扩展点
}

void EpollLoop::handle_events(int num_events, struct epoll_event* events) {
    for (int i = 0; i < num_events; ++i) {
        int fd = events[i].data.fd;
        uint32_t ev = events[i].events;

        if (fd == wakeup_fd_) {
            handle_wakeup();
        } else if (fd == timer_fd_) {
            handle_timer();
        } else if (fd == signal_fd_) {
            handle_signal();
        } else {
            handle_fd_event(fd, ev);
        }
    }
}

void EpollLoop::handle_fd_event(int fd, uint32_t events) {
    std::shared_ptr<FdContext> ctx;
    {
        std::shared_lock lock(fd_mutex_);
        auto it = fd_contexts_.find(fd);
        if (it != fd_contexts_.end()) {
            ctx = it->second;
        }
    }

    if (!ctx || ctx->removed) {
        return;
    }

    IOEvent ioe = from_epoll_events(events);

    safe_execute([&]() {
        if (ctx->callback) {
            ctx->callback(fd, ioe, ctx->user_data);
        }
    });
}

void EpollLoop::handle_wakeup() {
    uint64_t one;
    ::read(wakeup_fd_, &one, sizeof(one));
}

void EpollLoop::handle_timer() {
    if (timer_fd_ < 0 || !timer_manager_) return;

    uint64_t expirations;
    ::read(timer_fd_, &expirations, sizeof(expirations));

    timer_manager_->process_timers();
}

void EpollLoop::handle_signal() {
    if (signal_fd_ < 0) return;
    // TODO: 读取 signalfd 并分发到各个回调
}

void EpollLoop::process_pending_tasks() {
    std::queue<TaskCallback> tasks;
    {
        std::lock_guard<std::mutex> lock(task_mutex_);
        std::swap(tasks, pending_tasks_);
    }

    while (!tasks.empty()) {
        TaskCallback task = std::move(tasks.front());
        tasks.pop();
        execute_task(std::move(task));
    }
}

void EpollLoop::execute_task(TaskCallback task) {
    safe_execute([&]() {
        if (task) task();
    });
}

void EpollLoop::update_cycle_statistics(std::chrono::microseconds cycle_time) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.cycles++;
    stats_.total_cycle_time += cycle_time;
    if (cycle_time > stats_.max_cycle_time) {
        stats_.max_cycle_time = cycle_time;
    }
}

void EpollLoop::cleanup_removed_fds() {
    std::unique_lock lock(fd_mutex_);
    for (auto it = fd_contexts_.begin(); it != fd_contexts_.end();) {
        if (it->second->removed) {
            it = fd_contexts_.erase(it);
        } else {
            ++it;
        }
    }
}

uint32_t EpollLoop::to_epoll_events(IOEvent events) const {
    uint32_t ev = 0;
    if (io_event_contains(events, IOEvent::READ))   ev |= EPOLLIN;
    if (io_event_contains(events, IOEvent::WRITE))  ev |= EPOLLOUT;
    if (io_event_contains(events, IOEvent::ERROR))  ev |= EPOLLERR;
    if (io_event_contains(events, IOEvent::HUP))    ev |= EPOLLHUP;
    if (io_event_contains(events, IOEvent::PRI))    ev |= EPOLLPRI;
    if (io_event_contains(events, IOEvent::RDHUP))  ev |= EPOLLRDHUP;
    if (io_event_contains(events, IOEvent::ET))     ev |= EPOLLET;
    return ev;
}

IOEvent EpollLoop::from_epoll_events(uint32_t events) const {
    IOEvent e = IOEvent::NONE;
    if (events & EPOLLIN)     e |= IOEvent::READ;
    if (events & EPOLLOUT)    e |= IOEvent::WRITE;
    if (events & EPOLLERR)    e |= IOEvent::ERROR;
    if (events & EPOLLHUP)    e |= IOEvent::HUP;
    if (events & EPOLLPRI)    e |= IOEvent::PRI;
    if (events & EPOLLRDHUP)  e |= IOEvent::RDHUP;
    return e;
}

template<typename Func>
void EpollLoop::safe_execute(Func&& func) {
    try {
        func();
    } catch (const std::exception& e) {
        if (exception_handler_) {
            exception_handler_(e);
        } else {
            LOG_ERROR("NET", std::string("Exception in EpollLoop: ") + e.what());
        }
    } catch (...) {
        LOG_ERROR("NET", "Unknown exception in EpollLoop");
    }
}

// IOLoop 保护接口实现
void EpollLoop::notify_stop() {
    stop(false);
}

void EpollLoop::update_statistics(const Statistics& delta) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.fd_count         += delta.fd_count;
    stats_.timer_count      += delta.timer_count;
    stats_.signal_count     += delta.signal_count;
    stats_.processed_events += delta.processed_events;
    stats_.pending_tasks    += delta.pending_tasks;
    stats_.cycles           += delta.cycles;
    stats_.total_cycle_time += delta.total_cycle_time;
    if (delta.max_cycle_time > stats_.max_cycle_time) {
        stats_.max_cycle_time = delta.max_cycle_time;
    }
}

void EpollLoop::reset_statistics() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = Statistics{};
    stats_.start_time = std::chrono::steady_clock::now();
}

uint64_t EpollLoop::now_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool EpollLoop::supports(IOEvent events) const {
    // epoll 支持所有基本事件类型
    uint32_t ev = static_cast<uint32_t>(events);
    // 检查是否包含不支持的事件（目前都支持）
    return true;
}

const char* EpollLoop::implementation_name() const {
    return "epoll";
}

std::error_code EpollLoop::last_error() const {
    return last_error_;
}

void EpollLoop::set_debug_logger(DebugLogCallback callback) {
    debug_logger_ = std::move(callback);
}

IOEvent EpollLoop::get_fd_events(int fd) const {
    std::shared_lock lock(fd_mutex_);
    auto it = fd_contexts_.find(fd);
    if (it != fd_contexts_.end()) {
        return it->second->events;
    }
    return IOEvent::NONE;
}

bool EpollLoop::set_fd_user_data(int fd, void* user_data) {
    std::unique_lock lock(fd_mutex_);
    auto it = fd_contexts_.find(fd);
    if (it != fd_contexts_.end()) {
        it->second->user_data = user_data;
        return true;
    }
    return false;
}

void* EpollLoop::get_fd_user_data(int fd) const {
    std::shared_lock lock(fd_mutex_);
    auto it = fd_contexts_.find(fd);
    if (it != fd_contexts_.end()) {
        return it->second->user_data;
    }
    return nullptr;
}

void EpollLoop::clear_all_fds() {
    std::unique_lock lock(fd_mutex_);
    for (auto& [fd, ctx] : fd_contexts_) {
        if (fd != wakeup_fd_ && fd != timer_fd_ && fd != signal_fd_) {
            ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        }
    }
    fd_contexts_.clear();
    // 重新添加系统fd
    if (wakeup_fd_ >= 0) {
        struct epoll_event e{};
        e.events = EPOLLIN;
        e.data.fd = wakeup_fd_;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wakeup_fd_, &e);
    }
    if (timer_fd_ >= 0) {
        struct epoll_event e{};
        e.events = EPOLLIN;
        e.data.fd = timer_fd_;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, timer_fd_, &e);
    }
    if (signal_fd_ >= 0) {
        struct epoll_event e{};
        e.events = EPOLLIN;
        e.data.fd = signal_fd_;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, signal_fd_, &e);
    }
}

void EpollLoop::clear_all_timers() {
    std::lock_guard<std::mutex> lock(timer_mutex_);
    // 标记所有定时器为取消
    for (auto& [id, timer] : timers_) {
        timer->cancelled = true;
    }
    timers_.clear();
    // TimerManager 会在下次 process_timers 时清理已取消的定时器
}

bool EpollLoop::wait_for_pending_tasks(uint64_t timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + 
                    std::chrono::milliseconds(timeout_ms);
    
    std::unique_lock<std::mutex> lock(task_mutex_);
    while (!pending_tasks_.empty()) {
        if (timeout_ms > 0) {
            if (task_cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                return false;
            }
        } else {
            task_cv_.wait(lock);
        }
    }
    return true;
}

// ==================== TimerManager ====================

TimerManager::TimerManager(EpollLoop* loop)
    : loop_(loop) {
}

TimerManager::~TimerManager() = default;

EpollLoop::TimerId TimerManager::add_timer(const EpollLoop::TimerInfo& info) {
    auto timer = std::make_shared<EpollLoop::TimerInfo>(info);
    std::lock_guard<std::mutex> lock(timer_mutex_);
    timer_queue_.push(timer);
    return timer->id;
}

bool TimerManager::cancel_timer(EpollLoop::TimerId timer_id) {
    std::lock_guard<std::mutex> lock(timer_mutex_);
    // 简化：标记取消，实际清理在 process_timers 中进行
    std::priority_queue<std::shared_ptr<EpollLoop::TimerInfo>,
                        std::vector<std::shared_ptr<EpollLoop::TimerInfo>>,
                        TimerCompare> new_queue;

    bool cancelled = false;
    while (!timer_queue_.empty()) {
        auto t = timer_queue_.top();
        timer_queue_.pop();
        if (t->id == timer_id) {
            t->cancelled = true;
            cancelled = true;
            continue;
        }
        new_queue.push(t);
    }

    std::swap(timer_queue_, new_queue);
    return cancelled;
}

void TimerManager::process_timers() {
    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    std::vector<std::shared_ptr<EpollLoop::TimerInfo>> expired;

    {
        std::lock_guard<std::mutex> lock(timer_mutex_);
        while (!timer_queue_.empty()) {
            auto t = timer_queue_.top();
            if (t->expire_time > now_ms) break;
            timer_queue_.pop();
            if (!t->cancelled) {
                expired.push_back(t);
            }
        }
    }

    for (auto& t : expired) {
        loop_->run_in_loop([cb = t->callback, ud = t->user_data]() {
            if (cb) cb(ud);
        });
        if (t->repeated && !t->cancelled) {
            t->expire_time = now_ms + t->interval;
            add_timer(*t);
        }
    }
}

uint64_t TimerManager::get_next_timeout() const {
    std::lock_guard<std::mutex> lock(timer_mutex_);
    if (timer_queue_.empty()) return 0;
    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    auto t = timer_queue_.top();
    if (t->expire_time <= now_ms) return 0;
    return t->expire_time - now_ms;
}

// ==================== SignalHandler ====================

SignalHandler::SignalHandler(EpollLoop* loop)
    : loop_(loop) {
    sigemptyset(&signal_mask_);
}

SignalHandler::~SignalHandler() = default;

bool SignalHandler::add_signal(int signum,
                               std::function<void(int)> callback,
                               const EpollLoop::SignalOptions& options) {
    (void)callback;
    (void)options;
    sigaddset(&signal_mask_, signum);
    return true;
}

bool SignalHandler::remove_signal(int signum) {
    sigdelset(&signal_mask_, signum);
    return true;
}

void SignalHandler::process_signals() {
    // TODO: 读取 signalfd 并分发到各个回调
}

} // namespace net
} // namespace mementodb

