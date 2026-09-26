#pragma once
// two threads: decode thread parses ITCH and pushes Events into the SPSC ring,
// book thread pops them and updates the books.
//
// splitting it lets decode and book-building run on different cores. whether
// that's actually faster than one thread depends on the machine (on 2 cores
// it wasn't!), which is why bench runs both
#include <cstddef>
#include <cstdint>

#include "fh/itch.hpp"
#include "fh/latency.hpp"
#include "fh/order_book.hpp"

namespace fh {

struct RunResult {
    StreamStats stream;
    uint64_t events_applied = 0;
    double seconds = 0;
    LatencyHistogram latency;  // decode -> applied, empty unless measure_latency

    double events_per_sec() const { return seconds > 0 ? events_applied / seconds : 0; }
};

struct PipelineOptions {
    bool measure_latency = true;
    // > 0 = producer throttles itself to this many events/sec. otherwise at
    // full speed the ring fills up and "latency" is really just queueing time
    double target_rate = 0;
    int producer_cpu = -1;  // cpu to pin to (linux only), -1 = don't pin
    int consumer_cpu = -1;
};

// everything on one thread (the baseline to compare against)
RunResult run_single_thread(const uint8_t* data, size_t size, OrderBookManager& books);

// decode + apply on two threads with the ring in between
RunResult run_pipeline(const uint8_t* data, size_t size, OrderBookManager& books,
                       const PipelineOptions& opts = {});

}  // namespace fh
