#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <functional>
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace mementodb {
namespace net {

/**
 * ThreadPool - 业务线程池
 * 
 * 用于处理客户端请求的业务逻辑，避免阻塞IO线程
 */
class ThreadPool {
public:
    using Task = std::function<void()>;
    
    ThreadPool(size_t thread_count = std::thread::hardware_concurrency());
    ~ThreadPool();
    
    /**
     * 启动线程池
     */
    void start();
    
    /**
     * 停止线程池（等待所有任务完成）
     */
    void stop();
    
    /**
     * 提交任务到线程池
     * @param task 要执行的任务
     */
    void submit(Task task);
    
    /**
     * 获取线程池大小
     * @return 线程数量
     */
    size_t size() const { return threads_.size(); }
    
    /**
     * 获取当前队列中的任务数
     * @return 任务数量
     */
    size_t pending_tasks() const;

private:
    void worker_thread();
    
    std::vector<std::thread> threads_;
    std::queue<Task> task_queue_;
    std::mutex queue_mutex_;
    std::condition_variable condition_;
    std::atomic<bool> stop_;
};

} // namespace net
} // namespace mementodb

#endif // THREAD_POOL_H

