#include <gtest/gtest.h>

#include "fh/order_book.hpp"
#include "fh/pipeline.hpp"
#include "fh/synthetic.hpp"

using namespace fh;

namespace {
Event add(uint64_t ref, char side, uint32_t shares, uint32_t price, uint16_t loc = 1) {
    Event e;
    e.type = EventType::Add;
    e.order_ref = ref;
    e.side = side;
    e.shares = shares;
    e.price = price;
    e.locate = loc;
    return e;
}
Event simple(EventType t, uint64_t ref, uint32_t shares = 0) {
    Event e;
    e.type = t;
    e.order_ref = ref;
    e.shares = shares;
    return e;
}
}  // namespace

TEST(OrderBook, BestBidAskAndAggregation) {
    OrderBookManager m;
    m.apply(add(1, 'B', 100, 10'000));
    m.apply(add(2, 'B', 200, 10'100));
    m.apply(add(3, 'B', 300, 10'100));
    m.apply(add(4, 'S', 50, 10'300));
    m.apply(add(5, 'S', 70, 10'200));
    const Book* b = m.book(1);
    ASSERT_NE(b, nullptr);
    auto bid = b->best_bid();
    auto ask = b->best_ask();
    ASSERT_TRUE(bid && ask);
    EXPECT_EQ(bid->price, 10'100u);
    EXPECT_EQ(bid->shares, 500u);
    EXPECT_EQ(bid->orders, 2u);
    EXPECT_EQ(ask->price, 10'200u);
    EXPECT_EQ(b->bid_levels(), 2u);
}

TEST(OrderBook, PartialExecuteThenFullExecuteRemovesLevel) {
    OrderBookManager m;
    m.apply(add(1, 'S', 300, 20'000));
    m.apply(simple(EventType::Execute, 1, 100));
    EXPECT_EQ(m.book(1)->best_ask()->shares, 200u);
    EXPECT_EQ(m.book(1)->traded_volume, 100u);
    m.apply(simple(EventType::Execute, 1, 200));
    EXPECT_FALSE(m.book(1)->best_ask().has_value());
    EXPECT_EQ(m.live_orders(), 0u);
    EXPECT_EQ(m.book(1)->traded_volume, 300u);
}

TEST(OrderBook, CancelReducesAndDeleteRemoves) {
    OrderBookManager m;
    m.apply(add(1, 'B', 500, 10'000));
    m.apply(add(2, 'B', 100, 10'000));
    m.apply(simple(EventType::Cancel, 1, 200));
    EXPECT_EQ(m.book(1)->best_bid()->shares, 400u);
    m.apply(simple(EventType::Delete, 2));
    EXPECT_EQ(m.book(1)->best_bid()->shares, 300u);
    EXPECT_EQ(m.book(1)->best_bid()->orders, 1u);
}

TEST(OrderBook, ReplaceMovesPriceAndKeepsSide) {
    OrderBookManager m;
    m.apply(add(1, 'B', 100, 10'000));
    Event r;
    r.type = EventType::Replace;
    r.order_ref = 1;
    r.new_order_ref = 9;
    r.shares = 250;
    r.price = 10'500;
    m.apply(r);
    auto bid = m.book(1)->best_bid();
    ASSERT_TRUE(bid);
    EXPECT_EQ(bid->price, 10'500u);
    EXPECT_EQ(bid->shares, 250u);
    EXPECT_EQ(m.book(1)->bid_levels(), 1u);
    m.apply(simple(EventType::Delete, 9));  // the new ref has to be findable
    EXPECT_EQ(m.live_orders(), 0u);
}

TEST(OrderBook, TimePriorityWithinLevel) {
    OrderBookManager m;
    m.apply(add(1, 'S', 100, 10'000));
    m.apply(add(2, 'S', 100, 10'000));
    m.apply(add(3, 'S', 100, 10'000));
    m.apply(simple(EventType::Delete, 2));  // pull one out of the middle
    auto d = m.book(1)->depth('S', 1);
    ASSERT_EQ(d.size(), 1u);
    EXPECT_EQ(d[0].orders, 2u);
    EXPECT_EQ(d[0].shares, 200u);
}

TEST(OrderBook, UnknownReferenceIsCountedNotCrashed) {
    OrderBookManager m;
    m.apply(simple(EventType::Delete, 12345));
    m.apply(simple(EventType::Execute, 12345, 10));
    EXPECT_EQ(m.stats().unknown_refs, 2u);
}

TEST(OrderBook, SymbolsTrackedFromDirectory) {
    OrderBookManager m;
    Event e;
    e.type = EventType::StockDirectory;
    e.locate = 13;
    std::memcpy(e.symbol, "NVDA    ", 8);
    m.apply(e);
    EXPECT_EQ(m.locate_of("NVDA"), 13);
    EXPECT_EQ(m.symbol_of(13), "NVDA");
}

// pipeline has to end up with the exact same books as the single thread
// version, otherwise the threading is broken somewhere
TEST(Pipeline, MatchesSingleThreadBaseline) {
    auto data = generate_synthetic(300'000, 50, 7);
    OrderBookManager a, b;
    auto ra = run_single_thread(data.data(), data.size(), a);
    auto rb = run_pipeline(data.data(), data.size(), b);
    EXPECT_EQ(ra.events_applied, rb.events_applied);
    EXPECT_EQ(a.live_orders(), b.live_orders());
    EXPECT_EQ(a.stats().unknown_refs, 0u);
    EXPECT_EQ(b.stats().unknown_refs, 0u);
    for (uint16_t loc = 1; loc <= 50; ++loc) {
        auto ba = a.book(loc)->best_bid(), bb = b.book(loc)->best_bid();
        ASSERT_EQ(ba.has_value(), bb.has_value());
        if (ba) {
            EXPECT_EQ(ba->price, bb->price);
            EXPECT_EQ(ba->shares, bb->shares);
        }
        EXPECT_EQ(a.book(loc)->traded_volume, b.book(loc)->traded_volume);
    }
}
