FROM gcc:14
RUN apt-get update && apt-get install -y --no-install-recommends cmake && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY . .
RUN cmake -S . -B build && cmake --build build -j
# Tests run once with ThreadSanitizer to catch data races, then the normal build runs.
RUN g++ -std=c++20 -O1 -g -fsanitize=thread -UNDEBUG tests/test_main.cpp -o tsan_tests -pthread && ./tsan_tests
CMD ["sh", "-c", "cd build && ctest --output-on-failure && ./order_dispatch"]
