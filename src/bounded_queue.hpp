#pragma once
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>

// Mutex + condition-variable queue with a fixed capacity. Many producers, many consumers.
// Idle consumers sleep instead of burning CPU, at the cost of a lock on every push and pop.
template <typename T>
class BoundedQueue {
 public:
  explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
    if (capacity == 0) throw std::invalid_argument("capacity must be at least 1");
  }

  // Blocks while full. False if the queue was closed.
  [[nodiscard]] bool push(T item) {
    std::unique_lock lock(mu_);
    not_full_.wait(lock, [&] { return closed_ || items_.size() < capacity_; });
    if (closed_) return false;
    items_.push_back(std::move(item));
    not_empty_.notify_one();
    return true;
  }

  // Never blocks. False if full or closed: the caller decides whether to shed or retry.
  [[nodiscard]] bool try_push(T item) {
    std::lock_guard lock(mu_);
    if (closed_ || items_.size() >= capacity_) return false;
    items_.push_back(std::move(item));
    not_empty_.notify_one();
    return true;
  }

  // Blocks while empty. nullopt once closed and drained.
  std::optional<T> pop() {
    std::unique_lock lock(mu_);
    not_empty_.wait(lock, [&] { return closed_ || !items_.empty(); });
    if (items_.empty()) return std::nullopt;
    T item = std::move(items_.front());
    items_.pop_front();
    not_full_.notify_one();
    return item;
  }

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
