cmake_minimum_required(VERSION 3.20)
project(itch_orderbook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(FH_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(FH_STD_INDEX "Use std::unordered_map for the order index (A/B comparison)" OFF)
option(FH_TSAN "Build with ThreadSanitizer (for the SPSC ring / pipeline)" OFF)

add_compile_options(-Wall -Wextra -Wpedantic)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
# Keep frame pointers so `perf record -g` gives usable call stacks.
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O3 -g -fno-omit-frame-pointer -DNDEBUG")

if(FH_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
if(FH_TSAN)
  add_compile_options(-fsanitize=thread)
  add_link_options(-fsanitize=thread)
endif()

find_package(Threads REQUIRED)

add_library(fh STATIC src/order_book.cpp src/pipeline.cpp)
target_include_directories(fh PUBLIC include)
target_link_libraries(fh PUBLIC Threads::Threads)
if(FH_STD_INDEX)
  target_compile_definitions(fh PUBLIC FH_STD_INDEX)
endif()

add_executable(replay apps/replay.cpp)
target_link_libraries(replay PRIVATE fh)

add_executable(bench apps/bench.cpp)
target_link_libraries(bench PRIVATE fh)

add_executable(gen_synthetic tools/gen_synthetic.cpp)
target_link_libraries(gen_synthetic PRIVATE fh)

# ---- tests ----
include(CTest)
if(BUILD_TESTING)
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()
  add_executable(fh_tests tests/test_itch.cpp tests/test_order_book.cpp tests/test_spsc_mold.cpp)
  target_link_libraries(fh_tests PRIVATE fh GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(fh_tests)
endif()
