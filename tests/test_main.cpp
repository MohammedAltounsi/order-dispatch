// Plain asserts, no framework. Asserts stay on in every build type.
#undef NDEBUG
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <set>
#include <thread>
#include <vector>

#include "../src/bounded_queue.hpp"
#include "../src/dedupe.hpp"
#include "../src/mpmc_ring.hpp"
#include "../src/stats.hpp"

static void dedupe_drops_repeats() {
  ShardedDedupe d(64);
  assert(d.first_time(42));
  assert(!d.first_time(42));
  assert(d.first_time(43));
}

// 8 threads race to claim the same 10,000 IDs: each ID must be won exactly once,
// with one shard (everyone shares a lock) and with 64.
static void dedupe_exactly_once_under_contention(std::size_t shards) {
  ShardedDedupe d(shards);
  std::atomic<int> wins{0};
  std::vector<std::thread> ts;
  for (int t = 0; t < 8; ++t)
    ts.emplace_back([&] { for (std::uint64_t id = 0; id < 10'000; ++id) if (d.first_time(id)) wins++; });
  for (auto& t : ts) t.join();
  assert(wins == 10'000);
}

// Order i from 8 different branches must not all land on one shard.
static void dedupe_spreads_same_index_across_branches() {
  ShardedDedupe d(64);
  for (std::uint64_t i = 0; i < 100; ++i) {
    std::set<std::size_t> used;
    for (std::uint64_t b = 0; b < 8; ++b) used.insert(d.shard_of((b << 32) | i));
    assert(used.size() >= 5);
  }
}

static void dedupe_rejects_bad_shard_count() {
  bool threw = false;
  try { ShardedDedupe d(3); } catch (const std::invalid_argument&) { threw = true; }
  assert(threw);
}

// Many producers, many consumers, tiny queue: nothing lost, nothing duplicated.
template <class Q>
static void delivers_everything_once() {
  Q q(4);
  std::atomic<long> sum{0}, count{0};
  std::vector<std::thread> cons, prods;
  for (int c = 0; c < 4; ++c) cons.emplace_back([&] { while (auto v = q.pop()) { sum += *v; count++; } });
  for (int p = 0; p < 4; ++p) prods.emplace_back([&] { for (int i = 1; i <= 20'000; ++i) { bool ok = q.push(i); assert(ok); } });
  for (auto& t : prods) t.join();
  q.close();
  for (auto& t : cons) t.join();
  assert(count == 80'000);
  assert(sum == 4L * 20'000 * 20'001 / 2);
}

template <class Q>
static void try_push_fails_when_full_and_order_is_fifo() {
  Q q(4);
  for (int i = 0; i < 4; ++i) assert(q.try_push(i));
  assert(!q.try_push(99));
  for (int i = 0; i < 4; ++i) assert(*q.pop() == i);
}

static void closed_mutex_queue_rejects_push() {
  BoundedQueue<int> q(2);
  q.close();
  assert(!q.push(1));
  assert(!q.pop().has_value());
}

static void ring_rejects_non_power_of_two() {
  bool threw = false;
  try { MpmcRing<int> r(6); } catch (const std::invalid_argument&) { threw = true; }
  assert(threw);
}

static void percentile_is_nearest_rank() {
  std::vector<std::int64_t> v;
  for (int i = 1; i <= 100; ++i) v.push_back(i);
  assert(percentile(v, 50) == 50);
  assert(percentile(v, 99) == 99);
  assert(percentile(v, 99.9) == 100);
  assert(percentile(v, 100) == 100);
  assert(percentile(v, 0) == 1);
  assert(percentile({}, 99) == 0);
}

int main() {
  dedupe_drops_repeats();
  dedupe_exactly_once_under_contention(1);
  dedupe_exactly_once_under_contention(64);
  dedupe_spreads_same_index_across_branches();
  dedupe_rejects_bad_shard_count();
  delivers_everything_once<BoundedQueue<int>>();
  delivers_everything_once<MpmcRing<int>>();
  try_push_fails_when_full_and_order_is_fifo<BoundedQueue<int>>();
  try_push_fails_when_full_and_order_is_fifo<MpmcRing<int>>();
  closed_mutex_queue_rejects_push();
  ring_rejects_non_power_of_two();
  percentile_is_nearest_rank();
  std::cout << "all tests passed\n";
}
