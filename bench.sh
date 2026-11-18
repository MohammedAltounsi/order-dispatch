#!/bin/sh
# Every row: 1 warm-up run, then 5 runs. Prints the median with (min-max).
# Run inside the container: docker run --rm order-dispatch ./bench.sh
set -eu
BIN=${BIN:-./build/order_dispatch}
RUNS=5
OUT=$(mktemp)

# measure "label" field1 [field2 ...] -- flags...
measure() {
  label=$1; shift
  fields=""
  while [ "$1" != "--" ]; do fields="$fields $1"; shift; done; shift
  "$BIN" "$@" >/dev/null
  : >"$OUT"
  i=0; while [ $i -lt $RUNS ]; do "$BIN" "$@" >>"$OUT"; i=$((i + 1)); done
  grep -q "check=FAILED" "$OUT" && { echo "check failed: $*"; exit 1; }
  line="| $label |"
  for f in $fields; do
    vals=$(sed -n "s/.*[ ]$f=\([0-9.]*\).*/\1/p; s/^$f=\([0-9.]*\).*/\1/p" "$OUT" | sort -n)
    med=$(echo "$vals" | sed -n "$(((RUNS + 1) / 2))p")
    line="$line $med ($(echo "$vals" | head -1)-$(echo "$vals" | tail -1)) |"
  done
  echo "$line"
}

echo "cpus: $(nproc)"
echo
echo "## 1. Queue: mutex vs lock-free ring (closed loop, 8 branches, 64 shards, 1.6M orders)"
echo "| Queue, workers | Throughput (orders/s) |"
echo "|---|---|"
for w in 4 8; do
  measure "mutex, $w" throughput -- --queue mutex --workers $w --orders 200000
  measure "ring, $w" throughput -- --queue ring --workers $w --orders 200000
done
echo
echo "## 2. Dedupe lock contention: 1 shard vs 64 (ring queue, 8 workers)"
echo "| Shards | Throughput (orders/s) |"
echo "|---|---|"
measure "1" throughput -- --queue ring --workers 8 --shards 1 --orders 200000
measure "64" throughput -- --queue ring --workers 8 --shards 64 --orders 200000
echo
echo "## 3. Open loop: 4 workers x 20 us of work = 200k orders/s capacity (mutex queue)"
echo "| Offered load, queue capacity | p50 (us) | p99 (us) | p99.9 (us) | Shed % |"
echo "|---|---|---|---|---|"
measure "100k/s (50%), 1024" p50 p99 p999 shed_pct -- --queue mutex --rate 100000 --work-ns 20000 --capacity 1024 --orders 25000
measure "180k/s (90%), 1024" p50 p99 p999 shed_pct -- --queue mutex --rate 180000 --work-ns 20000 --capacity 1024 --orders 45000
measure "300k/s (150%), 1024" p50 p99 p999 shed_pct -- --queue mutex --rate 300000 --work-ns 20000 --capacity 1024 --orders 75000
measure "300k/s (150%), 64" p50 p99 p999 shed_pct -- --queue mutex --rate 300000 --work-ns 20000 --capacity 64 --orders 75000
