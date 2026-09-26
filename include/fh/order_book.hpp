#pragma once
// per-symbol limit order books, rebuilt from ITCH events.
//
// notes on the data structures:
//
// - each price level holds its orders in an intrusive doubly linked list
//   (prev/next live inside Order). since we already have the Order* from the
//   hash lookup, removing one is O(1), no search, no allocation. list order is
//   time priority for free.
//
// - order ref -> Order* lookup goes through FlatHashMap (open addressing).
//   E/X/D/U messages only give you the ref, so this is the hot lookup. see
//   flat_hash_map.hpp for why not std::unordered_map
//
// - levels are in a std::map sorted best-first, so best bid/ask = begin().
//   adding a level is O(log L) but L (distinct prices) is small. map nodes
//   don't move, so holding a raw Level* in Order is safe until it's erased.
//   TODO maybe: flat array of levels indexed by ticks from the best price,
//   should be more cache friendly for dense books
//
// - Orders come from ObjectPool, and the map nodes come from a pmr pool over
//   an arena we fault in at startup -> no malloc and no page faults on the
//   hot path (waitlens is how I found those, see README)
#include <cstdint>
#include <functional>
#include <map>
#include <memory_resource>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "fh/flat_hash_map.hpp"
#include "fh/itch.hpp"
#include "fh/object_pool.hpp"

namespace fh {

struct Level;

struct Order {
    uint64_t ref = 0;
    uint32_t price = 0;
    uint32_t shares = 0;
    uint16_t locate = 0;
    char side = 0;
    Order* prev = nullptr;
    Order* next = nullptr;
    Level* level = nullptr;
};

struct Level {
    uint32_t price = 0;
    uint64_t total_shares = 0;
    uint32_t order_count = 0;
    Order* head = nullptr;  // oldest = first in line
    Order* tail = nullptr;

    void push_back(Order* o) noexcept {
        o->prev = tail;
        o->next = nullptr;
        if (tail) tail->next = o; else head = o;
        tail = o;
        o->level = this;
        total_shares += o->shares;
        ++order_count;
    }

    void unlink(Order* o) noexcept {
        if (o->prev) o->prev->next = o->next; else head = o->next;
        if (o->next) o->next->prev = o->prev; else tail = o->prev;
        total_shares -= o->shares;
        --order_count;
        o->prev = o->next = nullptr;
        o->level = nullptr;
    }
};

struct Quote {
    uint32_t price = 0;
    uint64_t shares = 0;
    uint32_t orders = 0;
};

class Book {
public:
    explicit Book(std::pmr::memory_resource* mr = std::pmr::get_default_resource())
        : bids_(mr), asks_(mr) {}

    void add(Order* o);
    void remove(Order* o);                    // just unlinks, caller frees it
    void reduce(Order* o, uint32_t shares);  // partial execute / cancel

    std::optional<Quote> best_bid() const;
    std::optional<Quote> best_ask() const;
    size_t bid_levels() const { return bids_.size(); }
    size_t ask_levels() const { return asks_.size(); }

    // top n levels on one side, best first
    std::vector<Quote> depth(char side, size_t n) const;

    uint64_t traded_volume = 0;

private:
    template <typename Map>
    void add_to(Map& m, Order* o);
    template <typename Map>
    void remove_from(Map& m, Order* o);

    std::pmr::map<uint32_t, Level, std::greater<uint32_t>> bids_;  // highest first
    std::pmr::map<uint32_t, Level, std::less<uint32_t>> asks_;     // lowest first
};

struct BookStats {
    uint64_t adds = 0;
    uint64_t executes = 0;
    uint64_t cancels = 0;
    uint64_t deletes = 0;
    uint64_t replaces = 0;
    uint64_t unknown_refs = 0;  // should stay 0 if you replay a full day from the start
};

class OrderBookManager {
public:
    explicit OrderBookManager(size_t expected_orders = 1 << 20);

    void apply(const Event& ev);

    const Book* book(uint16_t locate) const {
        return locate < books_.size() ? books_[locate].get() : nullptr;
    }
    std::optional<uint16_t> locate_of(const std::string& symbol) const;
    std::string symbol_of(uint16_t locate) const;

    size_t live_orders() const { return orders_.size(); }
    const BookStats& stats() const { return stats_; }
    size_t symbol_count() const { return symbols_.size(); }

private:
    Book& book_for(uint16_t locate);
    void remove_order(Order* o);

    // NOTE: has to be declared before books_ so it gets destroyed *after*
    // them (the maps free into it on destruction -- got a segfault before).
    // the arena is allocated + touched once at startup, then the pool just
    // recycles nodes as levels come and go
    std::vector<std::byte> level_arena_;
    std::pmr::monotonic_buffer_resource level_upstream_;
    std::pmr::unsynchronized_pool_resource level_pool_;
    std::vector<std::unique_ptr<Book>> books_;  // indexed by stock locate
#ifdef FH_STD_INDEX
    StdHashMap<Order*> orders_;
#else
    FlatHashMap<Order*> orders_;
#endif
    std::unordered_map<uint16_t, std::string> symbols_;
    ObjectPool<Order> pool_;
    BookStats stats_;
};

}  // namespace fh
