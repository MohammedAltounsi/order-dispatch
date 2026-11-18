#pragma once
#include <atomic>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

// Lock-free bounded queue. Dmitry Vyukov's bounded MPMC queue (1024cores.net).
//
// Each slot has a sequence number:
//   seq == pos      empty, waiting for whichever producer claims position pos
//   seq == pos + 1  full, waiting for whichever consumer claims position pos
// Producer: CAS head_ to claim pos, write the item, then store seq = pos + 1 (release).
// Consumer: load seq (acquire) so the item is visible, read it, then store
// seq = pos + capacity so the slot is free for the producer on the next lap.
//
// No lock, but an idle consumer spins (with yield) and keeps a core busy.
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
      if (diff == 0) {  // slot is free for us, try to claim it
        if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          c.item = std::move(item);
          c.seq.store(pos + 1, std::memory_order_release);
          return true;
        }  // lost the race; CAS put the current head in pos, go again
      } else if (diff < 0) {
        return false;  // still holds last lap's item, so we're full
      } else {
        pos = head_.load(std::memory_order_relaxed);  // someone else got ahead of us
      }
    }
  }

  [[nodiscard]] bool try_pop(T& out) {
    std::size_t pos = tail_.load(std::memory_order_relaxed);
    for (;;) {
      Cell& c = cells_[pos & mask_];
      std::size_t seq = c.seq.load(std::memory_order_acquire);
      auto diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
      if (diff == 0) {  // an item is ready here, try to claim it
        if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          out = std::move(c.item);
          c.seq.store(pos + mask_ + 1, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        return false;  // nothing here yet, so we're empty
      } else {
        pos = tail_.load(std::memory_order_relaxed);
      }
    }
  }

  // Blocking versions, so main.cpp can use either queue the same way.
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
      // close() only happens after every producer is done, so one last try is enough.
      if (closed_.load(std::memory_order_acquire)) return try_pop(out) ? std::optional<T>(out) : std::nullopt;
      std::this_thread::yield();
    }
  }

  void close() { closed_.store(true, std::memory_order_release); }

 private:
  struct alignas(64) Cell {  // own cache line, so neighbouring slots don't false-share
    std::atomic<std::size_t> seq;
    T item;
  };
  std::vector<Cell> cells_;
  const std::size_t mask_;
  alignas(64) std::atomic<std::size_t> head_{0};  // producers
  alignas(64) std::atomic<std::size_t> tail_{0};  // consumers
  alignas(64) std::atomic<bool> closed_{false};
};
