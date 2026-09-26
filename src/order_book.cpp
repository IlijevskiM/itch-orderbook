#include "fh/order_book.hpp"

#include <algorithm>

namespace fh {

template <typename Map>
void Book::add_to(Map& m, Order* o) {
    auto [it, inserted] = m.try_emplace(o->price);
    if (inserted) it->second.price = o->price;
    it->second.push_back(o);
}

template <typename Map>
void Book::remove_from(Map& m, Order* o) {
    Level* lvl = o->level;
    const uint32_t price = lvl->price;
    lvl->unlink(o);
    if (lvl->order_count == 0) m.erase(price);
}

void Book::add(Order* o) {
    if (o->side == 'B') add_to(bids_, o); else add_to(asks_, o);
}

void Book::remove(Order* o) {
    if (o->side == 'B') remove_from(bids_, o); else remove_from(asks_, o);
}

void Book::reduce(Order* o, uint32_t shares) {
    const uint32_t cut = std::min(shares, o->shares);
    o->shares -= cut;
    o->level->total_shares -= cut;
}

std::optional<Quote> Book::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    const Level& l = bids_.begin()->second;
    return Quote{l.price, l.total_shares, l.order_count};
}

std::optional<Quote> Book::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    const Level& l = asks_.begin()->second;
    return Quote{l.price, l.total_shares, l.order_count};
}

std::vector<Quote> Book::depth(char side, size_t n) const {
    std::vector<Quote> out;
    auto collect = [&](const auto& m) {
        for (auto it = m.begin(); it != m.end() && out.size() < n; ++it)
            out.push_back({it->second.price, it->second.total_shares, it->second.order_count});
    };
    if (side == 'B') collect(bids_); else collect(asks_);
    return out;
}

OrderBookManager::OrderBookManager(size_t expected_orders) : orders_(expected_orders) {
    books_.resize(1 << 16);  // stock locate is a uint16
}

Book& OrderBookManager::book_for(uint16_t locate) {
    auto& b = books_[locate];
    if (!b) b = std::make_unique<Book>();
    return *b;
}

void OrderBookManager::remove_order(Order* o) {
    book_for(o->locate).remove(o);
    orders_.erase(o->ref);
    pool_.destroy(o);
}

void OrderBookManager::apply(const Event& ev) {
    switch (ev.type) {
        case EventType::Add: {
            Order* o = pool_.create();
            o->ref = ev.order_ref;
            o->price = ev.price;
            o->shares = ev.shares;
            o->locate = ev.locate;
            o->side = ev.side;
            orders_.insert(ev.order_ref, o);
            book_for(ev.locate).add(o);
            ++stats_.adds;
            break;
        }
        case EventType::Execute:
        case EventType::Cancel: {
            Order** found = orders_.find(ev.order_ref);
            if (!found) { ++stats_.unknown_refs; break; }
            Order* o = *found;
            Book& b = book_for(o->locate);
            if (ev.type == EventType::Execute) {
                b.traded_volume += ev.shares;
                ++stats_.executes;
            } else {
                ++stats_.cancels;
            }
            if (ev.shares >= o->shares) remove_order(o);
            else b.reduce(o, ev.shares);
            break;
        }
        case EventType::Delete: {
            Order** found = orders_.find(ev.order_ref);
            if (!found) { ++stats_.unknown_refs; break; }
            remove_order(*found);
            ++stats_.deletes;
            break;
        }
        case EventType::Replace: {
            // replace = delete the old one + add a new one. same side/symbol,
            // but it goes to the back of the line (loses time priority)
            Order** found = orders_.find(ev.order_ref);
            if (!found) { ++stats_.unknown_refs; break; }
            Order* old = *found;
            const char side = old->side;
            const uint16_t locate = old->locate;
            remove_order(old);
            Order* o = pool_.create();
            o->ref = ev.new_order_ref;
            o->price = ev.price;
            o->shares = ev.shares;
            o->locate = locate;
            o->side = side;
            orders_.insert(o->ref, o);
            book_for(locate).add(o);
            ++stats_.replaces;
            break;
        }
        case EventType::StockDirectory: {
            std::string sym(ev.symbol, 8);
            sym.erase(sym.find_last_not_of(' ') + 1);
            symbols_[ev.locate] = std::move(sym);
            break;
        }
        case EventType::SystemEvent:
        case EventType::None:
            break;
    }
}

std::optional<uint16_t> OrderBookManager::locate_of(const std::string& symbol) const {
    for (const auto& [loc, sym] : symbols_)
        if (sym == symbol) return loc;
    return std::nullopt;
}

std::string OrderBookManager::symbol_of(uint16_t locate) const {
    auto it = symbols_.find(locate);
    return it == symbols_.end() ? std::string("#") + std::to_string(locate) : it->second;
}

}  // namespace fh
