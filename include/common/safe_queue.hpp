#pragma once
// safe_queue.hpp — Thread-safe queue for IPC between gateway threads.
// Pattern: same role as asyncio.Queue in Python but using std::mutex + condition_variable.
// Usage: SafeQueue<DecodedSignal> between SignalDecoder thread and TelemetryPublisher thread.

#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <chrono>
#include <stdexcept>

template<typename T>
class SafeQueue {
public:
    // Push item — non-blocking, always succeeds (no capacity limit)
    void push(T item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(item));
        }
        cv_.notify_one();
    }

    // Pop item — BLOCKING. Waits until an item is available.
    T pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]{ return !queue_.empty() || shutdown_; });
        if (queue_.empty()) throw std::runtime_error("SafeQueue: shutdown");
        T item = std::move(queue_.front());
        queue_.pop();
        return item;
    }

    // Try pop — NON-BLOCKING. Returns empty optional if queue is empty.
    std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return std::nullopt;
        T item = std::move(queue_.front());
        queue_.pop();
        return item;
    }

    // Pop with timeout — returns empty optional if timeout expires
    std::optional<T> pop_for(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [this]{ return !queue_.empty() || shutdown_; }))
            return std::nullopt;
        if (queue_.empty()) return std::nullopt;
        T item = std::move(queue_.front());
        queue_.pop();
        return item;
    }

    // Signal all waiting threads to unblock (used during shutdown)
    void shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_ = true;
        cv_.notify_all();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex      mutex_;
    std::condition_variable cv_;
    std::queue<T>           queue_;
    bool                    shutdown_ = false;
};
