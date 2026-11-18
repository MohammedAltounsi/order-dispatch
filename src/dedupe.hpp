#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_set>

// Remembers which order IDs were already processed, so a retried delivery is dropped.
// Split into shards, each with its own lock: workers handling different IDs rarely
// wait on each other. One global lock would make every worker queue up here.
// Memory grows with every ID seen; production would expire IDs after a time window.
template <std::size_t Shards = 64>
class ShardedDedupe {
 public:
  // True the first time an ID is seen, false for every repeat.
  bool first_time(std::uint64_t id) {
    auto& s = shards_[std::hash<std::uint64_t>{}(id) % Shards];
    std::lock_guard lock(s.mu);
    return s.seen.insert(id).second;
  }

 private:
  struct Shard {
    std::mutex mu;
    std::unordered_set<std::uint64_t> seen;
  };
  std::array<Shard, Shards> shards_;
};
