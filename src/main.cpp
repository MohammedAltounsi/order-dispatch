// order-dispatch: branch threads -> bounded queue -> worker threads -> dedupe -> kitchen.
// A share of orders is delivered twice, like a delivery app retrying a webhook.
//
// Two load modes:
//   --rate 0 (default)  Closed loop. Branches push as fast as the queue accepts.
//                       Reports throughput only: in a closed loop the producers slow down
//                       to match the workers, so any latency number would be misleading.
//   --rate N            Open loop. N orders/s arrive on a fixed schedule whether or not the
//                       workers keep up. Latency runs from the scheduled arrival to the end of
//                       processing. A full queue rejects the order (shed, like a 429) instead
//                       of slowing arrivals down.
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "bounded_queue.hpp"
#include "dedupe.hpp"
#include "mpmc_ring.hpp"
#include "stats.hpp"

using Clock = std::chrono::steady_clock;
using std::chrono::nanoseconds;

struct Order {
  std::uint64_t id = 0;
  std::uint32_t branch = 0;
  std::uint32_t total_halalas = 0;  // money as integers
  Clock::time_point arrival{};      // scheduled arrival (open loop)
};

struct Config {
  std::string queue = "mutex";
  long long branches = 8, workers = 4, orders = 50'000, capacity = 1024, shards = 64;
  long long retry_pct = 5, rate = 0, work_ns = 0;
};

[[noreturn]] static void usage(const std::string& why) {
  std::cerr << "error: " << why << "\n"
            << "usage: order_dispatch [--queue mutex|ring] [--branches N] [--workers N] [--orders N-per-branch]\n"
            << "                      [--capacity N] [--shards N] [--retry-pct 0-100] [--rate orders/s] [--work-ns N]\n";
  std::exit(2);
}

static bool pow2(long long v) { return v > 0 && (v & (v - 1)) == 0; }

static Config parse(int argc, char** argv) {
  Config c;
  for (int i = 1; i < argc; i += 2) {
    std::string_view k = argv[i];
    if (i + 1 >= argc) usage(std::string(k) + " needs a value");
    std::string_view v = argv[i + 1];
    if (k == "--queue") { c.queue = v; continue; }
    long long n = 0;
    auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
    if (ec != std::errc{} || end != v.data() + v.size()) usage(std::string(k) + ": not a whole number");
    if (k == "--branches") c.branches = n;
    else if (k == "--workers") c.workers = n;
    else if (k == "--orders") c.orders = n;
    else if (k == "--capacity") c.capacity = n;
    else if (k == "--shards") c.shards = n;
    else if (k == "--retry-pct") c.retry_pct = n;
    else if (k == "--rate") c.rate = n;
    else if (k == "--work-ns") c.work_ns = n;
    else usage("unknown flag " + std::string(k));
  }
  if (c.queue != "mutex" && c.queue != "ring") usage("--queue must be mutex or ring");
  if (c.branches < 1 || c.branches > 4096 || c.workers < 1 || c.workers > 256) usage("branches 1-4096, workers 1-256");
  if (c.orders < 1 || c.capacity < 1 || c.rate < 0 || c.work_ns < 0) usage("orders, capacity >= 1; rate, work-ns >= 0");
  if (c.retry_pct < 0 || c.retry_pct > 100) usage("--retry-pct must be 0-100");
  if (!pow2(c.shards)) usage("--shards must be a power of two");
  if (c.queue == "ring" && (!pow2(c.capacity) || c.capacity < 2)) usage("ring --capacity must be a power of two >= 2");
  return c;
}

// Stand-in for real per-order work (writing to the branch POS). Busy-waits so the
// worker's capacity is predictable: workers / work_ns orders per second.
static void work(long long ns) {
  if (ns <= 0) return;
  auto until = Clock::now() + nanoseconds(ns);
  while (Clock::now() < until) {}
}

struct alignas(64) WorkerStats {  // one cache line apart, so workers never write to each other's line
  std::vector<std::int64_t> latency_ns;
  std::uint64_t processed = 0, dropped = 0, revenue = 0;
};

struct alignas(64) ProducerStats {
  std::uint64_t accepted = 0, shed = 0, unique_accepted = 0;
};

template <class Queue>
static int run(const Config& cfg) {
  const bool open = cfg.rate > 0;
  const auto total = static_cast<std::uint64_t>(cfg.branches * cfg.orders);
  Queue queue(static_cast<std::size_t>(cfg.capacity));
  ShardedDedupe dedupe(static_cast<std::size_t>(cfg.shards), total);
  std::vector<WorkerStats> ws(cfg.workers);
  std::vector<ProducerStats> ps(cfg.branches);
  if (open) for (auto& w : ws) w.latency_ns.reserve(total / cfg.workers + total / 10);

  const auto t0 = Clock::now() + std::chrono::milliseconds(20);  // every thread is started by then
  const nanoseconds interval(open ? static_cast<long long>(1e9 * cfg.branches / cfg.rate) : 0);

  std::vector<std::thread> workers;
  for (int w = 0; w < cfg.workers; ++w) {
    workers.emplace_back([&, w] {
      WorkerStats& me = ws[w];
      while (auto o = queue.pop()) {
        if (!dedupe.first_time(o->id)) { ++me.dropped; continue; }
        work(cfg.work_ns);
        ++me.processed;
        me.revenue += o->total_halalas;
        if (open) me.latency_ns.push_back((Clock::now() - o->arrival).count());
      }
    });
  }

  std::vector<std::thread> producers;
  for (int b = 0; b < cfg.branches; ++b) {
    producers.emplace_back([&, b] {
      ProducerStats& me = ps[b];
      std::mt19937 rng(static_cast<unsigned>(b));  // fixed seed per branch: same orders every run
      std::uniform_int_distribution<std::uint32_t> price(1'500, 9'000);
      std::uniform_int_distribution<int> pct(0, 99);
      for (long long i = 0; i < cfg.orders; ++i) {
        Order o{(std::uint64_t(b) << 32) | std::uint64_t(i), std::uint32_t(b), price(rng), t0 + i * interval};
        const bool retried = pct(rng) < cfg.retry_pct;
        bool first = false, second = false;
        if (open) {
          if (Clock::now() < o.arrival) std::this_thread::sleep_until(o.arrival);
          first = queue.try_push(o);
          if (retried) second = queue.try_push(o);  // the retry keeps the original arrival time
          me.shed += !first + (retried && !second);
        } else {
          first = queue.push(o);
          if (retried) second = queue.push(o);
        }
        me.accepted += first + second;
        me.unique_accepted += first || second;
      }
    });
  }

  for (auto& t : producers) t.join();
  queue.close();
  for (auto& t : workers) t.join();
  const double secs = std::chrono::duration<double>(Clock::now() - (open ? t0 : t0 - std::chrono::milliseconds(20))).count();

  std::uint64_t processed = 0, dropped = 0, accepted = 0, shed = 0, unique = 0;
  std::vector<std::int64_t> lat;
  for (auto& w : ws) { processed += w.processed; dropped += w.dropped; lat.insert(lat.end(), w.latency_ns.begin(), w.latency_ns.end()); }
  for (auto& p : ps) { accepted += p.accepted; shed += p.shed; unique += p.unique_accepted; }

  // Every order that got into the queue at least once is processed exactly once,
  // and every other accepted copy is dropped as a duplicate.
  const bool ok = processed == unique && dropped == accepted - processed;

  std::printf("queue=%s branches=%lld workers=%lld capacity=%lld shards=%lld rate=%lld work_ns=%lld\n",
              cfg.queue.c_str(), cfg.branches, cfg.workers, cfg.capacity, cfg.shards, cfg.rate, cfg.work_ns);
  std::printf("orders=%llu processed=%llu duplicates_dropped=%llu shed=%llu shed_pct=%.1f\n",
              (unsigned long long)total, (unsigned long long)processed, (unsigned long long)dropped,
              (unsigned long long)shed, 100.0 * double(shed) / double(accepted + shed));
  std::printf("throughput=%.0f orders/s\n", double(processed) / secs);
  if (open) {
    std::sort(lat.begin(), lat.end());
    std::printf("latency_us p50=%.1f p99=%.1f p999=%.1f max=%.1f\n", percentile(lat, 50) / 1e3,
                percentile(lat, 99) / 1e3, percentile(lat, 99.9) / 1e3, (lat.empty() ? 0 : lat.back()) / 1e3);
  }
  std::printf("check=%s\n", ok ? "ok" : "FAILED");
  return ok ? 0 : 1;
}

int main(int argc, char** argv) {
  const Config cfg = parse(argc, argv);
  return cfg.queue == "ring" ? run<MpmcRing<Order>>(cfg) : run<BoundedQueue<Order>>(cfg);
}
