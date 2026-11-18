#pragma once
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <unordered_set>
#include <vector>

// IDs we've already processed. first_time() returns true once per ID.
// One mutex per shard, one cache line per shard. Never shrinks (fine for a benchmark run).
class ShardedDedupe {
 public:
  explicit ShardedDedupe(std::size_t shards, std::size_t expected_ids = 0) : shards_(shards) {
    if (shards == 0 || (shards & (shards - 1)) != 0) throw std::invalid_argument("shards must be a power of two");
    while ((std::size_t{1} << bits_) < shards) ++bits_;
    for (auto& s : shards_) s.seen.reserve(expected_ids / shards + 1);
  }

  bool first_time(std::uint64_t id) {
    Shard& s = shards_[shard_of(id)];
    std::lock_guard lock(s.mu);
    return s.seen.insert(id).second;
  }

  // Don't use std::hash here: in libstdc++ it returns the ID as-is, and since the branch
  // lives in the high bits, order i from every branch hit the same shard. splitmix64 mixes it.
  std::size_t shard_of(std::uint64_t id) const {
    if (bits_ == 0) return 0;
    id ^= id >> 30; id *= 0xBF58476D1CE4E5B9ull;
    id ^= id >> 27; id *= 0x94D049BB133111EBull;
    id ^= id >> 31;
    return static_cast<std::size_t>(id >> (64 - bits_));
  }

 private:
  struct alignas(64) Shard {
    std::mutex mu;
    std::unordered_set<std::uint64_t> seen;
  };
  std::vector<Shard> shards_;
  unsigned bits_ = 0;
};
