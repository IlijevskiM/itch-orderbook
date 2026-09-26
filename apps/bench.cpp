// quick benchmark on synthetic data: single thread vs pipeline, at full speed
// and at a paced rate (for latency).
//
// usage: bench [n_messages=20000000] [paced_rate=2000000]
#include <cstdio>
#include <cstdlib>

#include "fh/order_book.hpp"
#include "fh/pipeline.hpp"
#include "fh/synthetic.hpp"

using namespace fh;

static void report(const char* name, const RunResult& r) {
    std::printf("%-28s %7.2f M events/s", name, r.events_per_sec() / 1e6);
    if (r.latency.count())
        std::printf("   p50 %5llu ns  p99 %6llu ns  p99.9 %6llu ns",
                    (unsigned long long)r.latency.percentile(50),
                    (unsigned long long)r.latency.percentile(99),
                    (unsigned long long)r.latency.percentile(99.9));
    std::printf("\n");
}

int main(int argc, char** argv) {
#ifdef FH_STD_INDEX
    std::printf("order index: std::unordered_map (FH_STD_INDEX build)\n");
#else
    std::printf("order index: FlatHashMap (open addressing)\n");
#endif
    const size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 20'000'000;
    const double rate = argc > 2 ? std::atof(argv[2]) : 2'000'000;

    std::printf("generating %zu synthetic messages...\n", n);
    auto data = generate_synthetic(n);
    std::printf("stream size: %.1f MB\n\n", data.size() / 1e6);

    {
        OrderBookManager books;
        report("single thread", run_single_thread(data.data(), data.size(), books));
    }
    {
        OrderBookManager books;
        PipelineOptions o;
        o.measure_latency = false;
        report("pipeline (max throughput)", run_pipeline(data.data(), data.size(), books, o));
    }
    {
        OrderBookManager books;
        PipelineOptions o;
        o.measure_latency = true;
        o.target_rate = rate;
        char name[64];
        std::snprintf(name, sizeof name, "pipeline (paced %.1fM/s)", rate / 1e6);
        report(name, run_pipeline(data.data(), data.size(), books, o));
    }
    std::printf("\nNote: max-throughput latency is dominated by queueing, so measure latency "
                "at a paced rate below capacity.\n");
    return 0;
}
