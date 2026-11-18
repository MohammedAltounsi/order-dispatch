#pragma once
#include <atomic>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

// Lock-free bounded queue, Dmitry Vyukov's MPMC design (1024cores.net, "Bounded MPMC queue").
//
// Each slot carries a sequence number that says whose turn it is:
//   seq == pos       the slot is empty and waiting for the producer that claims position pos
//   seq == pos + 1   the slot holds the item for the consumer that claims position pos
// A producer claims a position with one compare-and-swap on head_, writes the item, then
// publishes it with a release store of seq = pos + 1. The consumer's acquire load of seq
// guarantees it sees the item. After reading, the consumer sets seq = pos + capacity:
// the slot is free again for the producer one lap later.
//
// Trade-off vs BoundedQueue: no lock and no kernel calls, but an idle consumer spins
// (with yield) instead of sleeping, so it burns CPU when there is no work.
template <typename T>
class MpmcRing {
 public:
  explicit MpmcRing(std::size_t capacity) : cells_(capacity), mask_(capacity - 1) {
    if (capacity < 2 || (capacity & (capacity - 1)) != 0)
      throw std::invalid_argument("ring capacity must be a power of two, at least 2");
    for (std::size_t i = 0; i < capacity; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
  }

  [[nodiscard]] bool try_push(T item) {
    std::size_t pos = head_.load(std::memory_order_relaxed);
    for (;;) {
      Cell& c = cells_[pos & mask_];
      std::size_t seq = c.seq.load(std::memory_order_acquire);
      auto diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
      if (diff == 0) {  // slot free for this position: try to claim it
        if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          c.item = std::move(item);
          c.seq.store(pos + 1, std::memory_order_release);
          return true;
        }  // lost the race: pos now holds the current head, loop again
      } else if (diff < 0) {
        return false;  // slot still holds last lap's item: queue is full
      } else {
        pos = head_.load(std::memory_order_relaxed);  // another producer moved ahead
      }
    }
  }

  [[nodiscard]] bool try_pop(T& out) {
    std::size_t pos = tail_.load(std::memory_order_relaxed);
    for (;;) {
      Cell& c = cells_[pos & mask_];
      std::size_t seq = c.seq.load(std::memory_order_acquire);
      auto diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
      if (diff == 0) {  // item published for this position: try to claim it
        if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          out = std::move(c.item);
          c.seq.store(pos + mask_ + 1, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        return false;  // nothing published yet: queue is empty
      } else {
        pos = tail_.load(std::memory_order_relaxed);
      }
    }
  }

  // Same interface as BoundedQueue, built on the try_ versions.
  [[nodiscard]] bool push(T item) {
    while (!try_push(item)) {
      if (closed_.load(std::memory_order_acquire)) return false;
      std::this_thread::yield();
    }
    return true;
  }

  std::optional<T> pop() {
    T out;
    for (;;) {
      if (try_pop(out)) return out;
      // Producers have all finished before close(), so one more try after seeing
      // closed_ catches the last items; after that the queue really is empty.
      if (closed_.load(std::memory_order_acquire)) return try_pop(out) ? std::optional<T>(out) : std::nullopt;
      std::this_thread::yield();
    }
  }

  void close() { closed_.store(true, std::memory_order_release); }

 private:
  struct alignas(64) Cell {  // one slot per cache line: neighbouring slots don't false-share
    std::atomic<std::size_t> seq;
    T item;
  };
  std::vector<Cell> cells_;
  const std::size_t mask_;
  alignas(64) std::atomic<std::size_t> head_{0};  // producers' counter, on its own cache line
  alignas(64) std::atomic<std::size_t> tail_{0};  // consumers' counter, on its own cache line
  alignas(64) std::atomic<bool> closed_{false};
};
