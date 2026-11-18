# order-dispatch

A multithreaded order dispatcher in C++20. It simulates peak-hour orders from many restaurant
branches flowing to kitchen workers, drops duplicate deliveries, and measures latency.

```
branch threads ──► bounded queue (backpressure) ──► worker threads ──► dedupe ──► kitchen
                   capacity N, blocks when full                      sharded locks
```

## What it shows

- **Backpressure.** The queue has a fixed capacity. When it's full, producers wait instead of
  memory growing without limit.
- **Idempotency.** About 5% of orders are delivered twice, like a delivery app retrying a webhook.
  A sharded set of seen order IDs drops every repeat exactly once.
- **Low lock contention.** The dedupe set is split into 64 shards, each with its own mutex, so
  workers handling different orders rarely wait on each other. Each worker records latency into
  its own vector, so there is no shared lock on the hot path.
- **Tail latency.** It reports p50, p95 and p99 queue-wait time, not the average.
- **Money as integers.** Totals are in halalas.
- **Correctness check.** It exits non-zero if any order was lost or double-counted.

## Run

Needs Docker only. The image builds with GCC 14, runs the unit tests under ThreadSanitizer
(data-race detector), then runs the tests and the benchmark.

```bash
docker build -t order-dispatch .
docker run --rm order-dispatch
docker run --rm order-dispatch ./build/order_dispatch --workers 8 --queue 64
```

Flags: `--branches` (8), `--workers` (4), `--orders` per branch (50000), `--queue` capacity (1024),
`--retry-pct` (5).

## Results

400,000 orders from 8 branches, 5% retried. Docker on an i5-14600K.

| Workers | Queue | Throughput (msgs/s) | p50 wait | p99 wait |
|---|---|---|---|---|
| 1 | 1024 | 102,697 | 9.9 ms | 15.3 ms |
| 4 | 1024 | 266,623 | 3.8 ms | 6.4 ms |
| 8 | 1024 | 547,125 | 1.8 ms | 3.5 ms |
| 8 | 64 | 469,424 | 0.10 ms | 0.51 ms |

**The last row is the point.** Shrinking the queue cut p99 wait about 7× for about 14% less
throughput. A big queue only hides overload: orders sit in it longer (Little's law: wait =
queue length ÷ throughput). A small queue pushes back on producers early, which is what you want
when a customer is waiting for their food.

## Tests

`tests/test_main.cpp`, plain asserts:

- dedupe drops repeats
- 8 threads racing for the same 10,000 IDs win each exactly once
- 4 producers and 4 consumers through a 4-slot queue: nothing lost, nothing duplicated
- a closed queue rejects pushes
- percentile maths

## Limits and next steps

- The seen-ID set grows forever. Production would expire IDs after a time window, or keep them in
  Redis with a TTL.
- It's in-memory and single-process. Across machines the queue becomes Kafka, RabbitMQ or SQS, and
  the dedupe becomes a unique key in the database.
- `std::mutex` + `condition_variable` is simple and correct. A lock-free ring buffer would be
  faster, but only worth it if profiling shows the queue lock is the bottleneck.
