# order-dispatch

Restaurant branches send orders to kitchen workers through a bounded queue. Some orders arrive
twice, the way a delivery app retries a webhook, and every repeat has to be dropped exactly once.
C++20, about 500 lines, no dependencies.

```
branch threads ──► bounded queue ──► worker threads ──► dedupe (sharded) ──► kitchen
                   mutex or lock-free                   one lock per shard
```

I wrote it to measure three questions I kept hand-waving about: how much a lock-free queue
actually buys, how much one shared lock costs, and what queue size does to latency when a
rush arrives faster than the kitchen can work.

## Results

Docker (WSL2) on an i5-14600K, 20 hardware threads, mixed performance and efficiency cores.
Each row is the median of 5 runs after a warm-up, with the min–max range in brackets.
`./bench.sh` reproduces all of it.

### 1. Mutex queue vs lock-free ring

Closed loop: 8 branches push as fast as the queue accepts, 1.6M orders.

| Queue, workers | Throughput (orders/s) |
|---|---|
| mutex, 4 | 310,966 (301,828–315,272) |
| ring, 4 | 4,561,051 (4,447,107–4,638,827) |
| mutex, 8 | 391,573 (368,669–473,687) |
| ring, 8 | 5,104,784 (4,527,589–5,375,883) |

The ring is about 15× faster here. With 16 threads on one mutex, most of the time goes to
waiting for the lock and waking sleeping threads. The ring has no lock, and each slot sits on its
own cache line. The cost: an idle ring consumer spins instead of sleeping, so it burns a core when
there is no work. With bursty, low traffic the mutex queue is the better default.

### 2. One lock vs 64 for the dedupe set

Ring queue, 8 workers.

| Shards | Throughput (orders/s) |
|---|---|
| 1 | 2,473,365 (2,437,830–2,504,152) |
| 64 | 6,133,245 (4,471,876–6,350,638) |

With one shard, every worker takes the same mutex on every order. Splitting the set into 64,
each on its own cache line, gives 2.5×. A bug I hit along the way: libstdc++'s
`std::hash<uint64_t>` returns the ID unchanged, and my IDs put the branch in the high bits, so
`id % 64` sent order *i* of every branch to the same shard at the same moment. The shard is now
picked with the splitmix64 finalizer, and a test checks the spread.

### 3. A rush faster than the kitchen

Open loop: orders arrive on a fixed schedule whether or not workers keep up, and latency runs
from each order's scheduled arrival to the end of processing. 4 workers spend 20 µs per order,
so capacity is about 200k orders/s on paper. Measured, it's about 174k/s.

| Offered load, queue capacity | p50 (µs) | p99 (µs) | p99.9 (µs) | Shed |
|---|---|---|---|---|
| 100k/s, 1024 | 126 | 297 | 408 | 0% |
| 180k/s, 1024 | 261 | 1,975 | 2,180 | 0% |
| 300k/s, 1024 | 5,662 | 6,211 | 6,350 | 42.1% |
| 300k/s, 64 | 401 | 590 | 703 | 42.2% |

- **Near capacity, the tail grows first.** At 90% load the median barely moves but p99 is 7×
  higher than at 50%, and it varies a lot run to run (1.0–5.8 ms). That's queueing: small
  bursts pile up when there's no slack.
- **Over capacity, the queue size sets the latency.** Both sizes shed the same share, because the
  excess has nowhere to go. The difference is what happens to the orders that are accepted. A
  1024 queue makes each one wait behind about 1000 others, roughly 6 ms. A 64 queue keeps p99
  under 0.6 ms. By Little's law, wait ≈ queue length ÷ throughput: 1024 ÷ 174k ≈ 5.9 ms.
- So the queue bound is a latency budget. Size it from the wait you can accept, and reject the rest
  early (an HTTP 429 with Retry-After) so the client can retry or route elsewhere.

The p50 of 126 µs at light load is more than the 20 µs of work because it includes the
generator thread waking up late from `sleep_until`. Those late wake-ups count against the
system, as they would for a real customer.

## Measuring honestly

My first version produced a nicer headline: shrinking the queue cut wait 7×. It was wrong.
Each branch only created its next order after the queue accepted the last one, so when the queue
was full, the waiting happened in orders that didn't exist yet and never got a timestamp
(coordinated omission; see Gil Tene's wrk2). The fixed generator keeps a schedule, measures from
the scheduled time, and sheds what doesn't fit.

Every run checks itself and exits non-zero on failure: every order accepted at least once is
processed exactly once, and every other accepted copy is counted as a dropped duplicate.

## Run

```bash
docker build -t order-dispatch .
docker run --rm order-dispatch                                   # one default run
docker run --rm order-dispatch ./bench.sh                        # all tables above
docker run --rm --security-opt seccomp=unconfined order-dispatch ./ci.sh
```

`ci.sh` builds with `-Wall -Wextra -Wpedantic -Werror`, runs the tests, then runs the tests and
the binary (both queues, both load modes) under ThreadSanitizer and under AddressSanitizer +
UndefinedBehaviorSanitizer. TSan needs address randomisation off, hence the seccomp flag locally
and the `sysctl` line in the GitHub workflow.

Flags: `--queue mutex|ring`, `--branches`, `--workers`, `--orders` (per branch), `--capacity`,
`--shards`, `--retry-pct`, `--rate` (orders/s, 0 = closed loop), `--work-ns`.

## Files

- `src/mpmc_ring.hpp`: lock-free bounded queue, Dmitry Vyukov's design. Each slot has a
  sequence number saying whose turn it is; producers and consumers claim positions with one
  compare-and-swap and publish with release/acquire ordering.
- `src/bounded_queue.hpp`: mutex + condition variable queue with the same interface.
- `src/dedupe.hpp`: sharded set of seen order IDs.
- `src/main.cpp`: load generator, workers, stats, self-check.
- `tests/test_main.cpp`: exactly-once under 8 racing threads, nothing lost through a 4-slot queue
  for both queues, FIFO, full-queue rejection, shard spread, percentile maths.

## Not done

- The seen-ID set never shrinks. A long-running service would expire IDs, for example two sets
  rotated every retry window.
- Single process, in memory. Across machines the queue is Kafka/SQS and the dedupe is a unique
  key in the database.
- Threads aren't pinned to cores, so the P-core/E-core mix adds noise; that's part of why the
  ranges are reported.
