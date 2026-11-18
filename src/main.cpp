// order-dispatch: simulates peak-hour orders from many branches flowing to kitchen workers.
//   producers (branches) -> bounded queue -> workers -> dedupe -> "send to kitchen"
// Some orders are sent twice on purpose, like a delivery app retrying a webhook.
// Prints throughput, duplicates dropped, and queue-wait latency at p50/p95/p99.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "bounded_queue.hpp"
#include "dedupe.hpp"
#include "stats.hpp"

using Clock = std::chrono::steady_clock;

struct Order {
  std::uint64_t id;
  std::uint32_t branch;
  std::uint32_t total_halalas;  // money as integers, never floats
  Clock::time_point enqueued;
};

struct Config {
  int branches = 8, workers = 4, orders_per_branch = 50'000, queue_cap = 1024;
  int retry_pct = 5;  // % of orders delivered twice
};

static Config parse(int argc, char** argv) {
  Config c;
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string k = argv[i];
    int v = std::atoi(argv[i + 1]);
    if (k == "--branches") c.branches = v;
    else if (k == "--workers") c.workers = v;
    else if (k == "--orders") c.orders_per_branch = v;
    else if (k == "--queue") c.queue_cap = v;
    else if (k == "--retry-pct") c.retry_pct = v;
  }
  return c;
}

int main(int argc, char** argv) {
  const Config cfg = parse(argc, argv);
  BoundedQueue<Order> queue(cfg.queue_cap);
  ShardedDedupe<> dedupe;
  std::atomic<std::uint64_t> processed{0}, dropped{0}, revenue{0};
  std::vector<std::vector<std::int64_t>> waits(cfg.workers);  // one per worker: no shared lock

  auto start = Clock::now();

  std::vector<std::thread> workers;
  for (int w = 0; w < cfg.workers; ++w) {
    workers.emplace_back([&, w] {
      auto& my_waits = waits[w];
      while (auto o = queue.pop()) {
        my_waits.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                               Clock::now() - o->enqueued).count());
        if (!dedupe.first_time(o->id)) { dropped.fetch_add(1, std::memory_order_relaxed); continue; }
        revenue.fetch_add(o->total_halalas, std::memory_order_relaxed);  // "send to kitchen"
        processed.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  std::vector<std::thread> producers;
  for (int b = 0; b < cfg.branches; ++b) {
    producers.emplace_back([&, b] {
      std::mt19937 rng(b);  // fixed seed per branch: runs are repeatable
      std::uniform_int_distribution<std::uint32_t> price(1'500, 9'000);
      std::uniform_int_distribution<int> pct(0, 99);
      for (int i = 0; i < cfg.orders_per_branch; ++i) {
        // Unique ID: branch in the high bits, sequence in the low bits.
        Order o{(std::uint64_t(b) << 32) | std::uint32_t(i), std::uint32_t(b), price(rng), Clock::now()};
        queue.push(o);
        if (pct(rng) < cfg.retry_pct) { o.enqueued = Clock::now(); queue.push(o); }  // retried delivery
      }
    });
  }

  for (auto& t : producers) t.join();
  queue.close();
  for (auto& t : workers) t.join();

  double secs = std::chrono::duration<double>(Clock::now() - start).count();
  std::vector<std::int64_t> all;
  for (auto& v : waits) all.insert(all.end(), v.begin(), v.end());

  const std::uint64_t expected = std::uint64_t(cfg.branches) * cfg.orders_per_branch;
  std::cout << "branches=" << cfg.branches << " workers=" << cfg.workers << " queue=" << cfg.queue_cap << "\n"
            << "processed=" << processed << " (expected " << expected << ")\n"
            << "duplicates_dropped=" << dropped << "\n"
            << "throughput=" << static_cast<long>(all.size() / secs) << " msgs/s\n"
            << "queue_wait_us p50=" << percentile(all, 50) << " p95=" << percentile(all, 95)
            << " p99=" << percentile(all, 99) << "\n";

  return processed == expected ? 0 : 1;  // non-zero if any order was lost or double-counted
}
