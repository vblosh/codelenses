#include "codelenses/index/thread_pool.hpp"

#include <algorithm>

namespace codelenses::index {

ThreadPool::ThreadPool(std::size_t threads) {
    if (threads == 0) {
        threads = std::max(1u, std::thread::hardware_concurrency());
    }

    workers_.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
        workers_.emplace_back([this] {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock lock(queue_mutex_);
                    cv_.wait(lock, [this] { return stopped_ || !tasks_.empty(); });
                    if (stopped_ && tasks_.empty()) {
                        return;
                    }
                    task = std::move(tasks_.front());
                    tasks_.pop();
                }
                task();
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::submit_task(std::function<void()> task) {
    {
        std::unique_lock lock(queue_mutex_);
        if (stopped_) {
            return;
        }
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
}

void ThreadPool::shutdown() {
    {
        std::unique_lock lock(queue_mutex_);
        if (stopped_) {
            return;
        }
        stopped_ = true;
    }
    cv_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

void ThreadPool::cancel() {
    {
        std::unique_lock lock(queue_mutex_);
        if (stopped_) {
            return;
        }
        stopped_ = true;
        // Drain pending tasks without executing them
        std::queue<std::function<void()>> empty;
        std::swap(tasks_, empty);
    }
    cv_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

bool ThreadPool::is_stopped() const {
    std::lock_guard lock(queue_mutex_);
    return stopped_;
}

} // namespace codelenses::index
