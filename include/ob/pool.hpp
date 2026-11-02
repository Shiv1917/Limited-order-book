// Order storage policies. Both hand out 32-bit indices rather than pointers.
//
//   PoolStorage<T> : one contiguous pre-allocated (and pre-faulted) array with
//                    an intrusive free list threaded through T::next.
//                    alloc/free are a couple of loads/stores, no syscalls, no
//                    allocator locks, and orders stay dense in memory.
//   HeapStorage<T> : the "naive" policy used for the optimisation story: every
//                    order is a separate `new T` / `delete`; the index maps to
//                    a pointer table. Same interface, so the book is identical.
#pragma once

#include <cstdint>
#include <vector>

#include "ob/types.hpp"

namespace ob {

template <class T>
class PoolStorage {
public:
    explicit PoolStorage(std::uint32_t capacity = 1024) { grow(capacity == 0 ? 1024 : capacity); }

    std::uint32_t alloc() {
        if (free_head_ == kNil) [[unlikely]]
            grow(static_cast<std::uint32_t>(slots_.size()));
        const std::uint32_t i = free_head_;
        free_head_ = slots_[i].next;
        ++live_;
        return i;
    }
    void free(std::uint32_t i) noexcept {
        slots_[i].next = free_head_;
        free_head_ = i;
        --live_;
    }
    T& operator[](std::uint32_t i) noexcept { return slots_[i]; }
    const T& operator[](std::uint32_t i) const noexcept { return slots_[i]; }
    std::uint32_t live() const noexcept { return live_; }
    std::size_t capacity() const noexcept { return slots_.size(); }

    static constexpr const char* name() { return "pool"; }

private:
    // Growth is off the hot path: only when the pre-sized pool is exhausted.
    void grow(std::uint32_t extra) {
        const auto old = static_cast<std::uint32_t>(slots_.size());
        slots_.resize(static_cast<std::size_t>(old) + extra);  // value-init touches every page (pre-fault)
        // Thread new slots onto the free list in ascending order.
        for (std::uint32_t i = old + extra; i-- > old;) {
            slots_[i].next = free_head_;
            free_head_ = i;
        }
    }
    std::vector<T> slots_;
    std::uint32_t free_head_ = kNil;
    std::uint32_t live_ = 0;
};

template <class T>
class HeapStorage {
public:
    explicit HeapStorage(std::uint32_t capacity = 1024) {
        ptrs_.reserve(capacity);
        free_idx_.reserve(capacity);
    }
    ~HeapStorage() {
        for (T* p : ptrs_) delete p;
    }
    HeapStorage(const HeapStorage&) = delete;
    HeapStorage& operator=(const HeapStorage&) = delete;

    std::uint32_t alloc() {
        std::uint32_t i;
        if (!free_idx_.empty()) {
            i = free_idx_.back();
            free_idx_.pop_back();
        } else {
            i = static_cast<std::uint32_t>(ptrs_.size());
            ptrs_.push_back(nullptr);
        }
        ptrs_[i] = new T{};
        ++live_;
        return i;
    }
    void free(std::uint32_t i) {
        delete ptrs_[i];
        ptrs_[i] = nullptr;
        free_idx_.push_back(i);
        --live_;
    }
    T& operator[](std::uint32_t i) noexcept { return *ptrs_[i]; }
    const T& operator[](std::uint32_t i) const noexcept { return *ptrs_[i]; }
    std::uint32_t live() const noexcept { return live_; }

    static constexpr const char* name() { return "new/delete"; }

private:
    std::vector<T*> ptrs_;
    std::vector<std::uint32_t> free_idx_;
    std::uint32_t live_ = 0;
};

}  // namespace ob
