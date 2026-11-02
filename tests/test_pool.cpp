#include <catch2/catch_test_macros.hpp>
#include <set>

#include "ob/book.hpp"
#include "ob/pool.hpp"

TEST_CASE("PoolStorage recycles slots LIFO and grows", "[pool]") {
    ob::PoolStorage<ob::OrderPacked> p(4);
    std::set<std::uint32_t> got;
    for (int i = 0; i < 10; ++i) REQUIRE(got.insert(p.alloc()).second);  // grows past 4
    REQUIRE(p.live() == 10);
    REQUIRE(p.capacity() >= 10);
    p.free(3);
    REQUIRE(p.alloc() == 3);  // hot slot reused first
    for (auto i : got) p.free(i);
    REQUIRE(p.live() == 0);
}

TEST_CASE("HeapStorage behaves like the pool", "[pool]") {
    ob::HeapStorage<ob::OrderLoose> h(2);
    const auto a = h.alloc(), b = h.alloc();
    h[a].id = 11;
    h[b].id = 22;
    h.free(a);
    const auto c = h.alloc();
    REQUIRE(c == a);
    REQUIRE(h[b].id == 22);
    REQUIRE(h.live() == 2);
}

TEST_CASE("order layouts", "[pool]") {
    STATIC_REQUIRE(sizeof(ob::OrderPacked) == 32);
    STATIC_REQUIRE(alignof(ob::OrderPacked) == 32);
    STATIC_REQUIRE(sizeof(ob::OrderLoose) == 80);
}
