// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/scan/bounded_queue.hpp
// Purpose:       A bounded, blocking, closable multi-producer/multi-consumer
//                queue. The scanner's producer (directory walker) blocks when it
//                is full, which is what keeps memory flat on a tree of millions
//                of files rather than reading every path into RAM first.
// Created:       2026-07-24
// Last Modified: 2026-07-24
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <queue>
#include <utility>

namespace starbase::scan {

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity ? capacity : 1) {}

    // Block until there is room, then enqueue. Returns false if the queue was
    // closed (no further items will be accepted).
    bool push(T value) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [&] { return queue_.size() < capacity_ || closed_; });
        if (closed_) return false;
        queue_.push(std::move(value));
        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    // Block until an item is available, then dequeue it. Returns nullopt once
    // the queue is both closed and drained, so a worker loop ends cleanly.
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [&] { return !queue_.empty() || closed_; });
        if (queue_.empty()) return std::nullopt;  // closed and drained
        T value = std::move(queue_.front());
        queue_.pop();
        lock.unlock();
        not_full_.notify_one();
        return value;
    }

    // No more items will be pushed. Wakes every blocked producer and consumer.
    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

private:
    const std::size_t capacity_;
    std::mutex mutex_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    std::queue<T> queue_;
    bool closed_ = false;
};

}  // namespace starbase::scan
