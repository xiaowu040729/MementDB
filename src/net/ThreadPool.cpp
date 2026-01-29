// File: src/net/ThreadPool.cpp

#include "ThreadPool.hpp"
#include "../utils/LoggingSystem/LogMacros.hpp"

#include <deque>

namespace mementodb {
namespace net {

// ===== TaskResult =====

template<typename ResultType>
ResultType TaskResult<ResultType>::get() const {
    if (error) {
        std::rethrow_exception(error);
    }
    return value;
}

// ===== ThreadPoolStatistics =====

void ThreadPoolStatistics::reset() {
    total_threads = 0;
    active_threads = 0;
    idle_threads = 0;
    max_threads = 0;
    total_tasks_submitted = 0;
    total_tasks_completed = 0;
    total_tasks_failed = 0;
    pending_tasks = 0;
    queue_capacity = 0;
    queue_usage = 0;
    total_busy_time = std::chrono::microseconds{0};
    max_task_time = std::chrono::microseconds{0};
    avg_task_time = std::chrono::microseconds{0};
    start_time = std::chrono::steady_clock::now();
}

std::string ThreadPoolStatistics::to_string() const {
    return "ThreadPoolStatistics{threads=" + std::to_string(total_threads) +
           ", active=" + std::to_string(active_threads) +
           ", pending=" + std::to_string(pending_tasks) + "}";
}

// ===== ThreadPoolConfig =====

bool ThreadPoolConfig::validate() const {
    if (min_threads == 0) return false;
    if (max_threads == 0) return false;
    if (min_threads > max_threads) return false;
    if (init_threads != 0 &&
        (init_threads < min_threads || init_threads > max_threads)) {
        return false;
    }
    if (enable_batch_processing && batch_size == 0) return false;
    return true;
}

ThreadPoolConfig ThreadPoolConfig::default_config() {
    ThreadPoolConfig cfg;
    if (cfg.max_threads == 0) {
        cfg.max_threads = 4;
    }
    cfg.init_threads = cfg.min_threads;
    return cfg;
}

// ===== TaskWrapper =====

std::atomic<TaskWrapper::TaskId> TaskWrapper::next_id_{1};

TaskWrapper::TaskWrapper()
    : id_(next_id_++),
      task_(nullptr),
      priority_(TaskPriority::NORMAL),
      name_(""),
      submit_time_(std::chrono::steady_clock::now()) {}

TaskWrapper::TaskWrapper(std::function<void()> task,
                         TaskPriority priority,
                         std::string name)
    : id_(next_id_++),
      task_(std::move(task)),
      priority_(priority),
      name_(std::move(name)),
      submit_time_(std::chrono::steady_clock::now()) {}

void TaskWrapper::operator()() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (completed_ || cancelled_ || !task_) {
            completed_ = true;
            cv_.notify_all();
            return;
        }
        start_time_ = std::chrono::steady_clock::now();
    }

    try {
        task_();
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        exception_ = std::current_exception();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        end_time_ = std::chrono::steady_clock::now();
        completed_ = true;
    }
    cv_.notify_all();
}

bool TaskWrapper::cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (completed_) return false;
    cancelled_ = true;
    return true;
}

void TaskWrapper::wait() const {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return completed_.load(); });
}

bool TaskWrapper::is_done() const {
    return completed_.load();
}

// ===== ThreadPool internals =====

class ThreadPool::TaskQueue {
public:
    explicit TaskQueue(const ThreadPoolConfig& config)
        : config_(config) {}

    bool enqueue(const std::shared_ptr<TaskWrapper>& task) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (config_.max_queue_size > 0 &&
            tasks_.size() >= config_.max_queue_size) {
            return false;
        }
        tasks_.push_back(task);
        lock.unlock();
        cv_.notify_one();
        return true;
    }

    std::shared_ptr<TaskWrapper> dequeue() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return stopping_ || !tasks_.empty(); });
        if (stopping_ && tasks_.empty()) {
            return nullptr;
        }
        auto task = tasks_.front();
        tasks_.pop_front();
        return task;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tasks_.size();
    }

private:
    const ThreadPoolConfig& config_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<TaskWrapper>> tasks_;
    bool stopping_{false};
};

class ThreadPool::Worker {
public:
    Worker(ThreadPool* pool, size_t index)
        : pool_(pool), index_(index) {}

    void start();
    void join();

    std::thread::id id() const { return thread_.get_id(); }

    uint64_t tasks_processed() const { return tasks_processed_; }

private:
    void run();

    ThreadPool* pool_;
    size_t index_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    uint64_t tasks_processed_{0};

    friend class ThreadPool;
};

void ThreadPool::Worker::start() {
    running_ = true;
    thread_ = std::thread(&Worker::run, this);
}

void ThreadPool::Worker::join() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

void ThreadPool::Worker::run() {
    if (pool_->thread_init_func_) {
        pool_->safe_invoke_callback([this] { pool_->thread_init_func_(thread_.get_id()); });
    }

    while (pool_->state_.load() == State::RUNNING) {
        auto task = pool_->dequeue_task(this);
        if (!task) {
            if (pool_->state_.load() != State::RUNNING) break;
            continue;
        }
        pool_->process_task(task, this);
        ++tasks_processed_;
    }

    if (pool_->thread_destroy_func_) {
        pool_->safe_invoke_callback([this] { pool_->thread_destroy_func_(thread_.get_id()); });
    }
}

// ===== ThreadPool =====

std::atomic<ThreadPool::TaskId> ThreadPool::next_task_id_{1};

ThreadPool::ThreadPool(const ThreadPoolConfig& config)
    : config_(config),
      state_(State::CREATED),
      paused_(false) {
    if (!config_.validate()) {
        throw std::invalid_argument("Invalid ThreadPoolConfig");
    }
    if (config_.init_threads == 0) {
        config_.init_threads = config_.min_threads;
    }
    task_queue_ = std::make_unique<TaskQueue>(config_);
    stats_.start_time = std::chrono::steady_clock::now();
}

ThreadPool::~ThreadPool() {
    try {
        stop(true, 0);
    } catch (...) {
    }
}

bool ThreadPool::init() {
    std::unique_lock<std::shared_mutex> lock(workers_mutex_);
    if (!workers_.empty()) return true;

    workers_.reserve(config_.max_threads);
    for (size_t i = 0; i < config_.init_threads; ++i) {
        workers_.emplace_back(std::make_unique<Worker>(this, i));
    }
    return true;
}

bool ThreadPool::start() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (state_ != State::CREATED && state_ != State::STOPPED) {
        return false;
    }

    if (!init()) {
        state_ = State::ERROR;
        return false;
    }

    state_ = State::RUNNING;

    {
        std::shared_lock<std::shared_mutex> wlock(workers_mutex_);
        for (auto& w : workers_) {
            w->start();
        }
        stats_.total_threads = workers_.size();
        stats_.max_threads = std::max(stats_.max_threads, stats_.total_threads);
    }

    return true;
}

void ThreadPool::stop(bool graceful, uint64_t /*timeout_ms*/) {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (state_ == State::STOPPING || state_ == State::STOPPED) {
            return;
        }
        state_ = State::STOPPING;
    }

    if (graceful) {
        while (task_queue_->size() > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    task_queue_->stop();

    {
        std::shared_lock<std::shared_mutex> wlock(workers_mutex_);
        for (auto& w : workers_) {
            w->join();
        }
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_ = State::STOPPED;
    }
    notify_state_change();
}

void ThreadPool::pause() {
    paused_.store(true);
}

void ThreadPool::resume() {
    paused_.store(false);
    pause_cv_.notify_all();
}

ThreadPool::TaskId ThreadPool::submit(Task task,
                                      TaskPriority priority,
                                      const std::string& name) {
    if (!task) return 0;

    if (state_.load() != State::RUNNING) {
        return 0;
    }

    auto wrapper = std::make_shared<TaskWrapper>(std::move(task), priority, name);
    auto id = wrapper->id();

    if (!enqueue_task(wrapper)) {
    return 0;
}

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.total_tasks_submitted;
        stats_.pending_tasks = task_queue_->size();
    }

    return id;
}

std::vector<ThreadPool::TaskId> ThreadPool::submit_batch(const std::vector<Task>& tasks,
                                                         TaskPriority priority) {
    std::vector<TaskId> ids;
    ids.reserve(tasks.size());
    for (const auto& t : tasks) {
        ids.push_back(submit(t, priority));
    }
    return ids;
}

ThreadPool::TaskId ThreadPool::submit_after(Task task,
                                            std::chrono::milliseconds delay,
                                            const std::string& name) {
    Task delayed = [task = std::move(task), delay]() {
        std::this_thread::sleep_for(delay);
        task();
    };
    return submit(std::move(delayed), TaskPriority::NORMAL, name);
}

ThreadPool::TaskId ThreadPool::submit_every(Task task,
                                            std::chrono::milliseconds interval,
                                            const std::string& name) {
    auto shared_pool = this->shared_from_this();
    Task periodic = [task = std::move(task), interval, wp = std::weak_ptr<ThreadPool>(shared_pool)]() mutable {
        while (auto sp = wp.lock()) {
            task();
            std::this_thread::sleep_for(interval);
            if (sp->state_.load() != State::RUNNING) break;
        }
    };
    return submit(std::move(periodic), TaskPriority::NORMAL, name);
}

bool ThreadPool::cancel_task(TaskId /*task_id*/, bool /*interrupt*/) {
    return false;
}

bool ThreadPool::wait_for_task(TaskId /*task_id*/, uint64_t /*timeout_ms*/) {
    return false;
}

void ThreadPool::wait_all(uint64_t /*timeout_ms*/) {
    while (task_queue_->size() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool ThreadPool::resize(size_t min_threads, size_t max_threads, bool /*immediate*/) {
    if (min_threads == 0 || max_threads == 0 || min_threads > max_threads) {
        return false;
    }
    config_.min_threads = min_threads;
    config_.max_threads = max_threads;
    return true;
}

bool ThreadPool::update_config(const ThreadPoolConfig& new_config) {
    if (!new_config.validate()) return false;
    config_ = new_config;
    return true;
}

ThreadPoolStatistics ThreadPool::get_statistics() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void ThreadPool::reset_statistics() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.reset();
}

ThreadPool::HealthStatus ThreadPool::health_check() const {
    HealthStatus hs;
    auto st = state_.load();
    hs.healthy = (st == State::RUNNING);
    hs.status = (hs.healthy ? "RUNNING" : "NOT_RUNNING");
    hs.details["pending_tasks"] = std::to_string(task_queue_->size());
    return hs;
}

std::vector<ThreadPool::ThreadInfo> ThreadPool::get_thread_infos() const {
    std::vector<ThreadInfo> infos;
    std::shared_lock<std::shared_mutex> lock(workers_mutex_);
    infos.reserve(workers_.size());
    for (const auto& w : workers_) {
        ThreadInfo info{};
        info.id = w->id();
        info.cpu_affinity = 0;
        info.name = "worker";
        info.start_time = stats_.start_time;
        info.tasks_processed = w->tasks_processed();
        info.is_busy = false;
        info.current_task_id = 0;
        info.current_task_runtime = std::chrono::microseconds{0};
        infos.push_back(info);
    }
    return infos;
}

std::vector<ThreadPool::TaskInfo> ThreadPool::get_task_infos() const {
    return {};
}

void ThreadPool::set_thread_init_func(ThreadInitFunc func) {
    thread_init_func_ = std::move(func);
}

void ThreadPool::set_thread_destroy_func(ThreadDestroyFunc func) {
    thread_destroy_func_ = std::move(func);
}

void ThreadPool::set_task_exception_handler(TaskExceptionHandler handler) {
    task_exception_handler_ = std::move(handler);
}

void ThreadPool::set_task_completion_handler(TaskCompletionHandler handler) {
    task_completion_handler_ = std::move(handler);
}

std::shared_ptr<ThreadPool> ThreadPool::create(const ThreadPoolConfig& config) {
    return std::make_shared<ThreadPool>(config);
}

bool ThreadPool::add_worker() {
    std::unique_lock<std::shared_mutex> lock(workers_mutex_);
    if (workers_.size() >= config_.max_threads) return false;
    size_t index = workers_.size();
    workers_.emplace_back(std::make_unique<Worker>(this, index));
    workers_.back()->start();
    ++stats_.total_threads;
    stats_.max_threads = std::max(stats_.max_threads, stats_.total_threads);
    return true;
}

bool ThreadPool::remove_worker(bool /*force*/) {
    return false;
}

void ThreadPool::adjust_worker_count() {
}

void ThreadPool::worker_main(Worker* /*worker*/) {
}

bool ThreadPool::enqueue_task(std::shared_ptr<TaskWrapper> task) {
    return task_queue_->enqueue(task);
}

std::shared_ptr<TaskWrapper> ThreadPool::dequeue_task(Worker* /*worker*/) {
    if (paused_.load()) {
        std::unique_lock<std::mutex> lock(state_mutex_);
        pause_cv_.wait(lock, [this] { return !paused_.load() || state_.load() != State::RUNNING; });
    }
    if (state_.load() != State::RUNNING) {
        return nullptr;
    }
    return task_queue_->dequeue();
}

void ThreadPool::process_task(std::shared_ptr<TaskWrapper> task, Worker* /*worker*/) {
    if (!task) return;
    auto start = std::chrono::steady_clock::now();
    try {
        (*task)();
        if (task_completion_handler_) {
            safe_invoke_callback([this, id = task->id()] { task_completion_handler_(id, true); });
        }
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.total_tasks_completed;
    } catch (const std::exception& ex) {
        if (task_exception_handler_) {
            safe_invoke_callback([this, id = task->id(), &ex] { task_exception_handler_(id, ex); });
        }
        if (task_completion_handler_) {
            safe_invoke_callback([this, id = task->id()] { task_completion_handler_(id, false); });
        }
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.total_tasks_failed;
    }
    auto end = std::chrono::steady_clock::now();
    auto dur = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.total_busy_time += dur;
    if (dur > stats_.max_task_time) {
        stats_.max_task_time = dur;
    }
}

bool ThreadPool::try_steal_work(Worker* /*thief*/, std::shared_ptr<TaskWrapper>& /*task*/) {
    return false;
}

void ThreadPool::update_statistics() {
}

void ThreadPool::stats_collection_loop() {
}

void ThreadPool::set_state(State new_state) {
    state_.store(new_state);
}

void ThreadPool::notify_state_change() {
    state_cv_.notify_all();
}

void ThreadPool::safe_invoke_callback(const std::function<void()>& callback) {
    try {
        callback();
    } catch (const std::exception& e) {
        LOG_ERROR("NET", std::string("ThreadPool callback threw exception: ") + e.what());
    } catch (...) {
        LOG_ERROR("NET", "ThreadPool callback threw unknown exception");
    }
}

bool ThreadPool::set_thread_affinity(std::thread& /*thread*/, size_t /*cpu_index*/) {
    return false;
}

namespace thread_pool_factory {

std::shared_ptr<ThreadPool> create_cpu_bound_pool() {
    ThreadPoolConfig cfg = ThreadPoolConfig::default_config();
    size_t cores = std::max<size_t>(1, std::thread::hardware_concurrency());
    cfg.min_threads = std::max<size_t>(2, cores);
    cfg.max_threads = cfg.min_threads;
    cfg.init_threads = cfg.min_threads;
    return ThreadPool::create(cfg);
}

std::shared_ptr<ThreadPool> create_io_bound_pool() {
    ThreadPoolConfig cfg = ThreadPoolConfig::default_config();
    size_t cores = std::max<size_t>(1, std::thread::hardware_concurrency());
    cfg.min_threads = cores * 2;
    cfg.max_threads = cores * 4;
    cfg.init_threads = cfg.min_threads;
    return ThreadPool::create(cfg);
}

std::shared_ptr<ThreadPool> create_single_thread_pool() {
    ThreadPoolConfig cfg = ThreadPoolConfig::default_config();
    cfg.min_threads = 1;
    cfg.max_threads = 1;
    cfg.init_threads = 1;
    return ThreadPool::create(cfg);
}

std::shared_ptr<ThreadPool> create_fixed_size_pool(size_t size) {
    ThreadPoolConfig cfg = ThreadPoolConfig::default_config();
    cfg.min_threads = size;
    cfg.max_threads = size;
    cfg.init_threads = size;
    return ThreadPool::create(cfg);
}

std::shared_ptr<ThreadPool> create_cached_pool() {
    ThreadPoolConfig cfg = ThreadPoolConfig::default_config();
    size_t cores = std::max<size_t>(1, std::thread::hardware_concurrency());
    cfg.min_threads = cores;
    cfg.max_threads = cores * 8;
    cfg.init_threads = cfg.min_threads;
    return ThreadPool::create(cfg);
}

} // namespace thread_pool_factory

} // namespace net
} // namespace mementodb


