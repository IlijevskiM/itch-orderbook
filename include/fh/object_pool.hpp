#pragma once
// simple object pool w/ an intrusive free list.
//
// orders get created/destroyed millions of times a day, and malloc/free for
// each one is slow + fragments the heap. so: grab big chunks, hand out slots,
// put freed slots back on a free list. chunks never move, so Order* pointers
// stay valid as long as the pool is alive.
#include <cstddef>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace fh {

template <typename T, size_t ChunkSize = 1 << 16>
class ObjectPool {
    union Node {
        Node* next_free;
        alignas(T) unsigned char storage[sizeof(T)];
    };

public:
    ObjectPool() = default;
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    template <typename... Args>
    T* create(Args&&... args) {
        if (!free_) grow();
        Node* n = free_;
        free_ = n->next_free;
        ++live_;
        return ::new (static_cast<void*>(n->storage)) T{std::forward<Args>(args)...};
    }

    void destroy(T* obj) noexcept {
        obj->~T();
        Node* n = reinterpret_cast<Node*>(obj);
        n->next_free = free_;
        free_ = n;
        --live_;
    }

    // grow the pool before we start so the hot path never has to. grow()
    // writes every node while linking the free list, so this also faults the
    // pages in now instead of on the book thread (waitlens caught that)
    void reserve(size_t n) {
        while (capacity() < n) grow();
    }

    size_t live() const noexcept { return live_; }
    size_t capacity() const noexcept { return chunks_.size() * ChunkSize; }

private:
    void grow() {
        auto chunk = std::make_unique<Node[]>(ChunkSize);
        for (size_t i = 0; i + 1 < ChunkSize; ++i) chunk[i].next_free = &chunk[i + 1];
        chunk[ChunkSize - 1].next_free = free_;
        free_ = &chunk[0];
        chunks_.push_back(std::move(chunk));
    }

    std::vector<std::unique_ptr<Node[]>> chunks_;
    Node* free_ = nullptr;
    size_t live_ = 0;
};

}  // namespace fh
