#!/bin/sh
# Build, unit + end-to-end tests, then the tests AND the real binary under
# ThreadSanitizer (data races) and AddressSanitizer + UBSan (memory and undefined behaviour).
set -eu
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-Werror
cmake --build build -j
(cd build && ctest --output-on-failure)

# TSan aborts on kernels with high ASLR entropy ("unexpected memory mapping"); run without ASLR.
NOASLR=""
if setarch "$(uname -m)" -R true 2>/dev/null; then NOASLR="setarch $(uname -m) -R"; fi
for san in thread address,undefined; do
  echo "== sanitizer: $san"
  flags="-std=c++20 -O1 -g -fno-omit-frame-pointer -fsanitize=$san -pthread"
  g++ $flags tests/test_main.cpp -o /tmp/tests_san && $NOASLR /tmp/tests_san
  g++ $flags src/main.cpp -o /tmp/main_san
  $NOASLR /tmp/main_san --queue mutex --orders 5000 >/dev/null
  $NOASLR /tmp/main_san --queue ring --orders 5000 >/dev/null
  $NOASLR /tmp/main_san --queue ring --rate 100000 --work-ns 20000 --capacity 64 --orders 5000 >/dev/null
done
echo "ci: all green"
