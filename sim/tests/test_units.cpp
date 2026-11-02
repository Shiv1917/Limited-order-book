// Unit tests: accounting (validation layer 2), event ordering, latency,
// hashing, formatting, and the book additions the simulator relies on.
#include <catch2/catch_test_macros.hpp>

#include "lobster.hpp"
#include "ob/book.hpp"
#include "sim/book_view.hpp"
#include "sim/event_queue.hpp"
#include "sim/latency.hpp"
#include "sim/params.hpp"
#include "sim/portfolio.hpp"
#include "sim/sha256.hpp"

using namespace sim;

namespace {
// realized + unrealized - fees == equity - equity_0, in x2 units (equity_0 = 0).
void check_identity(const Portfolio& p, Price mid_x2) {
    CHECK(2 * p.realized() + p.unrealized_x2(mid_x2) - 2 * p.fees() == p.equity_x2(mid_x2));
}
}  // namespace

TEST_CASE("portfolio: round trip with fees and rebates", "[sim][portfolio]") {
    Portfolio p;
    p.on_fill(Side::Buy, 100000, 100, -2000);  // maker rebate $0.002 x 100
    CHECK(p.position() == 100);
    CHECK(p.cash() == -100000 * 100 + 2000);
    check_identity(p, 2 * 100050);
    p.on_fill(Side::Sell, 100100, 100, 3000);  // taker fee
    CHECK(p.position() == 0);
    CHECK(p.realized() == 100 * 100);  // +1 cent x 100 shares
    CHECK(p.fees() == 1000);
    CHECK(p.cash() == 100 * 100 - 1000);
    CHECK(p.basis() == 0);
    check_identity(p, 2 * 99000);
    CHECK(p.turnover() == 200);
    CHECK(p.max_abs_position() == 100);
}

TEST_CASE("portfolio: partial close uses average cost", "[sim][portfolio]") {
    Portfolio p;
    p.on_fill(Side::Buy, 100000, 100, 0);
    p.on_fill(Side::Buy, 100200, 100, 0);  // avg cost 10.01
    p.on_fill(Side::Sell, 100300, 50, 0);  // realize (10.03 - 10.01) x 50
    CHECK(p.position() == 150);
    CHECK(p.realized() == 200 * 50);
    CHECK(p.basis() == 100100 * 150);
    check_identity(p, 2 * 100250);
}

TEST_CASE("portfolio: flip through zero resets cost to the fill price", "[sim][portfolio]") {
    Portfolio p;
    p.on_fill(Side::Buy, 100000, 100, 0);
    p.on_fill(Side::Sell, 100100, 150, 0);  // close 100 at +100, open short 50 at 10.01
    CHECK(p.position() == -50);
    CHECK(p.realized() == 100 * 100);
    CHECK(p.basis() == -50 * 100100);
    check_identity(p, 2 * 100000 + 100);
    p.on_fill(Side::Buy, 99900, 50, 0);  // cover: (10.01 - 9.99) x 50
    CHECK(p.position() == 0);
    CHECK(p.realized() == 100 * 100 + 200 * 50);
    check_identity(p, 0);
    CHECK(p.max_abs_position() == 100);
}

TEST_CASE("portfolio: identity holds over a messy fill sequence", "[sim][portfolio]") {
    Portfolio p;
    std::mt19937_64 rng(7);
    for (int i = 0; i < 5000; ++i) {
        const Side s = rng() % 2 ? Side::Buy : Side::Sell;
        const Price px = 100000 + static_cast<Price>(rng() % 41) * 100 - 2000;
        const auto q = static_cast<Qty>(1 + rng() % 300);
        const std::int64_t fee = (rng() % 2 ? 30 : -20) * static_cast<std::int64_t>(q);
        p.on_fill(s, px, q, fee);
        const Price mid_x2 = 2 * (100000 + static_cast<Price>(rng() % 21) * 100 - 1000) + (rng() % 2 ? 100 : 0);
        check_identity(p, mid_x2);
    }
}

TEST_CASE("event queue orders by (ts, priority, seq)", "[sim][queue]") {
    EventQueue q;
    auto push = [&](Time ts, Prio p, int tag) {
        SimEvent e;
        e.ts = ts;
        e.prio = p;
        e.timer_id = tag;
        q.push(e);
    };
    push(10, Prio::Timer, 1);
    push(10, Prio::Deliver, 2);
    push(5, Prio::Timer, 3);
    push(10, Prio::OrderArrive, 4);
    push(10, Prio::OrderArrive, 5);  // same key as 4: FIFO by seq
    std::vector<int> order;
    while (!q.empty()) order.push_back(q.pop().timer_id);
    CHECK(order == std::vector<int>{3, 4, 5, 2, 1});
    CHECK(q.top_ts() == kNever);
}

TEST_CASE("latency: fixed delays, feed only for market-data triggers", "[sim][latency]") {
    LatencyModel m(LatencyConfig{20, 10, 50, 40, 0}, 1);
    CHECK(m.order_arrival(1000, true) == 1080);
    CHECK(m.order_arrival(1000, false) == 1080);  // clamped: may not overtake the previous order
    CHECK(m.order_arrival(2000, false) == 2060);
    CHECK(m.delivery(3000) == 3040);
}

TEST_CASE("latency: jitter is seeded, bounded, and never reorders a stream", "[sim][latency]") {
    const LatencyConfig cfg{0, 0, 100, 100, 1000};
    LatencyModel a(cfg, 99), b(cfg, 99), c(cfg, 100);
    bool differs = false;
    Time prev_in = 0, prev_out = 0;
    for (Time t = 0; t < 100000; t += 10) {
        const Time x = a.order_arrival(t, false), y = b.order_arrival(t, false), z = c.order_arrival(t, false);
        CHECK(x == y);
        differs |= x != z;
        CHECK(x >= t + 100);
        CHECK(x >= prev_in);
        prev_in = x;
        const Time d = a.delivery(t);
        b.delivery(t);
        c.delivery(t);
        CHECK(d >= prev_out);
        prev_out = d;
    }
    CHECK(differs);
}

TEST_CASE("sha256 known vectors", "[sim][sha]") {
    CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex(std::string(1000000, 'a')) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("exact formatting and time parsing", "[sim][format]") {
    CHECK(fmt_fixed(1234500, 4) == "123.4500");
    CHECK(fmt_fixed(-5, 4) == "-0.0005");
    CHECK(fmt_fixed(0, 2) == "0.00");
    CHECK(fmt_mid_x2(2 * 100000 + 100) == "10.00500");
    CHECK(fmt_time(34200004241176) == "34200.004241176");
    CHECK(parse_time(json("34200.5")) == 34200500000000);
    CHECK(parse_time(json("34200.004241176")) == 34200004241176);
    CHECK(parse_time(json(17)) == 17);
    CHECK_THROWS(parse_time(json("12:00")));
    ob::lobster::Row r;
    REQUIRE(ob::lobster::parse_row("34200.004241176,1,16113575,18,5853300,1", r));
    CHECK(r.time_ns == 34200004241176);
    REQUIRE(ob::lobster::parse_row("57599.9,3,1,1,1,-1", r));
    CHECK(r.time_ns == 57599900000000);
}

TEST_CASE("book: arrival sequence and read-only queue walk", "[sim][book]") {
    ob::Book b(ob::BookConfig{ob::PriceBand{90000, 100, 512}, 64});
    ob::EventBuffer out;
    b.process(ob::Message::add(1, Side::Buy, 100000, 50), out);
    b.process(ob::Message::add(2, Side::Buy, 100000, 30), out);
    b.process(ob::Message::add(3, Side::Buy, 99900, 10), out);
    b.process(ob::Message::add(4, Side::Sell, 100200, 70), out);
    CHECK(b.last_seq() == 4);
    CHECK(b.queue_entry(1)->seq == 1);
    CHECK(b.queue_entry(2)->seq == 2);
    // Quantity decrease keeps priority (and seq); a price change re-queues.
    b.process(ob::Message::modify(1, 100000, 20), out);
    CHECK(b.queue_entry(1)->seq == 1);
    CHECK(b.queue_entry(1)->qty == 20);
    b.process(ob::Message::modify(3, 100000, 10), out);
    CHECK(b.queue_entry(3)->seq == 5);
    std::vector<OrderId> fifo;
    b.for_each_in_level(Side::Buy, 100000, [&](const ob::QueueEntry& e) {
        fifo.push_back(e.id);
        return true;
    });
    CHECK(fifo == std::vector<OrderId>{1, 2, 3});
    CHECK(b.level_qty(Side::Buy, 100000) == 60);
    CHECK(b.level_qty(Side::Buy, 100050) == 0);  // off tick
    CHECK_FALSE(b.queue_entry(99).has_value());
    CHECK(b.validate());

    Time clock = 7;
    BookView v(b, clock);
    CHECK(v.best_bid() == 100000);
    CHECK(v.best_ask() == 100200);
    CHECK(v.bid_qty(0) == 60);
    CHECK(v.bid_px(1) == kNoBid);
    CHECK(v.ask_qty(3) == 0);
    CHECK(v.mid_x2() == 200200);
    CHECK(v.depth_qty(Side::Buy, 5) == 60);
    CHECK(v.now() == 7);
}
