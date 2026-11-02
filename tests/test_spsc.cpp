#include <catch2/catch_test_macros.hpp>
#include <thread>

#include "ob/spsc.hpp"

TEST_CASE("SPSC ring single-threaded semantics", "[spsc]") {
    ob::SpscRing<int> r(4);
    int v;
    REQUIRE(!r.try_pop(v));
    for (int i = 0; i < 4; ++i) REQUIRE(r.try_push(i));
    REQUIRE(!r.try_push(99));  // full
    REQUIRE(r.try_pop(v));
    REQUIRE(v == 0);
    REQUIRE(r.try_push(4));
    for (int i = 1; i <= 4; ++i) {
        REQUIRE(r.try_pop(v));
        REQUIRE(v == i);
    }
    REQUIRE(!r.try_pop(v));
    REQUIRE_THROWS(ob::SpscRing<int>(3));
}

TEST_CASE("SPSC ring preserves order across threads", "[spsc]") {
    constexpr std::uint64_t N = 2'000'000;
    ob::SpscRing<std::uint64_t> r(1024);
    std::thread prod([&] {
        for (std::uint64_t i = 0; i < N; ++i)
            while (!r.try_push(i)) {
            }
    });
    std::uint64_t expect = 0, v;
    while (expect < N)
        if (r.try_pop(v)) {
            REQUIRE(v == expect);
            ++expect;
        }
    prod.join();
}
