#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "market_maker.hpp"
#include "synthetic.hpp"

using namespace ob;
using namespace ob::strategy;

TEST_CASE("market maker improves a wide spread and gets filled", "[mm]") {
    MessageStream s;
    s.band = PriceBand{0, 1, 1000};
    s.msgs = {Message::add(1, Side::Sell, 110, 100), Message::add(2, Side::Buy, 100, 100),
              Message::market(3, Side::Sell, 50)};
    MMConfig cfg;
    cfg.quote_size = 100;
    const auto r = run_backtest(s, cfg);
    REQUIRE(r.fills == 1);
    REQUIRE(r.bought == 50);
    REQUIRE(r.fill_log[0].price == 101);  // stepped inside the 10-tick spread
    REQUIRE(r.cash == Catch::Approx(-50 * 101 / 1e4));
    REQUIRE(r.inventory == 50);
}

TEST_CASE("market maker respects inventory limits on a long run", "[mm]") {
    SynthConfig sc;
    sc.seed = 11;
    SyntheticFlow gen(sc);
    MessageStream s;
    s.band = sc.band;
    s.msgs = gen.generate(50'000);
    for (std::size_t latency : {0u, 5u}) {
        MMConfig cfg;
        cfg.max_inventory = 300;
        cfg.latency_msgs = latency;
        const auto r = run_backtest(s, cfg);
        REQUIRE(r.fills > 0);
        REQUIRE(static_cast<std::int64_t>(r.bought) - static_cast<std::int64_t>(r.sold) == r.inventory);
        if (latency == 0) {
            REQUIRE(r.max_abs_inventory <= cfg.max_inventory);
            REQUIRE(r.taker_fills == 0);  // post-only with instant reaction
        }
    }
}
