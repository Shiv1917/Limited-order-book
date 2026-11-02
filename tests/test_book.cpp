// Behavioural unit tests. Every test runs against the reference book and all
// four optimisation stages of the fast book, so the spec is enforced uniformly.
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include "book_types.hpp"

using namespace ob;
using namespace obtest;

#define ALL_BOOKS RefBook, BookFlatLevels, BookPool, BookFlatHash, Book

namespace {
template <class B>
void check_valid(const B& b) {
    if constexpr (requires { b.validate(); }) {
        std::string why;
        INFO(why);
        REQUIRE(b.validate(&why));
    }
}
}  // namespace

TEMPLATE_TEST_CASE("resting limit orders build the book", "[book]", ALL_BOOKS) {
    Harness<TestType> h;
    auto ev = h.send(Message::add(1, Side::Buy, P(10), 100));
    REQUIRE(ev == std::vector<Event>{make_ack(1, Side::Buy, P(10), 100)});
    h.send(Message::add(2, Side::Buy, P(9), 50));
    h.send(Message::add(3, Side::Sell, P(12), 70));
    h.send(Message::add(4, Side::Sell, P(11), 30));

    REQUIRE(h.book.best_bid() == P(10));
    REQUIRE(h.book.best_ask() == P(11));
    REQUIRE(h.book.depth(Side::Buy) == std::vector<LevelInfo>{{P(10), 100, 1}, {P(9), 50, 1}});
    REQUIRE(h.book.depth(Side::Sell) == std::vector<LevelInfo>{{P(11), 30, 1}, {P(12), 70, 1}});
    REQUIRE(h.book.order_count() == 4);
    check_valid(h.book);
}

TEMPLATE_TEST_CASE("crossing limit order trades at the maker's price", "[book][match]", ALL_BOOKS) {
    Harness<TestType> h;
    h.send(Message::add(1, Side::Sell, P(10), 100));
    auto ev = h.send(Message::add(2, Side::Buy, P(12), 40));  // crosses the spread
    REQUIRE(ev == std::vector<Event>{
                      make_ack(2, Side::Buy, P(12), 40),
                      make_trade(2, 1, Side::Buy, P(10), 40),  // price improvement for the taker
                  });
    REQUIRE(h.book.find(1)->qty == 60);
    REQUIRE(!h.book.best_bid());
    check_valid(h.book);
}

TEMPLATE_TEST_CASE("partial fill rests the remainder", "[book][match]", ALL_BOOKS) {
    Harness<TestType> h;
    h.send(Message::add(1, Side::Sell, P(10), 30));
    auto ev = h.send(Message::add(2, Side::Buy, P(10), 100));
    REQUIRE(ev == std::vector<Event>{make_ack(2, Side::Buy, P(10), 100), make_trade(2, 1, Side::Buy, P(10), 30)});
    REQUIRE(h.book.best_bid() == P(10));
    REQUIRE(!h.book.best_ask());
    REQUIRE(h.book.find(2)->qty == 70);
    REQUIRE(!h.book.find(1));
    check_valid(h.book);
}

TEMPLATE_TEST_CASE("price-time priority within and across levels", "[book][match]", ALL_BOOKS) {
    Harness<TestType> h;
    h.send(Message::add(1, Side::Sell, P(11), 10));
    h.send(Message::add(2, Side::Sell, P(10), 10));
    h.send(Message::add(3, Side::Sell, P(10), 10));
    h.send(Message::add(4, Side::Sell, P(10), 10));
    auto ev = h.send(Message::add(9, Side::Buy, P(11), 35));
    REQUIRE(ev == std::vector<Event>{
                      make_ack(9, Side::Buy, P(11), 35),
                      make_trade(9, 2, Side::Buy, P(10), 10),
                      make_trade(9, 3, Side::Buy, P(10), 10),
                      make_trade(9, 4, Side::Buy, P(10), 10),
                      make_trade(9, 1, Side::Buy, P(11), 5),
                  });
    REQUIRE(h.book.best_ask() == P(11));
    REQUIRE(h.book.find(1)->qty == 5);
    check_valid(h.book);
}

TEMPLATE_TEST_CASE("sweep through multiple levels with gaps", "[book][match]", ALL_BOOKS) {
    Harness<TestType> h;
    h.send(Message::add(1, Side::Buy, P(100), 10));
    h.send(Message::add(2, Side::Buy, P(90), 10));
    h.send(Message::add(3, Side::Buy, P(20), 10));
    h.send(Message::add(4, Side::Buy, P(5), 10));
    auto ev = h.send(Message::add(5, Side::Sell, P(20), 100));  // limit stops at P(20)
    REQUIRE(ev == std::vector<Event>{
                      make_ack(5, Side::Sell, P(20), 100),
                      make_trade(5, 1, Side::Sell, P(100), 10),
                      make_trade(5, 2, Side::Sell, P(90), 10),
                      make_trade(5, 3, Side::Sell, P(20), 10),
                  });
    REQUIRE(h.book.best_bid() == P(5));
    REQUIRE(h.book.best_ask() == P(20));
    REQUIRE(h.book.find(5)->qty == 70);
    check_valid(h.book);
}

TEMPLATE_TEST_CASE("market order sweeps and cancels the remainder", "[book][market]", ALL_BOOKS) {
    Harness<TestType> h;
    h.send(Message::add(1, Side::Sell, P(10), 10));
    h.send(Message::add(2, Side::Sell, P(500), 10));
    auto ev = h.send(Message::market(3, Side::Buy, 25));
    REQUIRE(ev == std::vector<Event>{
                      make_ack(3, Side::Buy, 0, 25),
                      make_trade(3, 1, Side::Buy, P(10), 10),
                      make_trade(3, 2, Side::Buy, P(500), 10),
                      make_cancel(3, Side::Buy, 0, 5, Reason::Unfilled),
                  });
    REQUIRE(h.book.order_count() == 0);
    check_valid(h.book);
}

TEMPLATE_TEST_CASE("market order into an empty book", "[book][market]", ALL_BOOKS) {
    Harness<TestType> h;
    auto ev = h.send(Message::market(1, Side::Sell, 10));
    REQUIRE(ev == std::vector<Event>{make_ack(1, Side::Sell, 0, 10), make_cancel(1, Side::Sell, 0, 10, Reason::Unfilled)});
}

TEMPLATE_TEST_CASE("IOC fills what it can and never rests", "[book][tif]", ALL_BOOKS) {
    Harness<TestType> h;
    h.send(Message::add(1, Side::Sell, P(10), 10));
    auto ev = h.send(Message::add(2, Side::Buy, P(10), 25, TimeInForce::IOC));
    REQUIRE(ev == std::vector<Event>{
                      make_ack(2, Side::Buy, P(10), 25),
                      make_trade(2, 1, Side::Buy, P(10), 10),
                      make_cancel(2, Side::Buy, P(10), 15, Reason::Unfilled),
                  });
    REQUIRE(h.book.order_count() == 0);
    // IOC that does not cross: acked then fully cancelled.
    ev = h.send(Message::add(3, Side::Buy, P(5), 10, TimeInForce::IOC));
    REQUIRE(ev.back() == make_cancel(3, Side::Buy, P(5), 10, Reason::Unfilled));
    check_valid(h.book);
}

TEMPLATE_TEST_CASE("FOK that cannot fully fill does nothing", "[book][tif]", ALL_BOOKS) {
    Harness<TestType> h;
    h.send(Message::add(1, Side::Sell, P(10), 10));
    h.send(Message::add(2, Side::Sell, P(11), 10));
    h.send(Message::add(3, Side::Sell, P(13), 100));  // beyond the limit, must not count
    auto ev = h.send(Message::add(4, Side::Buy, P(11), 21, TimeInForce::FOK));
    REQUIRE(ev == std::vector<Event>{
                      make_ack(4, Side::Buy, P(11), 21),
                      make_cancel(4, Side::Buy, P(11), 21, Reason::FokUnfillable),
                  });
    REQUIRE(h.book.find(1)->qty == 10);  // untouched
    REQUIRE(h.book.find(2)->qty == 10);
    // Exactly fillable FOK goes through.
    ev = h.send(Message::add(5, Side::Buy, P(11), 20, TimeInForce::FOK));
    REQUIRE(ev.size() == 3);
    REQUIRE(h.book.best_ask() == P(13));
    // Market FOK.
    ev = h.send(Message::market(6, Side::Buy, 101, TimeInForce::FOK));
    REQUIRE(ev.back().reason == Reason::FokUnfillable);
    check_valid(h.book);
}

TEMPLATE_TEST_CASE("cancel", "[book][cancel]", ALL_BOOKS) {
    Harness<TestType> h;
    h.send(Message::add(1, Side::Buy, P(10), 10));
    h.send(Message::add(2, Side::Buy, P(10), 20));
    auto ev = h.send(Message::cancel(1));
    REQUIRE(ev == std::vector<Event>{make_cancel(1, Side::Buy, P(10), 10, Reason::UserCancel)});
    REQUIRE(h.book.depth(Side::Buy) == std::vector<LevelInfo>{{P(10), 20, 1}});
    // Cancelling it again is an unknown order.
    REQUIRE(h.send(Message::cancel(1)) == std::vector<Event>{make_reject(1, Side::Buy, Reason::UnknownOrder)});
    h.send(Message::cancel(2));
    REQUIRE(!h.book.best_bid());
    check_valid(h.book);
}

TEMPLATE_TEST_CASE("cancel a filled order is rejected", "[book][cancel]", ALL_BOOKS) {
    Harness<TestType> h;
    h.send(Message::add(1, Side::Sell, P(10), 10));
    h.send(Message::add(2, Side::Buy, P(10), 10));
    REQUIRE(h.send(Message::cancel(1)) == std::vector<Event>{make_reject(1, Side::Buy, Reason::UnknownOrder)});
    REQUIRE(h.send(Message::cancel(2)) == std::vector<Event>{make_reject(2, Side::Buy, Reason::UnknownOrder)});
}

TEMPLATE_TEST_CASE("modify a nonexistent order is rejected", "[book][modify]", ALL_BOOKS) {
    Harness<TestType> h;
    REQUIRE(h.send(Message::modify(42, P(10), 5)) ==
            std::vector<Event>{make_reject(42, Side::Buy, Reason::UnknownOrder)});
}

TEMPLATE_TEST_CASE("modify: quantity decrease keeps priority", "[book][modify]", ALL_BOOKS) {
    Harness<TestType> h;
    h.send(Message::add(1, Side::Sell, P(10), 50));
    h.send(Message::add(2, Side::Sell, P(10), 50));
    REQUIRE(h.send(Message::modify(1, P(10), 20)) ==
            std::vector<Event>{make_ack(1, Side::Sell, P(10), 20, Reason::Modified)});
    REQUIRE(h.book.depth(Side::Sell) == std::vector<LevelInfo>{{P(10), 70, 2}});
    auto ev = h.send(Message::add(3, Side::Buy, P(10), 10));
    REQUIRE(ev[1] == make_trade(3, 1, Side::Buy, P(10), 10));  // order 1 still first
    check_valid(h.book);
}

TEMPLATE_TEST_CASE("modify: quantity increase loses priority", "[book][modify]", ALL_BOOKS) {
    Harness<TestType> h;
    h.send(Message::add(1, Side::Sell, P(10), 50));
    h.send(Message::add(2, Side::Sell, P(10), 50));
    h.send(Message::modify(1, P(10), 60));
    auto ev = h.send(Message::add(3, Side::Buy, P(10), 10));
    REQUIRE(ev[1] == make_trade(3, 2, Side::Buy, P(10), 10));  // order 2 now first
    check_valid(h.book);
}

TEMPLATE_TEST_CASE("modify: price change loses priority and can cross", "[book][modify]", ALL_BOOKS) {
    Harness<TestType> h;
    h.send(Message::add(1, Side::Buy, P(9), 50));
    h.send(Message::add(2, Side::Buy, P(10), 50));
    h.send(Message::add(3, Side::Sell, P(12), 30));
    // Self-crossing modify: bid 1 is repriced through the ask.
    auto ev = h.send(Message::modify(1, P(13), 40));
    REQUIRE(ev == std::vector<Event>{
                      make_ack(1, Side::Buy, P(13), 40, Reason::Modified),
                      make_trade(1, 3, Side::Buy, P(12), 30),
                  });
    REQUIRE(h.book.best_bid() == P(13));
    REQUIRE(h.book.find(1)->qty == 10);
    REQUIRE(!h.book.best_ask());
    check_valid(h.book);
}

TEMPLATE_TEST_CASE("modify: same price same qty is a no-op ack that keeps priority", "[book][modify]", ALL_BOOKS) {
    Harness<TestType> h;
    h.send(Message::add(1, Side::Buy, P(10), 50));
    h.send(Message::add(2, Side::Buy, P(10), 50));
    h.send(Message::modify(1, P(10), 50));
    auto ev = h.send(Message::market(3, Side::Sell, 1));
    REQUIRE(ev[1].maker_id == 1);
}

TEMPLATE_TEST_CASE("an order never trades with itself after a modify", "[book][modify]", ALL_BOOKS) {
    // The modified order is removed before it is re-matched, so it cannot
    // match its own former resting quantity.
    Harness<TestType> h;
    h.send(Message::add(1, Side::Sell, P(10), 50));
    auto ev = h.send(Message::modify(1, P(9), 50));
    REQUIRE(ev.size() == 1);
    REQUIRE(h.book.best_ask() == P(9));
}

TEMPLATE_TEST_CASE("validation rejects", "[book][reject]", ALL_BOOKS) {
    Harness<TestType> h;
    REQUIRE(h.send(Message::add(1, Side::Buy, P(10), 0)) ==
            std::vector<Event>{make_reject(1, Side::Buy, Reason::InvalidQty)});
    REQUIRE(h.send(Message::add(1, Side::Buy, P(10) + 1, 5)) ==
            std::vector<Event>{make_reject(1, Side::Buy, Reason::InvalidPrice)});  // off tick
    REQUIRE(h.send(Message::add(1, Side::Buy, P(-1), 5)) ==
            std::vector<Event>{make_reject(1, Side::Buy, Reason::InvalidPrice)});  // below band
    REQUIRE(h.send(Message::add(1, Side::Buy, P(1024), 5)) ==
            std::vector<Event>{make_reject(1, Side::Buy, Reason::InvalidPrice)});  // above band
    h.send(Message::add(1, Side::Buy, P(10), 5));
    REQUIRE(h.send(Message::add(1, Side::Sell, P(20), 5)) ==
            std::vector<Event>{make_reject(1, Side::Sell, Reason::DuplicateId)});
    REQUIRE(h.send(Message::market(1, Side::Sell, 5)) ==
            std::vector<Event>{make_reject(1, Side::Sell, Reason::DuplicateId)});
    REQUIRE(h.send(Message::modify(1, P(10), 0)) == std::vector<Event>{make_reject(1, Side::Buy, Reason::InvalidQty)});
    REQUIRE(h.send(Message::modify(1, P(5000), 1)) ==
            std::vector<Event>{make_reject(1, Side::Buy, Reason::InvalidPrice)});
    REQUIRE(h.book.find(1)->qty == 5);  // rejected modifies leave the order alone
    check_valid(h.book);
}

TEMPLATE_TEST_CASE("extreme band edges", "[book][edge]", ALL_BOOKS) {
    Harness<TestType> h;
    h.send(Message::add(1, Side::Buy, P(0), 5));
    h.send(Message::add(2, Side::Sell, P(1023), 5));
    REQUIRE(h.book.best_bid() == P(0));
    REQUIRE(h.book.best_ask() == P(1023));
    auto ev = h.send(Message::market(3, Side::Buy, 5));
    REQUIRE(ev[1] == make_trade(3, 2, Side::Buy, P(1023), 5));
    ev = h.send(Message::market(4, Side::Sell, 5));
    REQUIRE(ev[1] == make_trade(4, 1, Side::Sell, P(0), 5));
    REQUIRE(h.book.order_count() == 0);
    check_valid(h.book);
}

TEST_CASE("fast book: best level recovers through a large empty gap", "[book][bitset]") {
    // 2^18 levels -> 4-layer bitset; next-best search crosses many empty words.
    Book b(BookConfig{PriceBand{0, 1, 1u << 18}, 64});
    EventBuffer out;
    b.process(Message::add(1, Side::Sell, 5, 1), out);
    b.process(Message::add(2, Side::Sell, 200'000, 1), out);
    b.process(Message::add(3, Side::Buy, 3, 1), out);
    b.process(Message::add(4, Side::Buy, 0, 1), out);
    b.process(Message::cancel(1), out);
    REQUIRE(b.best_ask() == 200'000);
    b.process(Message::cancel(3), out);
    REQUIRE(b.best_bid() == 0);
    std::string why;
    REQUIRE(b.validate(&why));
}

TEST_CASE("fast book: pool grows past its initial capacity", "[book][pool]") {
    Book b(BookConfig{PriceBand{0, 1, 128}, 4});
    EventBuffer out;
    for (OrderId i = 1; i <= 1000; ++i) b.process(Message::add(i, Side::Buy, static_cast<Price>(i % 64), 1), out);
    REQUIRE(b.order_count() == 1000);
    REQUIRE(b.validate());
}
