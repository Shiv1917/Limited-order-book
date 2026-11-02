// Hierarchical occupancy bitset over price levels.
//
// Layer 0 has one bit per price level; each bit in layer k+1 says "word w of
// layer k is non-zero". With 64-bit words, L layers cover 64^L levels, e.g.
// 3 layers = 262,144 levels, 4 layers = 16.7M. next()/prev() therefore cost at
// most 2*L word reads + ctz/clz, independent of how many empty levels lie
// between two occupied prices. This is the "bounded bitset scan" used to find
// the next best price when the best level empties.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ob {

class LevelBitset {
public:
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    explicit LevelBitset(std::size_t nbits = 64) { resize(nbits); }

    void resize(std::size_t nbits) {
        nbits_ = nbits;
        layers_.clear();
        std::size_t n = nbits;
        do {
            const std::size_t words = (n + 63) / 64;
            layers_.emplace_back(words, 0);
            n = words;
        } while (n > 1);
    }

    std::size_t size() const noexcept { return nbits_; }

    bool test(std::size_t i) const noexcept { return (layers_[0][i >> 6] >> (i & 63)) & 1u; }

    void set(std::size_t i) noexcept {
        for (auto& layer : layers_) {
            std::uint64_t& w = layer[i >> 6];
            const bool was_zero = (w == 0);
            w |= std::uint64_t{1} << (i & 63);
            if (!was_zero) return;  // parent bits already set
            i >>= 6;
        }
    }

    void clear(std::size_t i) noexcept {
        for (auto& layer : layers_) {
            std::uint64_t& w = layer[i >> 6];
            w &= ~(std::uint64_t{1} << (i & 63));
            if (w != 0) return;  // word still non-empty: parent stays set
            i >>= 6;
        }
    }

    bool any() const noexcept { return layers_.back()[0] != 0; }

    // Smallest set index >= i, or npos.
    std::size_t next(std::size_t i) const noexcept {
        if (i >= nbits_) return npos;
        std::size_t layer = 0;
        for (;;) {
            if (layer == layers_.size()) return npos;
            const auto& words = layers_[layer];
            const std::size_t w = i >> 6;
            if (w >= words.size()) return npos;
            const std::uint64_t bits = words[w] & (~std::uint64_t{0} << (i & 63));
            if (bits) {
                i = (w << 6) | static_cast<std::size_t>(std::countr_zero(bits));
                break;
            }
            i = w + 1;
            ++layer;
        }
        while (layer > 0) {
            --layer;
            i = (i << 6) | static_cast<std::size_t>(std::countr_zero(layers_[layer][i]));
        }
        return i;
    }

    // Largest set index <= i, or npos.
    std::size_t prev(std::size_t i) const noexcept {
        if (i == npos) return npos;
        if (i >= nbits_) i = nbits_ - 1;
        std::size_t layer = 0;
        for (;;) {
            if (layer == layers_.size()) return npos;
            const auto& words = layers_[layer];
            const std::size_t w = i >> 6;
            const std::uint64_t bits = words[w] & (~std::uint64_t{0} >> (63 - (i & 63)));
            if (bits) {
                i = (w << 6) | static_cast<std::size_t>(63 - std::countl_zero(bits));
                break;
            }
            if (w == 0) return npos;
            i = w - 1;
            ++layer;
        }
        while (layer > 0) {
            --layer;
            i = (i << 6) | static_cast<std::size_t>(63 - std::countl_zero(layers_[layer][i]));
        }
        return i;
    }

private:
    std::size_t nbits_ = 0;
    std::vector<std::vector<std::uint64_t>> layers_;
};

}  // namespace ob
