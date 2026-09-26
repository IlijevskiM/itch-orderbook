#include <gtest/gtest.h>

#include <cstdint>
#include <thread>

#include "fh/moldudp64.hpp"
#include "fh/spsc_ring.hpp"

using namespace fh;

TEST(SpscRing, FullAndEmpty) {
    SpscRing<uint64_t, 4> r;
    uint64_t v;
    EXPECT_FALSE(r.try_pop(v));
    for (uint64_t i = 0; i < 4; ++i) EXPECT_TRUE(r.try_push(i));
    EXPECT_FALSE(r.try_push(99));
    EXPECT_TRUE(r.try_pop(v));
    EXPECT_EQ(v, 0u);
    EXPECT_TRUE(r.try_push(4));
}

TEST(SpscRing, TwoThreadsPreserveOrderAndLoseNothing) {
    constexpr uint64_t kN = 5'000'000;
    SpscRing<uint64_t, 1024> r;
    std::thread producer([&] {
        for (uint64_t i = 1; i <= kN; ++i)
            while (!r.try_push(i)) {}
    });
    uint64_t expected = 1, sum = 0, v;
    while (expected <= kN) {
        if (r.try_pop(v)) {
            ASSERT_EQ(v, expected);
            sum += v;
            ++expected;
        }
    }
    producer.join();
    EXPECT_EQ(sum, kN * (kN + 1) / 2);
}

TEST(Mold, TrackerDetectsGapsAndDuplicates) {
    mold::SequenceTracker t(1);
    EXPECT_EQ(t.on_packet(1, 10), mold::SequenceTracker::Result::InOrder);   // 1..10
    EXPECT_EQ(t.on_packet(11, 5), mold::SequenceTracker::Result::InOrder);   // 11..15
    EXPECT_EQ(t.on_packet(21, 5), mold::SequenceTracker::Result::Gap);       // 16..20 missing
    EXPECT_EQ(t.on_packet(11, 5), mold::SequenceTracker::Result::Duplicate); // replayed
    ASSERT_EQ(t.gaps().size(), 1u);
    EXPECT_EQ(t.gaps()[0].first_missing, 16u);
    EXPECT_EQ(t.gaps()[0].last_missing, 20u);
    EXPECT_EQ(t.missing_messages(), 5u);
    EXPECT_EQ(t.expected(), 26u);
}

TEST(Mold, BuildAndParsePacket) {
    const char session[11] = "TESTSESS01";
    mold::PacketBuilder b(session, 100);
    const uint8_t m1[3] = {'X', 1, 2}, m2[2] = {'Y', 3};
    ASSERT_TRUE(b.add(m1, 3));
    ASSERT_TRUE(b.add(m2, 2));
    auto pkt = b.flush();
    mold::Header h;
    ASSERT_TRUE(mold::parse_header(pkt.data(), pkt.size(), h));
    EXPECT_EQ(h.sequence, 100u);
    EXPECT_EQ(h.count, 2u);
    int n = 0;
    EXPECT_TRUE(mold::for_each_message(pkt.data(), pkt.size(), [&](const uint8_t* m, uint16_t len) {
        EXPECT_EQ(len, n == 0 ? 3 : 2);
        EXPECT_EQ(m[0], n == 0 ? 'X' : 'Y');
        ++n;
    }));
    EXPECT_EQ(n, 2);
    auto pkt2 = b.flush();  // next packet continues the sequence
    ASSERT_TRUE(mold::parse_header(pkt2.data(), pkt2.size(), h));
    EXPECT_EQ(h.sequence, 102u);
}

TEST(Mold, RejectsTruncatedPacket) {
    const char session[11] = "TESTSESS01";
    mold::PacketBuilder b(session, 1);
    const uint8_t m[4] = {'A', 0, 0, 0};
    b.add(m, 4);
    auto pkt = b.flush();
    pkt.pop_back();
    EXPECT_FALSE(mold::for_each_message(pkt.data(), pkt.size(), [](const uint8_t*, uint16_t) {}));
}

#include <random>
#include <unordered_map>

#include "fh/flat_hash_map.hpp"

// throw random inserts/erases/finds at both FlatHashMap and unordered_map and
// make sure they always agree. lots of erases to hit the backward-shift code,
// and it starts tiny so grow() gets hit too
TEST(FlatHashMap, MatchesUnorderedMap) {
    fh::FlatHashMap<uint64_t> flat(64);
    std::unordered_map<uint64_t, uint64_t> ref;
    std::mt19937_64 rng(1);
    for (int i = 0; i < 400'000; ++i) {
        const uint64_t k = rng() % 5000;
        switch (rng() % 3) {
            case 0: flat.insert(k, uint64_t(i)); ref[k] = uint64_t(i); break;
            case 1: EXPECT_EQ(flat.erase(k), ref.erase(k) == 1); break;
            case 2: {
                auto* v = flat.find(k);
                auto it = ref.find(k);
                ASSERT_EQ(v != nullptr, it != ref.end());
                if (v) {
                    EXPECT_EQ(*v, it->second);
                }
            }
        }
    }
    EXPECT_EQ(flat.size(), ref.size());
}
