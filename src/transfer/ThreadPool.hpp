/*
 * ThreadPool.hpp - Fixed-Size Thread Pool for Phase 4
 * 
 * Implements a fixed-size worker pool with task queue.
 * Per Phase 4 spec Section 4.1.
 */

#ifndef FTPCLIENT_TRANSFER_THREAD_POOL_HPP
#define FTPCLIENT_TRANSFER_THREAD_POOL_HPP

#include <vector>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <cstdint>

namespace ftpclient { namespace transfer {

/* Forward declaration */
struct Task;

/**
 * Thread Pool Configuration
 * Per spec Section 4.1
 */
struct ThreadPoolConfig {
    uint32_t num_workers;       /* 0 = hardware_concurrency(), clamped 1-16 */
    
    explicit ThreadPoolConfig(uint32_t workers = 0) : num_workers(workers) {}
};

/**
 * Fixed-Size Thread Pool
 * 
 * Manages a fixed number of worker threads that process tasks from a queue.
 * Implements work-stealing distribution per spec Section 4.1.
 * 
 * Thread Safety: All public methods are thread-safe.
 */
class ThreadPool {
public:
    /**
     * Construct thread pool with given configuration
     * 
     * @param config Pool configuration
     */
    explicit ThreadPool(const ThreadPoolConfig& config = ThreadPoolConfig());
    
    /**
     * Destructor - signals stop and joins all threads
     */
    ~ThreadPool();
    
    /* Disable copying */
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    
    /**
     * Enqueue a task for execution
     * 
     * @param task Task to enqueue (moved)
     * @return true on success, false if pool is stopped
     */
    bool enqueue(Task task);
    
    /**
     * Block until all tasks complete
     * 
     * Waits until the task queue is empty AND all workers have finished
     * their current tasks.
     */
    void wait_for_all();
    
    /**
     * Get number of worker threads
     */
    uint32_t get_worker_count() const { return workers_.size(); }
    
    /**
     * Check if pool is stopped
     */
    bool is_stopped() const { return stop_.load(std::memory_order_acquire); }

private:
    std::vector<std::thread> workers_;
    std::queue<Task> tasks_;
    mutable std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::condition_variable completion_cv_;
    std::atomic<bool> stop_{false};
    std::atomic<uint32_t> active_tasks_{0};
    
    /**
     * Worker thread function
     */
    void worker_loop();
};

}} // namespace ftpclient::transfer

#endif /* FTPCLIENT_TRANSFER_THREAD_POOL_HPP */
