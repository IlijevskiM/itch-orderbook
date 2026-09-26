#pragma once
// fake but valid ITCH 5.0 stream (same length-prefixed layout as NASDAQ's
// files) so I can test/bench without downloading a 10GB file every time.
// real numbers should still come from real data
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "fh/endian.hpp"

namespace fh {

class ItchWriter {
public:
    std::vector<uint8_t> buf;

    void stock_directory(uint16_t locate, const std::string& sym, uint64_t ts) {
        uint8_t m[39] = {};
        header(m, 'R', locate, ts);
        for (int i = 0; i < 8; ++i) m[11 + i] = i < int(sym.size()) ? uint8_t(sym[i]) : ' ';
        m[19] = 'Q';  // market category
        emit(m, sizeof m);
    }
    void add(uint16_t locate, uint64_t ts, uint64_t ref, char side, uint32_t shares,
             const std::string& sym, uint32_t price) {
        uint8_t m[36] = {};
        header(m, 'A', locate, ts);
        write_be64(m + 11, ref);
        m[19] = uint8_t(side);
        write_be32(m + 20, shares);
        for (int i = 0; i < 8; ++i) m[24 + i] = i < int(sym.size()) ? uint8_t(sym[i]) : ' ';
        write_be32(m + 32, price);
        emit(m, sizeof m);
    }
    void execute(uint16_t locate, uint64_t ts, uint64_t ref, uint32_t shares, uint64_t match) {
        uint8_t m[31] = {};
        header(m, 'E', locate, ts);
        write_be64(m + 11, ref);
        write_be32(m + 19, shares);
        write_be64(m + 23, match);
        emit(m, sizeof m);
    }
    void cancel(uint16_t locate, uint64_t ts, uint64_t ref, uint32_t shares) {
        uint8_t m[23] = {};
        header(m, 'X', locate, ts);
        write_be64(m + 11, ref);
        write_be32(m + 19, shares);
        emit(m, sizeof m);
    }
    void del(uint16_t locate, uint64_t ts, uint64_t ref) {
        uint8_t m[19] = {};
        header(m, 'D', locate, ts);
        write_be64(m + 11, ref);
        emit(m, sizeof m);
    }
    void replace(uint16_t locate, uint64_t ts, uint64_t old_ref, uint64_t new_ref,
                 uint32_t shares, uint32_t price) {
        uint8_t m[35] = {};
        header(m, 'U', locate, ts);
        write_be64(m + 11, old_ref);
        write_be64(m + 19, new_ref);
        write_be32(m + 27, shares);
        write_be32(m + 31, price);
        emit(m, sizeof m);
    }
    void trade(uint16_t locate, uint64_t ts) {  // 'P', ignored by the book
        uint8_t m[44] = {};
        header(m, 'P', locate, ts);
        emit(m, sizeof m);
    }

private:
    static void header(uint8_t* m, char type, uint16_t locate, uint64_t ts) {
        m[0] = uint8_t(type);
        write_be16(m + 1, locate);
        write_be16(m + 3, 0);
        write_be48(m + 5, ts);
    }
    void emit(const uint8_t* m, uint16_t len) {
        uint8_t prefix[2];
        write_be16(prefix, len);
        buf.insert(buf.end(), prefix, prefix + 2);
        buf.insert(buf.end(), m, m + len);
    }
};

// tries to look roughly like a real day: lots of adds + deletes, fewer
// executes/replaces, prices bunched around a mid that drifts a bit
inline std::vector<uint8_t> generate_synthetic(size_t n_messages, int n_symbols = 500,
                                               uint32_t seed = 42) {
    ItchWriter w;
    std::mt19937_64 rng(seed);
    std::vector<uint32_t> mid(size_t(n_symbols) + 1);
    struct Live { uint64_t ref; uint32_t shares; char side; };
    std::vector<std::vector<Live>> live(size_t(n_symbols) + 1);
    uint64_t ts = 34'200'000'000'000ULL;  // 9:30 am
    for (int s = 1; s <= n_symbols; ++s) {
        w.stock_directory(uint16_t(s), "SYM" + std::to_string(s), ts);
        mid[size_t(s)] = 100'000 + uint32_t(rng() % 4'000'000);  // $10 - $410
    }
    uint64_t next_ref = 1, match = 1;
    std::uniform_int_distribution<int> sym_dist(1, n_symbols);
    std::uniform_int_distribution<int> pct(0, 99);

    auto drop = [](std::vector<Live>& v, size_t k) { v[k] = v.back(); v.pop_back(); };

    for (size_t i = 0; i < n_messages; ++i) {
        ts += 1 + rng() % 2000;
        const int s = sym_dist(rng);
        auto& orders = live[size_t(s)];
        const int roll = pct(rng);
        if (orders.empty() || roll < 45) {
            const char side = (rng() & 1) ? 'B' : 'S';
            const uint32_t off = uint32_t(rng() % 50) * 100;  // up to 50 ticks away
            const uint32_t px = side == 'B' ? mid[size_t(s)] - off - 100
                                            : mid[size_t(s)] + off + 100;
            const uint64_t ref = next_ref++;
            const uint32_t shares = 100 * uint32_t(1 + rng() % 10);
            w.add(uint16_t(s), ts, ref, side, shares, "SYM" + std::to_string(s), px);
            orders.push_back({ref, shares, side});
        } else {
            const size_t k = rng() % orders.size();
            Live& o = orders[k];
            if (roll < 75) {
                w.del(uint16_t(s), ts, o.ref);
                drop(orders, k);
            } else if (roll < 93) {
                // cancel/execute 100 shares. if that takes it to 0 the order is
                // gone, same as the real feed, so stop referencing it
                if (roll < 85) w.cancel(uint16_t(s), ts, o.ref, 100);
                else w.execute(uint16_t(s), ts, o.ref, 100, match++);
                o.shares -= 100;
                if (o.shares == 0) drop(orders, k);
            } else if (roll < 97) {
                const uint64_t nref = next_ref++;
                const uint32_t px = o.side == 'B' ? mid[size_t(s)] - 100 : mid[size_t(s)] + 100;
                w.replace(uint16_t(s), ts, o.ref, nref, 200, px);
                o = {nref, 200, o.side};
            } else {
                w.trade(uint16_t(s), ts);
                mid[size_t(s)] += (rng() & 1) ? 100 : uint32_t(-100);
            }
        }
    }
    return std::move(w.buf);
}

}  // namespace fh
