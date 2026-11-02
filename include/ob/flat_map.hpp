// OrderId -> pool index maps.
//
//   FlatIdMap : open addressing, linear probing, power-of-two capacity,
//               Fibonacci hashing, backward-shift deletion (no tombstones, so
//               probe lengths do not degrade under the add/cancel churn that
//               dominates real order flow). 16-byte slots, 4 per cache line.
//   StdIdMap  : std::unordered_map wrapper, the baseline (node-based, one heap
//               allocation per insert, pointer chase per lookup).
#pragma once

#include <bit>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "ob/types.hpp"

namespace ob {

class FlatIdMap {
public:
    explicit FlatIdMap(std::size_t expected = 1024) {
        std::size_t cap = 16;
        while (cap < expected * 2) cap <<= 1;  // keep load factor <= 0.5
        rehash(cap);
    }

    // Returns kNil when absent.
    std::uint32_t find(OrderId key) const noexcept {
        std::size_t i = slot_of(key);
        for (;;) {
            const Slot& s = slots_[i];
            if (s.val == kNil) return kNil;
            if (s.key == key) return s.val;
            i = (i + 1) & mask_;
        }
    }

    bool contains(OrderId key) const noexcept { return find(key) != kNil; }

    // Inserts key -> val. Returns false (and does nothing) if key exists.
    bool insert(OrderId key, std::uint32_t val) {
        if ((size_ + 1) * 2 > slots_.size()) [[unlikely]]
            rehash(slots_.size() * 2);
        std::size_t i = slot_of(key);
        for (;;) {
            Slot& s = slots_[i];
            if (s.val == kNil) {
                s.key = key;
                s.val = val;
                ++size_;
                return true;
            }
            if (s.key == key) return false;
            i = (i + 1) & mask_;
        }
    }

    // Backward-shift deletion (Knuth 6.4 Algorithm R).
    bool erase(OrderId key) noexcept {
        std::size_t i = slot_of(key);
        for (;;) {
            const Slot& s = slots_[i];
            if (s.val == kNil) return false;
            if (s.key == key) break;
            i = (i + 1) & mask_;
        }
        std::size_t hole = i;
        std::size_t j = i;
        for (;;) {
            j = (j + 1) & mask_;
            Slot& s = slots_[j];
            if (s.val == kNil) break;
            const std::size_t home = slot_of(s.key);
            // Move s into the hole iff its home is not cyclically in (hole, j].
            const bool in_range = hole <= j ? (hole < home && home <= j) : (hole < home || home <= j);
            if (!in_range) {
                slots_[hole] = s;
                hole = j;
            }
        }
        slots_[hole].val = kNil;
        --size_;
        return true;
    }

    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return slots_.size(); }
    static constexpr const char* name() { return "flat-hash"; }

private:
    struct Slot {
        OrderId key = 0;
        std::uint32_t val = kNil;
    };
    std::size_t slot_of(OrderId key) const noexcept {
        return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> shift_);
    }
    void rehash(std::size_t cap) {
        std::vector<Slot> old;
        old.swap(slots_);
        slots_.assign(cap, Slot{});
        mask_ = cap - 1;
        shift_ = 64 - static_cast<unsigned>(std::countr_zero(cap));
        size_ = 0;
        for (const Slot& s : old)
            if (s.val != kNil) insert(s.key, s.val);
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    unsigned shift_ = 0;
    std::size_t size_ = 0;
};

class StdIdMap {
public:
    explicit StdIdMap(std::size_t expected = 1024) { map_.reserve(expected); }
    std::uint32_t find(OrderId key) const {
        auto it = map_.find(key);
        return it == map_.end() ? kNil : it->second;
    }
    bool contains(OrderId key) const { return map_.find(key) != map_.end(); }
    bool insert(OrderId key, std::uint32_t val) { return map_.emplace(key, val).second; }
    bool erase(OrderId key) { return map_.erase(key) != 0; }
    std::size_t size() const noexcept { return map_.size(); }
    static constexpr const char* name() { return "std::unordered_map"; }

private:
    std::unordered_map<OrderId, std::uint32_t> map_;
};

}  // namespace ob
