# itch-orderbook

A C++20 market data feed handler that decodes NASDAQ TotalView-ITCH 5.0, rebuilds a
limit order book for every symbol, and runs decode and book-building on separate cores
connected by a lock-free ring buffer.

```
 ITCH file (mmap) ──► decode thread ──► SPSC ring (64K cache-line slots) ──► book thread ──► per-symbol books
                        zero-copy                 acquire/release atomics         O(1) add/cancel/execute
                        MoldUDP64 gap detection
```

## Build and test

Requires CMake 3.20+ and a C++20 compiler (GCC 11+, Clang 14+, Apple Clang 15+).
GoogleTest is found on the system or downloaded automatically.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

# sanitizers (run these before every commit that touches the ring or the book)
cmake -S . -B build-tsan -DFH_TSAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build-tsan -j && ./build-tsan/fh_tests
cmake -S . -B build-asan -DFH_SANITIZE=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build-asan -j && ./build-asan/fh_tests
```

## Run it

```bash
# synthetic data (no download needed)
./build/gen_synthetic data/syn.itch 5000000 500
./build/replay data/syn.itch --symbol SYM7 --mold-drop 500
./build/bench 20000000 1000000
```

### Real NASDAQ data

NASDAQ publishes full-day ITCH 5.0 sample files at <https://emi.nasdaq.com/ITCH/>.

* Start with a **Nasdaq BX** or **PSX** file (smaller exchanges, a few hundred MB).
* Then run a full **Nasdaq ITCH** day (e.g. `01302019.NASDAQ_ITCH50.gz`, ~4-5 GB compressed,
  ~10 GB and ~400M messages uncompressed). You need the disk space.

```bash
mkdir -p data && cd data
curl -O "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/01302019.NASDAQ_ITCH50.gz"
gunzip 01302019.NASDAQ_ITCH50.gz
cd ..
./build/replay data/01302019.NASDAQ_ITCH50 --mode single --symbol AAPL
./build/replay data/01302019.NASDAQ_ITCH50 --mode pipeline --symbol AAPL
./build/replay data/01302019.NASDAQ_ITCH50 --mode pipeline --rate 2000000   # latency under load
```

A correct run over a full day shows `bad length: 0`, `unknown type: 0`, and `unknown refs: 0`.

## Benchmarking

How each number is measured (keep results in `RESULTS.md` with the machine, compiler, and
exact command so they can be reproduced):

| Metric | Command | What to read |
|---|---|---|
| messages decoded in a full day | `replay <day> --mode single` | `messages:` |
| throughput | `replay <day> --mode single` and `--mode pipeline` | `throughput:` (median of 5 runs) |
| p50 / p99 latency | `replay <day> --mode pipeline --rate R --pin 2,3` | `latency ... p50 / p99` |
| hash map speedup | build a second copy with `-DFH_STD_INDEX=ON`, run `replay --mode single` on both | ratio of median throughput |

Notes:

* **Latency must be measured at a paced rate below capacity.** At full speed the ring fills
  and you are measuring queueing delay, not the handoff. Find max throughput first, then pace at
  ~50-70% of it.
* **Pin threads** to two physical cores on Linux (`--pin 2,3`); pinning is a no-op on macOS.
* **Profiling with `perf` needs Linux.** On a Mac, use a Linux machine (UMich CAEN Linux login
  servers or a cheap cloud VM) for `perf`, or Instruments (Time Profiler) locally.

```bash
cmake -S . -B build-prof -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build-prof -j
perf stat -e cycles,instructions,cache-misses,branch-misses ./build-prof/replay data/<day> --mode single
perf record -g ./build-prof/replay data/<day> --mode single && perf report
```

Laptop runs are noisy (turbo boost, background stuff), so alternate the two builds run by run
instead of doing all of one then all of the other, and report medians.

## Results

One full NASDAQ BX trading day (`20200130.BX_ITCH_50`: 53.3M messages, 48.7M book updates) on a
2019 13" MacBook Pro (i5-8257U, 4 cores / 8 threads, 8 GB), Apple Clang 17, Release build.
Every run ends with `unknown refs 0` and `live orders at end: 0`, so every execute / cancel /
delete / replace found its order and the books drain to empty at the close.

| Mode | Median | Range |
|---|---|---|
| single thread, `FlatHashMap` (5 runs) | 3.32 M updates/s | 2.32 - 4.21 |
| single thread, `std::unordered_map` (5 runs) | 2.01 M updates/s | 1.64 - 2.26 |
| pipeline, 2 threads (3 runs) | 3.08 M updates/s | 3.02 - 3.09 |

Latency from decode to "applied to the book", with the producer paced:

| Rate | p50 | p99 | max |
|---|---|---|---|
| 500K/s | 411 ns | 85 us | 44 ms |
| 1M/s | 432 ns | > 100 us | 28 ms |
| 2M/s | 598 ns | > 100 us | 19 ms |

What I take from this:

* `FlatHashMap` is about 1.65x faster than `std::unordered_map` (median vs median). The order
  index is hit on every single update, so this is the biggest single win.
* The pipeline is *not* faster than one thread on this laptop, but it is way more consistent
  (3.02-3.09 vs 2.32-4.21). Not sure why yet, profiling it on Linux is next.
* The handoff itself is sub-microsecond at the median. The tail is the OS: macOS can't pin
  threads to cores, and stalls of tens of ms show up in `max`. Needs pinned, isolated cores on
  Linux before the p99 means anything.

## Design decisions

| Decision | Why | Alternatives |
|---|---|---|
| mmap the input file | no copy into user space; the OS pages it in sequentially | `read()` into a buffer; io_uring |
| Zero-copy decode with `memcpy` + `bswap` | fields are big-endian and unaligned; `memcpy` avoids UB and compiles to one load | `#pragma pack` structs (UB on unaligned access, non-portable) |
| Compact 56-byte `Event` | one event per cache line in the ring | pass raw message pointers (then the book thread touches the file's pages) |
| SPSC ring, acquire/release only | exactly one writer per index, so no CAS needed | mutex + condition variable (syscalls, far slower); MPMC queue (needs CAS) |
| head/tail on separate cache lines, cached copies | avoids false sharing; touches the other core's line once per lap | a single shared counter |
| 128-byte lines on Apple Silicon | M-series cache lines are 128 B | `std::hardware_destructive_interference_size` (not in every stdlib yet) |
| Intrusive list per price level | O(1) removal given the `Order*`, no allocation, FIFO = time priority | `std::list` (allocates), vector (O(n) erase) |
| `FlatHashMap` for order refs | inline keys, linear probing, backward-shift delete, no per-node allocation | `std::unordered_map` (build with `FH_STD_INDEX` to compare) |
| `std::map` for price levels | best price at `begin()`, stable node addresses | flat array indexed by ticks from the touch (faster for dense books) |
| Object pool for orders, pre-grown at startup | no malloc/free and no first-touch page faults on the hot path | `new`/`delete` |
| `std::pmr` pool over a pre-faulted arena for price-level map nodes | `std::map` node allocations were faulting in fresh pages on the book thread | a custom node allocator; a flat price array |
| MoldUDP64 sequence tracking | UDP drops packets; a gap means the book is untrustworthy until recovered | request a retransmit from the rewind server, or rebuild from a snapshot |

## Profiling it with waitlens

[waitlens](https://github.com/IlijevskiM/waitlens) (my eBPF latency profiler) showed two things
about the pipeline mode on a 2-vCPU VM with a 5M-message synthetic stream:

1. The book thread took **9,265 page faults** on its hot path: first-touch faults from the order
   pool growing and from `std::map` allocating price-level nodes. Pre-growing the pool and moving
   level nodes onto a `std::pmr::unsynchronized_pool_resource` over a pre-faulted 32 MB arena cut
   that to **28** and raised pipeline throughput about 8% (median of 22 runs each). The faults
   moved to startup on the main thread, which is the point.
2. Nearly every context switch was a preemption (123 of 128), with run-queue waits up to ~4 ms. With only two
   cores the p99 tail comes from the scheduler, so pin threads to isolated cores before
   micro-optimizing the book.

```bash
sudo waitlens -- ./build-prof/replay data/<day> --mode pipeline
```

## Ideas to extend

* Decode real MoldUDP64 packets from a pcap file and recover gaps from a second "retransmit" file.
* Replace `std::map` price levels with a flat tick-indexed array and measure the difference.
* Publish top-of-book updates to other processes over shared memory.
* Validate the book: every `E`/`C` execution must hit a resting order at the right price.
