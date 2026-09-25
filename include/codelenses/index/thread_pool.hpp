#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace codelenses::index {

class ThreadPool {
public:
    explicit ThreadPool(std::size_t threads = 0);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<typename std::invoke_result_t<F, Args...>> {
        using return_type = typename std::invoke_result_t<F, Args...>;

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            [func = std::forward<F>(f),
             ... captured_args = std::forward<Args>(args)]() mutable {
                return std::invoke(func, std::forward<Args>(captured_args)...);
            });

        std::future<return_type> res = task->get_future();
        {
            std::unique_lock lock(queue_mutex_);
            if (stopped_) {
                throw std::runtime_error("submit called on stopped ThreadPool");
            }
            tasks_.emplace([task]() { (*task)(); });
        }
        cv_.notify_one();
        return res;
    }

    void submit_task(std::function<void()> task);

    void shutdown();
    void cancel();

    [[nodiscard]] std::size_t size() const noexcept { return workers_.size(); }
    [[nodiscard]] bool is_stopped() const;

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    mutable std::mutex queue_mutex_;
    std::condition_variable cv_;
    bool stopped_{false};
};

} // namespace codelenses::index
