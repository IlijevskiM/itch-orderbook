#pragma once
// latency histogram: 1ns buckets up to 100us, anything above goes in overflow.
// recording = one array increment so it's fine to call on the hot path
#include <chrono>
#include <cstdint>
#include <vector>

namespace fh {

inline uint64_t now_ns() noexcept {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

class LatencyHistogram {
public:
    static constexpr uint64_t kMaxNs = 100'000;

    LatencyHistogram() : buckets_(kMaxNs, 0) {}

    void record(uint64_t ns) noexcept {
        ++count_;
        if (ns < kMaxNs) ++buckets_[ns];
        else ++overflow_;
        if (ns > max_) max_ = ns;
    }

    // p in [0, 100]. if it lands in the overflow you just get kMaxNs back
    // (read that as ">= 100us", not an exact number)
    uint64_t percentile(double p) const noexcept {
        if (count_ == 0) return 0;
        const uint64_t target = uint64_t(double(count_) * p / 100.0);
        uint64_t seen = 0;
        for (uint64_t ns = 0; ns < kMaxNs; ++ns) {
            seen += buckets_[ns];
            if (seen > target) return ns;
        }
        return kMaxNs;
    }

    uint64_t count() const noexcept { return count_; }
    uint64_t max() const noexcept { return max_; }
    uint64_t overflow() const noexcept { return overflow_; }

private:
    std::vector<uint64_t> buckets_;
    uint64_t count_ = 0;
    uint64_t overflow_ = 0;
    uint64_t max_ = 0;
};

}  // namespace fh
