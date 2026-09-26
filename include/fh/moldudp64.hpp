#pragma once
// MoldUDP64 = how NASDAQ actually sends ITCH over UDP multicast in prod.
//
// packet:
//   session (10 bytes) | seq num (8, BE) | msg count (2, BE)
//   then count x [len (2, BE) | ITCH message]
//
// seq is the number of the *first* message in the packet, so next expected is
// seq + count. UDP can drop/reorder, so if there's a gap the book can't be
// trusted anymore -- a real handler would ask the rewind server to resend.
// here we just detect + count gaps.
#include <cstdint>
#include <cstring>
#include <vector>

#include "fh/endian.hpp"

namespace fh::mold {

inline constexpr size_t kHeaderSize = 20;
inline constexpr uint16_t kHeartbeat = 0;       // count == 0
inline constexpr uint16_t kEndOfSession = 0xFFFF;

struct Header {
    char session[10];
    uint64_t sequence;
    uint16_t count;
};

inline bool parse_header(const uint8_t* pkt, size_t len, Header& h) noexcept {
    if (len < kHeaderSize) return false;
    std::memcpy(h.session, pkt, 10);
    h.sequence = read_be64(pkt + 10);
    h.count = read_be16(pkt + 18);
    return true;
}

// calls on_msg(msg, len) for each message in the packet. false = malformed
template <typename OnMsg>
bool for_each_message(const uint8_t* pkt, size_t len, OnMsg&& on_msg) {
    Header h;
    if (!parse_header(pkt, len, h)) return false;
    if (h.count == kEndOfSession) return true;
    size_t pos = kHeaderSize;
    for (uint16_t i = 0; i < h.count; ++i) {
        if (pos + 2 > len) return false;
        const uint16_t mlen = read_be16(pkt + pos);
        if (pos + 2 + mlen > len) return false;
        on_msg(pkt + pos + 2, mlen);
        pos += 2 + size_t(mlen);
    }
    return true;
}

// keeps track of the next seq we expect and remembers any gaps
class SequenceTracker {
public:
    enum class Result { InOrder, Gap, Duplicate };
    struct Gap {
        uint64_t first_missing;
        uint64_t last_missing;
    };

    explicit SequenceTracker(uint64_t first_expected = 1) : expected_(first_expected) {}

    // InOrder or Gap -> still process the packet (after a Gap you'd also want
    // to request a retransmit). Duplicate -> already seen, skip it
    Result on_packet(uint64_t seq, uint16_t count) {
        if (count == kEndOfSession) count = 0;
        if (seq + count <= expected_) {
            ++duplicates_;
            return Result::Duplicate;
        }
        Result r = Result::InOrder;
        if (seq > expected_) {
            gaps_.push_back({expected_, seq - 1});
            missing_ += seq - expected_;
            r = Result::Gap;
        }
        expected_ = seq + count;
        return r;
    }

    uint64_t expected() const { return expected_; }
    const std::vector<Gap>& gaps() const { return gaps_; }
    uint64_t missing_messages() const { return missing_; }
    uint64_t duplicates() const { return duplicates_; }

private:
    uint64_t expected_;
    uint64_t missing_ = 0;
    uint64_t duplicates_ = 0;
    std::vector<Gap> gaps_;
};

// builds MoldUDP64 packets. only used by the tests and the --mold-drop
// simulation in replay, not needed for reading files
class PacketBuilder {
public:
    PacketBuilder(const char (&session)[11], uint64_t first_seq, size_t max_payload = 1400)
        : next_seq_(first_seq), max_payload_(max_payload) {
        std::memcpy(session_, session, 10);
        reset();
    }

    // false if it won't fit -> flush() and try again
    bool add(const uint8_t* msg, uint16_t len) {
        if (buf_.size() + 2 + len > max_payload_ && count_ > 0) return false;
        const size_t pos = buf_.size();
        buf_.resize(pos + 2 + len);
        write_be16(buf_.data() + pos, len);
        std::memcpy(buf_.data() + pos + 2, msg, len);
        ++count_;
        return true;
    }

    bool empty() const { return count_ == 0; }

    // stamps seq/count into the header, hands back the packet, starts a new one
    std::vector<uint8_t> flush() {
        write_be64(buf_.data() + 10, next_seq_);
        write_be16(buf_.data() + 18, count_);
        next_seq_ += count_;
        std::vector<uint8_t> out = std::move(buf_);
        reset();
        return out;
    }

private:
    void reset() {
        buf_.assign(kHeaderSize, 0);
        std::memcpy(buf_.data(), session_, 10);
        count_ = 0;
    }

    char session_[10];
    uint64_t next_seq_;
    size_t max_payload_;
    uint16_t count_ = 0;
    std::vector<uint8_t> buf_;
};

}  // namespace fh::mold
