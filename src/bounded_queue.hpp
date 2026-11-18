#pragma once
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>

// Multi-producer, multi-consumer queue with a fixed capacity.
// A full queue blocks producers (backpressure) instead of growing memory without limit.
template <typename T>
class BoundedQueue {
 public:
  explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {}

  // Blocks while full. Returns false if the queue was closed.
  bool push(T item) {
    std::unique_lock lock(mu_);
    not_full_.wait(lock, [&] { return closed_ || items_.size() < capacity_; });
    if (closed_) return false;
    items_.push_back(std::move(item));
    not_empty_.notify_one();
    return true;
  }

  // Blocks while empty. Returns nullopt once closed and drained.
  std::optional<T> pop() {
    std::unique_lock lock(mu_);
    not_empty_.wait(lock, [&] { return closed_ || !items_.empty(); });
    if (items_.empty()) return std::nullopt;
    T item = std::move(items_.front());
    items_.pop_front();
    not_full_.notify_one();
    return item;
  }

  // No more pushes; consumers drain what is left, then stop.
  void close() {
    std::lock_guard lock(mu_);
    closed_ = true;
    not_empty_.notify_all();
    not_full_.notify_all();
  }

 private:
  std::size_t capacity_;
  std::deque<T> items_;
  bool closed_ = false;
  std::mutex mu_;
  std::condition_variable not_empty_, not_full_;
};
