#pragma once
// NASDAQ TotalView-ITCH 5.0 decoder
//
// zero-copy: fields get read straight out of whatever buffer we're handed (the
// mmap'd file, or a packet). the only copy is the few fields the book actually
// needs, which go into Event.
//
// spec: https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf
#include <array>
#include <cstddef>
#include <cstdint>

#include "fh/endian.hpp"

namespace fh {

enum class EventType : uint8_t {
    None = 0,
    SystemEvent,     // 'S'
    StockDirectory,  // 'R'
    Add,             // 'A' and 'F'
    Execute,         // 'E' and 'C'
    Cancel,          // 'X' (partial cancel)
    Delete,          // 'D'
    Replace,         // 'U'
};

// what the decode thread hands to the book thread.
// kept at 56 bytes on purpose so it fits in one cache line (one ring slot)
struct Event {
    uint64_t timestamp_ns = 0;   // ns since midnight (exchange time)
    uint64_t order_ref = 0;
    uint64_t new_order_ref = 0;  // Replace only
    uint64_t enqueue_ns = 0;     // stamped by the pipeline, only for latency numbers
    uint32_t shares = 0;
    uint32_t price = 0;          // 4 implied decimals, so $1.2345 -> 12345
    uint16_t locate = 0;         // NASDAQ's per-day id for the symbol
    EventType type = EventType::None;
    char side = 0;               // 'B' or 'S' (Add only)
    char symbol[8] = {};         // StockDirectory only, space padded
};
static_assert(sizeof(Event) <= 64, "Event must fit in one cache line");

// expected length per message type from the spec (0 = type we don't know)
constexpr std::array<uint16_t, 256> make_length_table() {
    std::array<uint16_t, 256> t{};
    t['S'] = 12; t['R'] = 39; t['H'] = 25; t['Y'] = 20; t['L'] = 26;
    t['V'] = 35; t['W'] = 12; t['K'] = 28; t['J'] = 35; t['h'] = 21;
    t['A'] = 36; t['F'] = 40; t['E'] = 31; t['C'] = 36; t['X'] = 23;
    t['D'] = 19; t['U'] = 35; t['P'] = 44; t['Q'] = 40; t['B'] = 19;
    t['I'] = 50; t['N'] = 20; t['O'] = 48;
    return t;
}
inline constexpr std::array<uint16_t, 256> kMessageLength = make_length_table();

enum class DecodeResult : uint8_t {
    Ok,          // event filled, relevant to the book
    Ignored,     // valid, but the book doesn't care (trades, imbalances, etc)
    BadLength,   // length prefix disagrees with the spec
    UnknownType,
};

// decode one message. msg points past the 2-byte length prefix
inline DecodeResult decode(const uint8_t* msg, uint16_t len, Event& ev) noexcept {
    const uint8_t type = msg[0];
    const uint16_t expected = kMessageLength[type];
    if (expected == 0) return DecodeResult::UnknownType;
    if (expected != len) return DecodeResult::BadLength;

    // every message starts with: type(1) locate(2) tracking(2) timestamp(6)
    ev.locate = read_be16(msg + 1);
    ev.timestamp_ns = read_be48(msg + 5);

    switch (type) {
        case 'S':
            ev.type = EventType::SystemEvent;
            ev.side = char(msg[11]);  // event code: O, S, Q, M, E, C
            return DecodeResult::Ok;
        case 'R':
            ev.type = EventType::StockDirectory;
            for (int i = 0; i < 8; ++i) ev.symbol[i] = char(msg[11 + i]);
            return DecodeResult::Ok;
        case 'A':
        case 'F':  // same as A + an MPID we don't need
            ev.type = EventType::Add;
            ev.order_ref = read_be64(msg + 11);
            ev.side = char(msg[19]);
            ev.shares = read_be32(msg + 20);
            ev.price = read_be32(msg + 32);
            return DecodeResult::Ok;
        case 'E':
            ev.type = EventType::Execute;
            ev.order_ref = read_be64(msg + 11);
            ev.shares = read_be32(msg + 19);
            ev.price = 0;  // E doesn't carry a price, it fills at the resting order's
            return DecodeResult::Ok;
        case 'C':
            ev.type = EventType::Execute;
            ev.order_ref = read_be64(msg + 11);
            ev.shares = read_be32(msg + 19);
            ev.price = read_be32(msg + 32);
            return DecodeResult::Ok;
        case 'X':
            ev.type = EventType::Cancel;
            ev.order_ref = read_be64(msg + 11);
            ev.shares = read_be32(msg + 19);
            return DecodeResult::Ok;
        case 'D':
            ev.type = EventType::Delete;
            ev.order_ref = read_be64(msg + 11);
            return DecodeResult::Ok;
        case 'U':
            ev.type = EventType::Replace;
            ev.order_ref = read_be64(msg + 11);
            ev.new_order_ref = read_be64(msg + 19);
            ev.shares = read_be32(msg + 27);
            ev.price = read_be32(msg + 31);
            return DecodeResult::Ok;
        default:
            return DecodeResult::Ignored;
    }
}

struct StreamStats {
    uint64_t messages = 0;
    uint64_t book_events = 0;
    uint64_t bad_length = 0;
    uint64_t unknown_type = 0;
    uint64_t truncated_bytes = 0;
};

// walk a buffer of [len][msg][len][msg]... (how NASDAQ's historical files are
// laid out) and call on_event for everything the book cares about
template <typename OnEvent>
StreamStats for_each_event(const uint8_t* data, size_t size, OnEvent&& on_event) {
    StreamStats st;
    size_t pos = 0;
    Event ev;
    while (pos + 2 <= size) {
        const uint16_t len = read_be16(data + pos);
        if (pos + 2 + len > size) {
            st.truncated_bytes = size - pos;
            break;
        }
        const uint8_t* msg = data + pos + 2;
        ++st.messages;
        switch (decode(msg, len, ev)) {
            case DecodeResult::Ok:
                ++st.book_events;
                on_event(ev);
                break;
            case DecodeResult::BadLength: ++st.bad_length; break;
            case DecodeResult::UnknownType: ++st.unknown_type; break;
            case DecodeResult::Ignored: break;
        }
        pos += 2 + size_t(len);
    }
    return st;
}

}  // namespace fh
