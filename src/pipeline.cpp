#include "fh/pipeline.hpp"

#include <atomic>
#include <thread>

#include "fh/spsc_ring.hpp"

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace fh {
namespace {

void pin_to_cpu([[maybe_unused]] int cpu) {
#if defined(__linux__)
    if (cpu < 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#endif
}

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}

}  // namespace

RunResult run_single_thread(const uint8_t* data, size_t size, OrderBookManager& books) {
    RunResult r;
    const uint64_t t0 = now_ns();
    r.stream = for_each_event(data, size, [&](const Event& ev) {
        books.apply(ev);
        ++r.events_applied;
    });
    r.seconds = double(now_ns() - t0) / 1e9;
    return r;
}

RunResult run_pipeline(const uint8_t* data, size_t size, OrderBookManager& books,
                       const PipelineOptions& opts) {
    // 64K slots * 64B = 4MB. big enough to soak up bursts, small enough to
    // mostly stay in cache
    static constexpr size_t kSlots = 1 << 16;
    auto ring = std::make_unique<SpscRing<Event, kSlots>>();
    std::atomic<bool> producer_done{false};

    RunResult r;
    const uint64_t t0 = now_ns();

    std::thread producer([&] {
        pin_to_cpu(opts.producer_cpu);
        const double ns_per_event = opts.target_rate > 0 ? 1e9 / opts.target_rate : 0;
        uint64_t i = 0;
        const uint64_t start = now_ns();
        r.stream = for_each_event(data, size, [&](const Event& decoded) {
            Event ev = decoded;
            if (ns_per_event > 0) {
                const uint64_t due = start + uint64_t(double(i) * ns_per_event);
                while (now_ns() < due) cpu_relax();
            }
            ++i;
            if (opts.measure_latency) ev.enqueue_ns = now_ns();
            while (!ring->try_push(ev)) cpu_relax();
        });
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        pin_to_cpu(opts.consumer_cpu);
        Event ev;
        for (;;) {
            if (ring->try_pop(ev)) {
                books.apply(ev);
                ++r.events_applied;
                if (opts.measure_latency) r.latency.record(now_ns() - ev.enqueue_ns);
            } else if (producer_done.load(std::memory_order_acquire)) {
                // producer might have pushed more between our failed pop and
                // seeing the flag, so drain whatever's left
                while (ring->try_pop(ev)) {
                    books.apply(ev);
                    ++r.events_applied;
                    if (opts.measure_latency) r.latency.record(now_ns() - ev.enqueue_ns);
                }
                break;
            } else {
                cpu_relax();
            }
        }
    });

    producer.join();
    consumer.join();
    r.seconds = double(now_ns() - t0) / 1e9;
    return r;
}

}  // namespace fh
