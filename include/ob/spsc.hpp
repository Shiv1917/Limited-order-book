// Lock-free single-producer / single-consumer ring buffer.
//
// * Capacity is a power of two; indices are free-running 64-bit counters.
// * head_ (consumer) and tail_ (producer) live on separate cache lines.
// * Each side keeps a *cached* copy of the other side's index and only
//   re-reads the shared atomic when the cache says the ring is full/empty,
//   which removes most cross-core cache-line traffic.
// * acquire/release ordering only; no CAS, no locks.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace ob {

// 128 bytes: covers x86 adjacent-line prefetch pairs and Apple M-series
// 128-byte lines. (std::hardware_destructive_interference_size is not ABI
// stable across GCC flags, so we pick a conservative constant.)
inline constexpr std::size_t kPad = 128;

template <class T>
class SpscRing {
    static_assert(std::is_trivially_copyable_v<T>, "SpscRing holds trivially copyable payloads");

public:
    explicit SpscRing(std::size_t capacity_pow2) : mask_(capacity_pow2 - 1), buf_(new T[capacity_pow2]) {
        if (capacity_pow2 == 0 || (capacity_pow2 & mask_) != 0) throw std::invalid_argument("capacity must be 2^k");
    }

    // Producer side.
    bool try_push(const T& v) noexcept {
        const std::uint64_t t = tail_.load(std::memory_order_relaxed);
        if (t - head_cache_ > mask_) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (t - head_cache_ > mask_) return false;  // full
        }
        buf_[t & mask_] = v;
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // Consumer side.
    bool try_pop(T& out) noexcept {
        const std::uint64_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (h == tail_cache_) return false;  // empty
        }
        out = buf_[h & mask_];
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    std::size_t capacity() const noexcept { return mask_ + 1; }
    std::size_t size_approx() const noexcept {
        return static_cast<std::size_t>(tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire));
    }

private:
    const std::uint64_t mask_;
    std::unique_ptr<T[]> buf_;
    alignas(kPad) std::atomic<std::uint64_t> head_{0};  // written by consumer
    std::uint64_t tail_cache_ = 0;                      // consumer's view of tail
    alignas(kPad) std::atomic<std::uint64_t> tail_{0};  // written by producer
    std::uint64_t head_cache_ = 0;                      // producer's view of head
    char pad_[kPad - sizeof(std::uint64_t)]{};
};

}  // namespace ob
