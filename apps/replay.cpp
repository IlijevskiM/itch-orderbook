// replay an ITCH 5.0 file through the books, print throughput / latency /
// a snapshot of one symbol's book.
//
// usage:
//   replay <file.itch> [--symbol AAPL] [--mode single|pipeline]
//          [--rate EVENTS_PER_SEC] [--pin P,C] [--mold-drop N]
//
// --mold-drop N: repackage the file as MoldUDP64 packets, drop every Nth one,
//                and check the SequenceTracker catches the gaps
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "fh/mapped_file.hpp"
#include "fh/moldudp64.hpp"
#include "fh/order_book.hpp"
#include "fh/pipeline.hpp"

using namespace fh;

static void print_book(const OrderBookManager& books, const std::string& symbol) {
    auto loc = books.locate_of(symbol);
    if (!loc) { std::printf("symbol %s not found\n", symbol.c_str()); return; }
    const Book* b = books.book(*loc);
    if (!b) { std::printf("no book for %s\n", symbol.c_str()); return; }
    std::printf("\n%s  (%zu bid levels, %zu ask levels, traded volume %llu)\n", symbol.c_str(),
                b->bid_levels(), b->ask_levels(), (unsigned long long)b->traded_volume);
    auto bids = b->depth('B', 5), asks = b->depth('S', 5);
    std::printf("  %12s %10s | %-10s %-12s\n", "bid size", "bid", "ask", "ask size");
    for (size_t i = 0; i < 5; ++i) {
        char l[64] = "", r[64] = "";
        if (i < bids.size())
            std::snprintf(l, sizeof l, "%12llu %10.4f", (unsigned long long)bids[i].shares,
                          bids[i].price / 1e4);
        if (i < asks.size())
            std::snprintf(r, sizeof r, "%-10.4f %-12llu", asks[i].price / 1e4,
                          (unsigned long long)asks[i].shares);
        std::printf("  %23s | %s\n", l, r);
    }
}

static void simulate_mold_loss(const MappedFile& f, uint64_t drop_every) {
    const char session[11] = "SIMSESSION";
    mold::PacketBuilder builder(session, 1);
    mold::SequenceTracker tracker(1);
    uint64_t packets = 0, dropped = 0;

    auto deliver = [&](const std::vector<uint8_t>& pkt) {
        ++packets;
        if (drop_every && packets % drop_every == 0) { ++dropped; return; }
        mold::Header h{};
        if (!mold::parse_header(pkt.data(), pkt.size(), h)) return;
        tracker.on_packet(h.sequence, h.count);
    };

    size_t pos = 0;
    const uint8_t* d = f.data();
    while (pos + 2 <= f.size()) {
        const uint16_t len = read_be16(d + pos);
        if (pos + 2 + len > f.size()) break;
        if (!builder.add(d + pos + 2, len)) {
            deliver(builder.flush());
            builder.add(d + pos + 2, len);
        }
        pos += 2 + size_t(len);
    }
    if (!builder.empty()) deliver(builder.flush());

    std::printf("\nMoldUDP64 simulation: %llu packets, %llu dropped\n",
                (unsigned long long)packets, (unsigned long long)dropped);
    std::printf("  gaps detected: %zu, messages missing: %llu\n", tracker.gaps().size(),
                (unsigned long long)tracker.missing_messages());
    for (size_t i = 0; i < tracker.gaps().size() && i < 3; ++i)
        std::printf("  gap %zu: seq %llu..%llu\n", i + 1,
                    (unsigned long long)tracker.gaps()[i].first_missing,
                    (unsigned long long)tracker.gaps()[i].last_missing);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <file.itch> [--symbol SYM] [--mode single|pipeline] "
                     "[--rate N] [--pin P,C] [--mold-drop N]\n", argv[0]);
        return 1;
    }
    std::string path = argv[1], symbol, mode = "pipeline";
    PipelineOptions opts;
    uint64_t mold_drop = 0;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--symbol") symbol = next();
        else if (a == "--mode") mode = next();
        else if (a == "--rate") opts.target_rate = std::atof(next().c_str());
        else if (a == "--mold-drop") mold_drop = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--pin") {
            std::string v = next();
            std::sscanf(v.c_str(), "%d,%d", &opts.producer_cpu, &opts.consumer_cpu);
        }
    }

    MappedFile file(path);
    std::printf("file: %s (%.1f MB)\n", path.c_str(), file.size() / 1e6);

    OrderBookManager books;
    RunResult r = mode == "single" ? run_single_thread(file.data(), file.size(), books)
                                   : run_pipeline(file.data(), file.size(), books, opts);

    std::printf("mode: %s\n", mode.c_str());
    std::printf("messages: %llu  book events: %llu  bad length: %llu  unknown type: %llu\n",
                (unsigned long long)r.stream.messages, (unsigned long long)r.stream.book_events,
                (unsigned long long)r.stream.bad_length, (unsigned long long)r.stream.unknown_type);
    std::printf("elapsed: %.3f s   throughput: %.2f M events/s\n", r.seconds,
                r.events_per_sec() / 1e6);
    if (r.latency.count()) {
        std::printf("latency decode->applied (ns): p50 %llu  p99 %llu  p99.9 %llu  max %llu\n",
                    (unsigned long long)r.latency.percentile(50),
                    (unsigned long long)r.latency.percentile(99),
                    (unsigned long long)r.latency.percentile(99.9),
                    (unsigned long long)r.latency.max());
    }
    const auto& s = books.stats();
    std::printf("adds %llu  executes %llu  cancels %llu  deletes %llu  replaces %llu  "
                "unknown refs %llu\n",
                (unsigned long long)s.adds, (unsigned long long)s.executes,
                (unsigned long long)s.cancels, (unsigned long long)s.deletes,
                (unsigned long long)s.replaces, (unsigned long long)s.unknown_refs);
    std::printf("symbols: %zu  live orders at end: %zu\n", books.symbol_count(),
                books.live_orders());

    if (!symbol.empty()) print_book(books, symbol);
    if (mold_drop) simulate_mold_loss(file, mold_drop);
    return 0;
}
