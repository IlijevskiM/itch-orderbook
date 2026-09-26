#pragma once
// lock-free single producer / single consumer ring buffer (bounded)
//
// how it works:
// - only the producer ever writes tail_, only the consumer writes head_.
//   one writer per index means no CAS needed, just acquire/release:
//     producer: write the slot, THEN tail_.store(release)
//     consumer: tail_.load(acquire) -> guaranteed to see that slot's data
//   (same thing in reverse for head_ when a slot gets freed)
// - head_ and tail_ are on different cache lines. if they shared one, every
//   push would bounce the line to the other core (false sharing)
// - each side caches the other side's index and only re-reads the real atomic
//   when the ring *looks* full/empty. way less cross-core traffic
// - slots are cache-line aligned too so writing slot i and reading slot i-1
//   never fight over a line
// - capacity is a power of 2 so wrapping is & mask instead of %
#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>

namespace fh {

// Apple M chips use 128 byte cache lines, x86 (and most other ARM) use 64
#if defined(__APPLE__) && defined(__aarch64__)
inline constexpr size_t kCacheLine = 128;
#else
inline constexpr size_t kCacheLine = 64;
#endif

template <typename T, size_t Capacity>
class SpscRing {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");

    struct alignas(kCacheLine) Slot {
        T value;
    };

public:
    SpscRing() : slots_(new Slot[Capacity]) {}
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    // producer thread only
    bool try_push(const T& v) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail - cached_head_ == Capacity) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (tail - cached_head_ == Capacity) return false;  // full
        }
        slots_[tail & kMask].value = v;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // consumer thread only
    bool try_pop(T& out) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head == cached_tail_) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (head == cached_tail_) return false;  // empty
        }
        out = slots_[head & kMask].value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // not exact while both sides are running, fine for stats
    size_t size() const noexcept {
        return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
    }
    static constexpr size_t capacity() noexcept { return Capacity; }

private:
    static constexpr size_t kMask = Capacity - 1;

    // consumer's cache line
    alignas(kCacheLine) std::atomic<size_t> head_{0};
    size_t cached_tail_ = 0;
    // producer's cache line
    alignas(kCacheLine) std::atomic<size_t> tail_{0};
    size_t cached_head_ = 0;

    alignas(kCacheLine) std::unique_ptr<Slot[]> slots_;
};

}  // namespace fh
