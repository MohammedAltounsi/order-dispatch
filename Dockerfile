FROM gcc:14
RUN apt-get update && apt-get install -y --no-install-recommends cmake && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-Werror && cmake --build build -j
# Full CI (tests + sanitizers) needs ASLR off for TSan, which a build step can't do:
#   docker run --rm --security-opt seccomp=unconfined order-dispatch ./ci.sh
CMD ["./build/order_dispatch"]
