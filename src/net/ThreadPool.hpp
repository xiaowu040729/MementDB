#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <functional>
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <atomic>
#include <future>
#include <memory>
#include <stdexcept>
#include <chrono>
#include <unordered_map>
#include <optional>
#include <system_error>
#include <type_traits>
#include <tuple>
#include <utility>
#include <string>

namespace mementodb {
namespace net {

// 线程池统计信息
struct ThreadPoolStatistics {
    size_t total_threads = 0;           // 总线程数
    size_t active_threads = 0;          // 活跃线程数
    size_t idle_threads = 0;            // 空闲线程数
    size_t max_threads = 0;             // 最大线程数历史
    uint64_t total_tasks_submitted = 0; // 提交的任务总数
    uint64_t total_tasks_completed = 0; // 完成的任务总数
    uint64_t total_tasks_failed = 0;    // 失败的任务总数
    uint64_t pending_tasks = 0;         // 等待中的任务数
    uint64_t queue_capacity = 0;        // 队列容量
    size_t queue_usage = 0;             // 队列使用量
    std::chrono::steady_clock::time_point start_time; // 启动时间
    std::chrono::microseconds total_busy_time{0};     // 总繁忙时间
    std::chrono::microseconds max_task_time{0};       // 最长任务执行时间
    std::chrono::microseconds avg_task_time{0};       // 平均任务执行时间
    
    // 重置统计
    void reset();
    // 转换为字符串表示
    std::string to_string() const;
};

// 任务优先级
enum class TaskPriority : uint8_t {
    LOW = 0,
    NORMAL = 1,
    HIGH = 2,
    CRITICAL = 3,
    MAX_PRIORITY = CRITICAL
};

// 任务执行策略
enum class TaskExecutionPolicy : uint8_t {
    FIFO = 0,        // 先入先出（默认）
    LIFO = 1,        // 后入先出（栈式）
    PRIORITY = 2     // 优先级队列
};

// 线程池配置
struct ThreadPoolConfig {
    // 线程配置
    size_t min_threads = 1;                                    // 最小线程数
    size_t max_threads = std::thread::hardware_concurrency();  // 最大线程数
    size_t init_threads = 0;                                   // 初始线程数（0表示使用min_threads）
    std::chrono::milliseconds thread_idle_timeout{60000};      // 线程空闲超时时间
    bool enable_thread_affinity = false;                       // 是否启用线程亲和性
    std::vector<size_t> cpu_affinities;                        // CPU亲和性绑定
    
    // 队列配置
    size_t max_queue_size = 10000;                             // 最大队列大小（0表示无限制）
    TaskExecutionPolicy execution_policy = TaskExecutionPolicy::FIFO; // 任务执行策略
    bool enable_priority = false;                              // 是否启用优先级（独立于execution_policy）
    
    // 性能配置
    bool enable_work_stealing = false;                         // 是否启用工作窃取
    size_t work_stealing_threshold = 10;                       // 工作窃取阈值
    bool enable_batch_processing = false;                      // 是否启用批处理
    size_t batch_size = 10;                                    // 批处理大小
    
    // 错误处理配置
    bool enable_task_exception_logging = true;                 // 是否启用任务异常日志
    size_t max_retry_attempts = 0;                             // 最大重试次数（0表示不重试）
    std::chrono::milliseconds retry_delay{100};                // 重试延迟
    
    // 监控配置
    bool enable_statistics = true;                             // 是否启用统计
    std::chrono::seconds statistics_interval{60};              // 统计收集间隔
    bool enable_health_check = true;                           // 是否启用健康检查
    
    // 验证配置
    bool validate() const;
    // 获取默认配置
    static ThreadPoolConfig default_config();
};

// 任务结果
template<typename ResultType>
struct TaskResult {
    bool success;               // 是否成功
    ResultType value;           // 结果值（如果成功）
    std::exception_ptr error;   // 异常指针（如果失败）
    std::chrono::microseconds execution_time;  // 执行时间
    size_t retry_count;         // 重试次数
    
    // 获取结果或抛出异常
    ResultType get() const;
    // 检查是否有异常
    bool has_error() const { return error != nullptr; }
};

// 任务包装器
class TaskWrapper {
public:
    using TaskId = uint64_t;
    
    TaskWrapper();
    explicit TaskWrapper(std::function<void()> task, 
                        TaskPriority priority = TaskPriority::NORMAL,
                        std::string name = "");
    
    // 执行任务
    void operator()();
    
    // 获取任务属性
    TaskId id() const { return id_; }
    TaskPriority priority() const { return priority_; }
    const std::string& name() const { return name_; }
    std::chrono::steady_clock::time_point submit_time() const { return submit_time_; }
    bool is_cancelled() const { return cancelled_; }
    
    // 取消任务
    bool cancel();
    
    // 等待任务完成
    void wait() const;
    
    // 检查是否完成
    bool is_done() const;
    
private:
    TaskId id_;
    std::function<void()> task_;
    TaskPriority priority_;
    std::string name_;
    std::chrono::steady_clock::time_point submit_time_;
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::atomic<bool> completed_{false};
    std::atomic<bool> cancelled_{false};
    std::exception_ptr exception_;
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point end_time_;
    
    static std::atomic<TaskId> next_id_;
};

/**
 * ThreadPool - 高级业务线程池
 * 
 * 用于处理客户端请求的业务逻辑，避免阻塞IO线程
 * 支持动态调整、优先级队列、任务取消、工作窃取等高级特性
 */
class ThreadPool : public std::enable_shared_from_this<ThreadPool> {
public:
    using Task = std::function<void()>;
    using TaskId = TaskWrapper::TaskId;
    
    // 线程池状态
    enum class State {
        CREATED,        // 已创建但未启动
        STARTING,       // 启动中
        RUNNING,        // 运行中
        DRAINING,       // 排空中（不接受新任务）
        STOPPING,       // 停止中
        STOPPED,        // 已停止
        ERROR           // 错误状态
    };
    
    // 创建线程池
    explicit ThreadPool(const ThreadPoolConfig& config = ThreadPoolConfig::default_config());
    
    ~ThreadPool();
    
    // 禁止拷贝和移动
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;
    
    /**
     * 启动线程池
     */
    bool start();
    
    /**
     * 停止线程池
     * @param graceful 是否优雅停止（等待所有任务完成）
     * @param timeout_ms 等待超时时间（毫秒）
     */
    void stop(bool graceful = true, uint64_t timeout_ms = 30000);
    
    /**
     * 暂停线程池（不接受新任务，但继续执行已有任务）
     */
    void pause();
    
    /**
     * 恢复线程池
     */
    void resume();
    
    /**
     * 提交任务（无返回值）
     * @param task 要执行的任务
     * @param priority 任务优先级
     * @param name 任务名称（用于监控）
     * @return 任务ID，0表示提交失败
     */
    TaskId submit(Task task, 
                 TaskPriority priority = TaskPriority::NORMAL,
                 const std::string& name = "");
    
    /**
     * 提交任务（有返回值）
     * @param func 要执行的任务函数
     * @param args 函数参数
     * @return std::future<ResultType>，可以获取异步结果
     */
    template<typename Func, typename... Args>
    auto submit_with_result(Func&& func, Args&&... args)
        -> std::future<typename std::invoke_result_t<std::decay_t<Func>, std::decay_t<Args>...>>;
    
    /**
     * 批量提交任务
     */
    std::vector<TaskId> submit_batch(const std::vector<Task>& tasks,
                                    TaskPriority priority = TaskPriority::NORMAL);
    
    /**
     * 延迟提交任务
     */
    TaskId submit_after(Task task, 
                       std::chrono::milliseconds delay,
                       const std::string& name = "");
    
    /**
     * 周期性提交任务
     */
    TaskId submit_every(Task task,
                       std::chrono::milliseconds interval,
                       const std::string& name = "");
    
    /**
     * 取消任务
     * @param task_id 任务ID
     * @param interrupt 是否中断正在执行的任务
     * @return 是否成功取消
     */
    bool cancel_task(TaskId task_id, bool interrupt = false);
    
    /**
     * 等待任务完成
     */
    bool wait_for_task(TaskId task_id, uint64_t timeout_ms = 0);
    
    /**
     * 等待所有任务完成
     */
    void wait_all(uint64_t timeout_ms = 0);
    
    /**
     * 调整线程池大小
     * @param min_threads 新的最小线程数
     * @param max_threads 新的最大线程数
     * @param immediate 是否立即调整
     */
    bool resize(size_t min_threads, size_t max_threads, bool immediate = false);
    
    /**
     * 获取线程池状态
     */
    State get_state() const { return state_.load(); }
    
    /**
     * 获取配置
     */
    const ThreadPoolConfig& get_config() const { return config_; }
    
    /**
     * 更新配置（部分配置可以动态更新）
     */
    bool update_config(const ThreadPoolConfig& new_config);
    
    /**
     * 获取统计信息
     */
    ThreadPoolStatistics get_statistics() const;
    
    /**
     * 重置统计信息
     */
    void reset_statistics();
    
    /**
     * 健康检查
     */
    struct HealthStatus {
        bool healthy;
        std::string status;
        std::unordered_map<std::string, std::string> details;
    };
    
    HealthStatus health_check() const;
    
    /**
     * 获取线程信息
     */
    struct ThreadInfo {
        std::thread::id id;
        size_t cpu_affinity;
        std::string name;
        std::chrono::steady_clock::time_point start_time;
        uint64_t tasks_processed;
        bool is_busy;
        TaskId current_task_id;
        std::chrono::microseconds current_task_runtime;
    };
    
    std::vector<ThreadInfo> get_thread_infos() const;
    
    /**
     * 获取任务信息
     */
    struct TaskInfo {
        TaskId id;
        std::string name;
        TaskPriority priority;
        std::chrono::steady_clock::time_point submit_time;
        std::optional<std::chrono::steady_clock::time_point> start_time;
        std::optional<std::chrono::steady_clock::time_point> end_time;
        bool completed;
        bool cancelled;
        std::string status;
    };
    
    std::vector<TaskInfo> get_task_infos() const;
    
    /**
     * 设置线程初始化函数
     */
    using ThreadInitFunc = std::function<void(std::thread::id)>;
    void set_thread_init_func(ThreadInitFunc func);
    
    /**
     * 设置线程销毁函数
     */
    using ThreadDestroyFunc = std::function<void(std::thread::id)>;
    void set_thread_destroy_func(ThreadDestroyFunc func);
    
    /**
     * 设置任务异常处理器
     */
    using TaskExceptionHandler = std::function<void(TaskId, const std::exception&)>;
    void set_task_exception_handler(TaskExceptionHandler handler);
    
    /**
     * 设置任务完成回调
     */
    using TaskCompletionHandler = std::function<void(TaskId, bool success)>;
    void set_task_completion_handler(TaskCompletionHandler handler);
    
    /**
     * 创建线程池实例
     */
    static std::shared_ptr<ThreadPool> create(const ThreadPoolConfig& config = ThreadPoolConfig::default_config());

private:
    // 内部工作线程类
    class Worker;
    
    // 内部任务队列（支持优先级）
    class TaskQueue; // 定义见 ThreadPool.cpp
    
    // 延迟任务管理器（占位空类型，当前实现中未使用）
    class DelayedTaskManager {};
    
    // 周期性任务管理器（占位空类型，当前实现中未使用）
    class PeriodicTaskManager {};
    
    // 主要组件
    ThreadPoolConfig config_;
    std::atomic<State> state_;
    std::atomic<bool> paused_;
    
    // 线程管理
    mutable std::shared_mutex workers_mutex_;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::atomic<size_t> active_workers_{0};
    
    // 任务队列
    std::unique_ptr<TaskQueue> task_queue_;
    
    // 延迟和周期性任务
    std::unique_ptr<DelayedTaskManager> delayed_task_manager_;
    std::unique_ptr<PeriodicTaskManager> periodic_task_manager_;
    
    // 统计和监控
    mutable std::mutex stats_mutex_;
    ThreadPoolStatistics stats_;
    std::thread stats_thread_;
    std::atomic<bool> stats_running_{false};
    
    // 回调函数
    ThreadInitFunc thread_init_func_;
    ThreadDestroyFunc thread_destroy_func_;
    TaskExceptionHandler task_exception_handler_;
    TaskCompletionHandler task_completion_handler_;
    
    // 同步原语
    mutable std::mutex state_mutex_;
    std::condition_variable state_cv_;
    std::condition_variable pause_cv_;
    
    // 私有方法
    bool init();
    void cleanup();
    
    // 线程管理
    bool add_worker();
    bool remove_worker(bool force = false);
    void adjust_worker_count();
    void worker_main(Worker* worker);
    
    // 任务管理
    bool enqueue_task(std::shared_ptr<TaskWrapper> task);
    std::shared_ptr<TaskWrapper> dequeue_task(Worker* worker);
    void process_task(std::shared_ptr<TaskWrapper> task, Worker* worker);
    
    // 工作窃取
    bool try_steal_work(Worker* thief, std::shared_ptr<TaskWrapper>& task);
    
    // 统计更新
    void update_statistics();
    void stats_collection_loop();
    
    // 辅助方法
    void set_state(State new_state);
    void notify_state_change();
    void safe_invoke_callback(const std::function<void()>& callback);
    
    // 线程亲和性设置
    bool set_thread_affinity(std::thread& thread, size_t cpu_index);
    
    // 任务ID生成
    static std::atomic<TaskId> next_task_id_;
};

// 模板方法实现（放在头文件中）
template<typename Func, typename... Args>
auto ThreadPool::submit_with_result(Func&& func, Args&&... args)
    -> std::future<typename std::invoke_result_t<std::decay_t<Func>, std::decay_t<Args>...>> {
    
    using ReturnType = typename std::invoke_result_t<std::decay_t<Func>, std::decay_t<Args>...>;
    
    // 创建packaged_task来包装函数
    auto task = std::make_shared<std::packaged_task<ReturnType()>>(
        [func = std::forward<Func>(func), 
         args_tuple = std::make_tuple(std::forward<Args>(args)...)]() mutable {
            return std::apply(func, std::move(args_tuple));
        }
    );
    
    // 获取future
    std::future<ReturnType> future = task->get_future();
    
    // 包装为无返回值的任务
    Task wrapper = [task]() {
        (*task)();
    };
    
    // 提交任务
    if (submit(std::move(wrapper)) == 0) {
        // 如果提交失败，设置future为异常状态
        std::promise<ReturnType> promise;
        promise.set_exception(std::make_exception_ptr(
            std::runtime_error("Failed to submit task to thread pool")
        ));
        return promise.get_future();
    }
    
    return future;
}

// 线程池工厂函数
namespace thread_pool_factory {
    
    // 创建CPU密集型任务线程池
    std::shared_ptr<ThreadPool> create_cpu_bound_pool();
    
    // 创建IO密集型任务线程池
    std::shared_ptr<ThreadPool> create_io_bound_pool();
    
    // 创建单线程线程池
    std::shared_ptr<ThreadPool> create_single_thread_pool();
    
    // 创建固定大小线程池
    std::shared_ptr<ThreadPool> create_fixed_size_pool(size_t size);
    
    // 创建缓存线程池（自动调整大小）
    std::shared_ptr<ThreadPool> create_cached_pool();
    
} // namespace thread_pool_factory

} // namespace net
} // namespace mementodb

#endif // THREAD_POOL_H

