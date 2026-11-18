#pragma once
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <unordered_set>
#include <vector>

// Order IDs already processed. first_time() is true once per ID, false for every repeat.
// Each shard has its own mutex and its own 64-byte cache line, so two workers only
// wait on each other when their IDs land in the same shard.
// The set never shrinks: fine for a benchmark run, a real service would expire old IDs.
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

  // libstdc++'s std::hash<uint64_t> returns the ID unchanged, so "id % shards" would send
  // order i of every branch (branch in the high 32 bits) to the same shard at the same moment.
  // The splitmix64 finalizer mixes every input bit into every output bit; keep the top bits.
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
