#include <catch2/catch_test_macros.hpp>
#include <random>
#include <unordered_map>

#include "ob/flat_map.hpp"

TEST_CASE("FlatIdMap matches std::unordered_map under churn", "[flatmap]") {
    ob::FlatIdMap m(16);  // tiny initial capacity: forces rehashes
    std::unordered_map<ob::OrderId, std::uint32_t> ref;
    std::mt19937_64 rng(7);
    for (int i = 0; i < 200'000; ++i) {
        // Small key space -> long probe chains and many backward shifts.
        const ob::OrderId k = rng() % 5000;
        switch (rng() % 3) {
            case 0: {
                const auto v = static_cast<std::uint32_t>(rng() % 1'000'000);
                REQUIRE(m.insert(k, v) == ref.emplace(k, v).second);
                break;
            }
            case 1: REQUIRE(m.erase(k) == (ref.erase(k) == 1)); break;
            default: {
                auto it = ref.find(k);
                REQUIRE(m.find(k) == (it == ref.end() ? ob::kNil : it->second));
            }
        }
        REQUIRE(m.size() == ref.size());
    }
    for (const auto& [k, v] : ref) REQUIRE(m.find(k) == v);
    REQUIRE(m.capacity() >= 2 * m.size());
}

TEST_CASE("FlatIdMap handles adversarial sequential ids", "[flatmap]") {
    ob::FlatIdMap m(1024);
    for (ob::OrderId k = 1; k <= 100'000; ++k) REQUIRE(m.insert(k << 20, static_cast<std::uint32_t>(k)));
    for (ob::OrderId k = 1; k <= 100'000; k += 2) REQUIRE(m.erase(k << 20));
    for (ob::OrderId k = 1; k <= 100'000; ++k)
        REQUIRE(m.find(k << 20) == (k % 2 ? ob::kNil : static_cast<std::uint32_t>(k)));
}
