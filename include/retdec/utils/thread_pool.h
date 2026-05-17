/**
 * @file include/retdec/utils/thread_pool.h
 * @brief Lightweight C++17 thread pool for parallel decompilation.
 * @copyright (c) 2024 RetDec contributors, MIT license
 */

#ifndef RETDEC_UTILS_THREAD_POOL_H
#define RETDEC_UTILS_THREAD_POOL_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace retdec {
namespace utils {

class ThreadPool {
public:
    explicit ThreadPool(std::size_t numThreads = 0)
        : stop_(false), activeTasks_(0)
    {
        if (numThreads == 0)
            numThreads = std::thread::hardware_concurrency();
        if (numThreads == 0)
            numThreads = 1;
        workers_.reserve(numThreads);
        for (std::size_t i = 0; i < numThreads; ++i)
            workers_.emplace_back([this] { workerLoop(); });
    }

    ~ThreadPool() {
        { std::unique_lock<std::mutex> lock(queueMutex_); stop_ = true; }
        condition_.notify_all();
        for (auto& t : workers_)
            if (t.joinable()) t.join();
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        using RetType = std::invoke_result_t<F, Args...>;
        auto task = std::make_shared<std::packaged_task<RetType()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        std::future<RetType> result = task->get_future();
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            if (stop_) throw std::runtime_error("ThreadPool: submit on stopped pool");
            ++activeTasks_;
            tasks_.emplace([task]() { (*task)(); });
        }
        condition_.notify_one();
        return result;
    }

    void wait() {
        std::unique_lock<std::mutex> lock(doneMutex_);
        doneCond_.wait(lock, [this] { return activeTasks_.load() == 0; });
    }

    std::size_t size() const { return workers_.size(); }

private:
    void workerLoop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                condition_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            try { task(); } catch (...) {}
            {
                std::unique_lock<std::mutex> lock(doneMutex_);
                if (--activeTasks_ == 0) doneCond_.notify_all();
            }
        }
    }

    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex                        queueMutex_;
    std::condition_variable           condition_;
    std::mutex                        doneMutex_;
    std::condition_variable           doneCond_;
    std::atomic<int>                  activeTasks_;
    bool                              stop_;
};

} // namespace utils
} // namespace retdec

#endif // RETDEC_UTILS_THREAD_POOL_H
