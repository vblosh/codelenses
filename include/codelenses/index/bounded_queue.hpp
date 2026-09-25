#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace codelenses::index {

enum class QueuePushResult {
    pushed,
    full,
    too_large,
    closed,
    cancelled,
};

template <typename T, typename SizeFn = std::function<std::size_t(const T&)>>
class BoundedQueue {
    static_assert(std::is_invocable_r_v<std::size_t, SizeFn&, const T&>,
                  "SizeFn must return the retained byte count for a const T&");

public:
    struct Limits {
        std::size_t max_items;
        std::size_t max_bytes;
    };

    explicit BoundedQueue(Limits limits, SizeFn size_fn = {})
        : limits_(limits), size_fn_(std::move(size_fn)) {
        if (limits_.max_items == 0 || limits_.max_bytes == 0) {
            throw std::invalid_argument("bounded queue limits must be positive");
        }
    }

    BoundedQueue(std::size_t max_items, std::size_t max_bytes, SizeFn size_fn = {})
        : BoundedQueue(Limits{max_items, max_bytes}, std::move(size_fn)) {}

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;
    BoundedQueue(BoundedQueue&&) = delete;
    BoundedQueue& operator=(BoundedQueue&&) = delete;

    [[nodiscard]] QueuePushResult push(T item, std::stop_token stop = {}) {
        const std::size_t item_bytes = retained_bytes(item);
        std::unique_lock lock(mutex_);

        if (cancelled_ || stop.stop_requested()) {
            return QueuePushResult::cancelled;
        }
        if (closed_) {
            return QueuePushResult::closed;
        }
        if (item_bytes > limits_.max_bytes) {
            return QueuePushResult::too_large;
        }

        std::stop_callback wake_waiters(stop, [this] {
            not_full_.notify_all();
            not_empty_.notify_all();
        });

        not_full_.wait(lock, [this, item_bytes, &stop] {
            return cancelled_ || closed_ || stop.stop_requested() || has_capacity(item_bytes);
        });

        if (cancelled_ || stop.stop_requested()) {
            return QueuePushResult::cancelled;
        }
        if (closed_) {
            return QueuePushResult::closed;
        }
        if (!has_capacity(item_bytes)) {
            return QueuePushResult::full;
        }

        items_.emplace_back(std::move(item), item_bytes);
        retained_bytes_ += item_bytes;
        not_empty_.notify_one();
        return QueuePushResult::pushed;
    }

    [[nodiscard]] QueuePushResult try_push(T item) {
        const std::size_t item_bytes = retained_bytes(item);
        std::lock_guard lock(mutex_);

        if (cancelled_) {
            return QueuePushResult::cancelled;
        }
        if (closed_) {
            return QueuePushResult::closed;
        }
        if (item_bytes > limits_.max_bytes) {
            return QueuePushResult::too_large;
        }
        if (!has_capacity(item_bytes)) {
            return QueuePushResult::full;
        }

        items_.emplace_back(std::move(item), item_bytes);
        retained_bytes_ += item_bytes;
        not_empty_.notify_one();
        return QueuePushResult::pushed;
    }

    [[nodiscard]] std::optional<T> pop(std::stop_token stop = {}) {
        std::unique_lock lock(mutex_);
        std::stop_callback wake_waiters(stop, [this] {
            not_full_.notify_all();
            not_empty_.notify_all();
        });

        not_empty_.wait(lock, [this, &stop] {
            return !items_.empty() || cancelled_ || closed_ || stop.stop_requested();
        });

        if (cancelled_ || stop.stop_requested() || items_.empty()) {
            return std::nullopt;
        }

        Entry entry = std::move(items_.front());
        items_.pop_front();
        retained_bytes_ -= entry.retained_bytes;
        not_full_.notify_all();
        return std::move(entry.item);
    }

    [[nodiscard]] std::optional<T> try_pop() {
        std::lock_guard lock(mutex_);
        if (items_.empty() || cancelled_) {
            return std::nullopt;
        }

        Entry entry = std::move(items_.front());
        items_.pop_front();
        retained_bytes_ -= entry.retained_bytes;
        not_full_.notify_all();
        return std::move(entry.item);
    }

    void close() {
        std::lock_guard lock(mutex_);
        closed_ = true;
        not_full_.notify_all();
        not_empty_.notify_all();
    }

    void cancel() {
        std::lock_guard lock(mutex_);
        cancelled_ = true;
        closed_ = true;
        items_.clear();
        retained_bytes_ = 0;
        not_full_.notify_all();
        not_empty_.notify_all();
    }

    [[nodiscard]] bool closed() const {
        std::lock_guard lock(mutex_);
        return closed_;
    }

    [[nodiscard]] bool cancelled() const {
        std::lock_guard lock(mutex_);
        return cancelled_;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mutex_);
        return items_.size();
    }

    [[nodiscard]] std::size_t retained_bytes() const {
        std::lock_guard lock(mutex_);
        return retained_bytes_;
    }

    [[nodiscard]] Limits limits() const noexcept { return limits_; }

private:
    struct Entry {
        T item;
        std::size_t retained_bytes;

        Entry(T value, std::size_t bytes) : item(std::move(value)), retained_bytes(bytes) {}
    };

    [[nodiscard]] std::size_t retained_bytes(const T& item) const {
        if constexpr (std::is_default_constructible_v<SizeFn> &&
                      std::is_same_v<SizeFn, std::function<std::size_t(const T&)>>) {
            if (!size_fn_) {
                return sizeof(T);
            }
        }
        return static_cast<std::size_t>(std::invoke(size_fn_, item));
    }

    [[nodiscard]] bool has_capacity(std::size_t item_bytes) const noexcept {
        return items_.size() < limits_.max_items &&
               item_bytes <= limits_.max_bytes - retained_bytes_;
    }

    const Limits limits_;
    SizeFn size_fn_;
    mutable std::mutex mutex_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    std::deque<Entry> items_;
    std::size_t retained_bytes_{0};
    bool closed_{false};
    bool cancelled_{false};
};

} // namespace codelenses::index
