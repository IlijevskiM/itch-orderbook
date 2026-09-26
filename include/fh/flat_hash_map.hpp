#pragma once
// open-addressing hash map, uint64 key -> value. built for the order ref index.
//
// why not std::unordered_map: it allocates a node per entry and every lookup
// chases bucket -> node, which is basically a cache miss per message. here
// keys/values sit in one flat array and a lookup just walks neighboring slots
// (linear probing). no allocation after startup unless it has to grow.
//
// deletes use backward-shift instead of tombstones. with tombstones the table
// slowly fills with junk over a full day of adds/deletes and lookups get slower
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace fh {

template <typename V>
class FlatHashMap {
    static constexpr uint64_t kEmpty = ~uint64_t(0);  // reserved key
    struct Slot {
        uint64_t key = kEmpty;
        V value{};
    };

public:
    explicit FlatHashMap(size_t expected = 1 << 20) {
        size_t cap = 16;
        while (cap < expected * 2) cap <<= 1;  // stay under 50% full
        slots_.resize(cap);
        mask_ = cap - 1;
    }

    // insert or overwrite
    void insert(uint64_t key, V value) {
        if ((size_ + 1) * 2 > slots_.size()) grow();
        size_t i = hash(key) & mask_;
        while (slots_[i].key != kEmpty && slots_[i].key != key) i = (i + 1) & mask_;
        if (slots_[i].key == kEmpty) ++size_;
        slots_[i] = {key, value};
    }

    // nullptr if it's not there
    V* find(uint64_t key) noexcept {
        size_t i = hash(key) & mask_;
        while (slots_[i].key != kEmpty) {
            if (slots_[i].key == key) return &slots_[i].value;
            i = (i + 1) & mask_;
        }
        return nullptr;
    }

    bool erase(uint64_t key) noexcept {
        size_t i = hash(key) & mask_;
        while (slots_[i].key != key) {
            if (slots_[i].key == kEmpty) return false;
            i = (i + 1) & mask_;
        }
        // backward shift: slide later entries in the same probe run back into
        // the hole, otherwise a lookup could hit the empty slot and stop early
        size_t hole = i;
        size_t j = (i + 1) & mask_;
        while (slots_[j].key != kEmpty) {
            const size_t home = hash(slots_[j].key) & mask_;
            // only move j if its home slot isn't in (hole, j] (wrapping around)
            const bool movable = ((j - home) & mask_) >= ((j - hole) & mask_);
            if (movable) {
                slots_[hole] = slots_[j];
                hole = j;
            }
            j = (j + 1) & mask_;
        }
        slots_[hole].key = kEmpty;
        --size_;
        return true;
    }

    size_t size() const noexcept { return size_; }

private:
    // splitmix64 mix. order refs are mostly sequential so they'd clump without it
    static uint64_t hash(uint64_t x) noexcept {
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return x;
    }

    void grow() {
        std::vector<Slot> old = std::move(slots_);
        slots_.assign(old.size() * 2, Slot{});
        mask_ = slots_.size() - 1;
        size_ = 0;
        for (const Slot& s : old)
            if (s.key != kEmpty) insert(s.key, s.value);
    }

    std::vector<Slot> slots_;
    size_t mask_ = 0;
    size_t size_ = 0;
};

// same interface on top of std::unordered_map, only here so I can A/B them:
// build with -DFH_STD_INDEX=ON and compare throughput
template <typename V>
class StdHashMap {
public:
    explicit StdHashMap(size_t expected = 1 << 20) { map_.reserve(expected); }
    void insert(uint64_t key, V value) { map_[key] = value; }
    V* find(uint64_t key) noexcept {
        auto it = map_.find(key);
        return it == map_.end() ? nullptr : &it->second;
    }
    bool erase(uint64_t key) noexcept { return map_.erase(key) == 1; }
    size_t size() const noexcept { return map_.size(); }

private:
    std::unordered_map<uint64_t, V> map_;
};

}  // namespace fh
