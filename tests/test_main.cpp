// Plain asserts, no framework. Run: ./order_dispatch_tests
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

#include "../src/bounded_queue.hpp"
#include "../src/dedupe.hpp"
#include "../src/stats.hpp"

static void dedupe_drops_repeats() {
  ShardedDedupe<> d;
  assert(d.first_time(42));
  assert(!d.first_time(42));
  assert(d.first_time(43));
}

// 8 threads race to claim the same 10,000 IDs: each ID must be won exactly once.
static void dedupe_is_exactly_once_under_contention() {
  ShardedDedupe<> d;
  std::atomic<int> wins{0};
  std::vector<std::thread> ts;
  for (int t = 0; t < 8; ++t)
    ts.emplace_back([&] { for (std::uint64_t id = 0; id < 10'000; ++id) if (d.first_time(id)) wins++; });
  for (auto& t : ts) t.join();
  assert(wins == 10'000);
}

// Many producers, many consumers, tiny queue: nothing lost, nothing duplicated.
static void queue_delivers_everything_once() {
  BoundedQueue<int> q(4);
  std::atomic<long> sum{0}, count{0};
  std::vector<std::thread> cons, prods;
  for (int c = 0; c < 4; ++c) cons.emplace_back([&] { while (auto v = q.pop()) { sum += *v; count++; } });
  for (int p = 0; p < 4; ++p) prods.emplace_back([&, p] { for (int i = 1; i <= 1000; ++i) q.push(i); });
  for (auto& t : prods) t.join();
  q.close();
  for (auto& t : cons) t.join();
  assert(count == 4000);
  assert(sum == 4L * 1000 * 1001 / 2);
}

static void closed_queue_rejects_push() {
  BoundedQueue<int> q(2);
  q.close();
  assert(!q.push(1));
  assert(!q.pop().has_value());
}

static void percentile_nearest_rank() {
  std::vector<std::int64_t> v;
  for (int i = 1; i <= 100; ++i) v.push_back(i);
  assert(percentile(v, 50) == 51);
  assert(percentile(v, 99) == 99);
  assert(percentile({}, 99) == 0);
}

int main() {
  dedupe_drops_repeats();
  dedupe_is_exactly_once_under_contention();
  queue_delivers_everything_once();
  closed_queue_rejects_push();
  percentile_nearest_rank();
  std::cout << "all tests passed\n";
}
