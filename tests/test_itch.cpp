#include <gtest/gtest.h>

#include <vector>

#include "fh/itch.hpp"
#include "fh/synthetic.hpp"

using namespace fh;

namespace {
std::vector<Event> decode_all(const std::vector<uint8_t>& buf, StreamStats* st = nullptr) {
    std::vector<Event> out;
    auto s = for_each_event(buf.data(), buf.size(), [&](const Event& e) { out.push_back(e); });
    if (st) *st = s;
    return out;
}
}  // namespace

TEST(Endian, RoundTrip48) {
    uint8_t b[6];
    write_be48(b, 0x0000123456789ABCULL);
    EXPECT_EQ(read_be48(b), 0x0000123456789ABCULL);
    EXPECT_EQ(b[0], 0x12);
}

TEST(Itch, DecodesAddOrder) {
    ItchWriter w;
    w.add(7, 34'200'000'000'123ULL, 42, 'B', 300, "AAPL", 1'893'400);
    auto ev = decode_all(w.buf);
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, EventType::Add);
    EXPECT_EQ(ev[0].locate, 7);
    EXPECT_EQ(ev[0].timestamp_ns, 34'200'000'000'123ULL);
    EXPECT_EQ(ev[0].order_ref, 42u);
    EXPECT_EQ(ev[0].side, 'B');
    EXPECT_EQ(ev[0].shares, 300u);
    EXPECT_EQ(ev[0].price, 1'893'400u);  // $189.34
}

TEST(Itch, DecodesEveryBookMessageType) {
    ItchWriter w;
    w.stock_directory(1, "MSFT", 1);
    w.add(1, 2, 10, 'S', 500, "MSFT", 4'000'000);
    w.execute(1, 3, 10, 100, 9);
    w.cancel(1, 4, 10, 100);
    w.replace(1, 5, 10, 11, 200, 4'000'100);
    w.del(1, 6, 11);
    auto ev = decode_all(w.buf);
    ASSERT_EQ(ev.size(), 6u);
    EXPECT_EQ(ev[0].type, EventType::StockDirectory);
    EXPECT_EQ(std::string(ev[0].symbol, 4), "MSFT");
    EXPECT_EQ(ev[2].type, EventType::Execute);
    EXPECT_EQ(ev[2].shares, 100u);
    EXPECT_EQ(ev[3].type, EventType::Cancel);
    EXPECT_EQ(ev[4].type, EventType::Replace);
    EXPECT_EQ(ev[4].new_order_ref, 11u);
    EXPECT_EQ(ev[4].price, 4'000'100u);
    EXPECT_EQ(ev[5].type, EventType::Delete);
}

TEST(Itch, SkipsNonBookMessages) {
    ItchWriter w;
    w.trade(1, 1);
    w.add(1, 2, 1, 'B', 100, "X", 10'000);
    StreamStats st;
    auto ev = decode_all(w.buf, &st);
    EXPECT_EQ(st.messages, 2u);
    EXPECT_EQ(st.book_events, 1u);
    EXPECT_EQ(ev.size(), 1u);
}

TEST(Itch, RejectsBadLengthAndUnknownType) {
    std::vector<uint8_t> buf = {0, 3, 'A', 0, 0,   // 'A' with length 3: bad length
                                0, 1, '?'};        // unknown type
    StreamStats st;
    decode_all(buf, &st);
    EXPECT_EQ(st.bad_length, 1u);
    EXPECT_EQ(st.unknown_type, 1u);
    EXPECT_EQ(st.book_events, 0u);
}

TEST(Itch, StopsCleanlyOnTruncatedTail) {
    ItchWriter w;
    w.add(1, 1, 1, 'B', 100, "X", 10'000);
    w.buf.resize(w.buf.size() - 5);
    StreamStats st;
    auto ev = decode_all(w.buf, &st);
    EXPECT_TRUE(ev.empty());
    EXPECT_GT(st.truncated_bytes, 0u);
}
