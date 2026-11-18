cmake_minimum_required(VERSION 3.16)
project(order_dispatch CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
find_package(Threads REQUIRED)
add_compile_options(-Wall -Wextra -Wpedantic)

add_executable(order_dispatch src/main.cpp)
target_link_libraries(order_dispatch Threads::Threads)

add_executable(order_dispatch_tests tests/test_main.cpp)
target_compile_options(order_dispatch_tests PRIVATE -UNDEBUG)  # keep asserts on in Release
target_link_libraries(order_dispatch_tests Threads::Threads)

enable_testing()
add_test(NAME unit COMMAND order_dispatch_tests)
add_test(NAME end_to_end COMMAND order_dispatch --orders 20000)
