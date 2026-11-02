#include <catch2/catch_test_macros.hpp>
#include <random>
#include <set>

#include "ob/level_bitset.hpp"

using ob::LevelBitset;

TEST_CASE("bitset next/prev agree with std::set", "[bitset]") {
    for (std::size_t n : {1u, 63u, 64u, 65u, 4096u, 4097u, 300000u}) {
        LevelBitset b(n);
        std::set<std::size_t> ref;
        std::mt19937_64 rng(n);
        std::uniform_int_distribution<std::size_t> pick(0, n - 1);
        for (int step = 0; step < 4000; ++step) {
            const std::size_t i = pick(rng);
            if (rng() % 3) {
                b.set(i);
                ref.insert(i);
            } else {
                b.clear(i);
                ref.erase(i);
            }
            const std::size_t q = pick(rng);
            auto it = ref.lower_bound(q);
            REQUIRE(b.next(q) == (it == ref.end() ? LevelBitset::npos : *it));
            auto jt = ref.upper_bound(q);
            REQUIRE(b.prev(q) == (jt == ref.begin() ? LevelBitset::npos : *std::prev(jt)));
            REQUIRE(b.any() == !ref.empty());
            REQUIRE(b.test(i) == (ref.count(i) == 1));
        }
    }
}

TEST_CASE("bitset edges", "[bitset]") {
    LevelBitset b(1000);
    REQUIRE(b.next(0) == LevelBitset::npos);
    REQUIRE(b.prev(999) == LevelBitset::npos);
    b.set(0);
    b.set(999);
    REQUIRE(b.next(1) == 999);
    REQUIRE(b.prev(998) == 0);
    REQUIRE(b.next(1000) == LevelBitset::npos);
    REQUIRE(b.prev(5000) == 999);  // clamps
    b.set(0);  // idempotent
    b.clear(0);
    REQUIRE(b.prev(998) == LevelBitset::npos);
}
