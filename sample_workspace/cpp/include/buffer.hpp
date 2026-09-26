#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace sample::engine {

template <typename T>
class CircularBuffer {
public:
    explicit CircularBuffer(std::size_t capacity)
        : capacity_(capacity), head_(0), tail_(0), size_(0) {
        data_.resize(capacity_);
    }

    void push(const T& item) {
        data_[head_] = item;
        head_ = (head_ + 1) % capacity_;
        if (size_ < capacity_) {
            size_++;
        } else {
            tail_ = (tail_ + 1) % capacity_;
        }
    }

    [[nodiscard]] std::optional<T> pop() {
        if (size_ == 0) {
            return std::nullopt;
        }
        T item = data_[tail_];
        tail_ = (tail_ + 1) % capacity_;
        size_--;
        return item;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }

private:
    std::size_t capacity_;
    std::size_t head_;
    std::size_t tail_;
    std::size_t size_;
    std::vector<T> data_;
};

using DoubleBuffer = CircularBuffer<double>;
using SharedBuffer = std::shared_ptr<DoubleBuffer>;

} // namespace sample::engine
