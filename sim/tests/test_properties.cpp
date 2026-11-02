// Property and fuzz tests (validation layers 3, 4 and 6 on generated data).
//
// A seeded generator produces a valid LOBSTER-style message stream together
// with its own top-of-book snapshots, computed by an independent aggregate
// model (not by ob::Book), so the parity check is not circular.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <random>
#include <vector>

#include "sim/run.hpp"
#include "sim/strategy/market_maker.hpp"
#include "sim/strategy/null.hpp"
#include "sim/strategy/scripted.hpp"

using namespace sim;
using ob::lobster::Row;
using ob::lobster::Snapshot;

namespace {

struct Generated {
    std::vector<Row> rows;
    std::vector<Snapshot> snaps;
};

// Valid stream: adds never cross, executions always hit the front order of
// the best level, partial cancels/deletes target random live orders.
Generated generate(std::uint64_t seed, std::size_t n, int levels = 5) {
    std::mt19937_64 rng(seed);
    struct O {
        std::uint64_t id;
        int side;  // 1 buy, -1 sell
        std::int64_t px;
        std::uint32_t qty;
    };
    std::vector<O> live;  // insertion order == FIFO order within a level
    std::uint64_t next_id = 1;
    std::int64_t t = 34200LL * kSecond;
    Generated g;

    auto best = [&](int side) {
        std::int64_t b = side == 1 ? 0 : INT64_MAX;
        for (const auto& o : live)
            if (o.side == side) b = side == 1 ? std::max(b, o.px) : std::min(b, o.px);
        return b;
    };
    auto snapshot = [&] {
        std::map<std::int64_t, std::int64_t> bids, asks;
        for (const auto& o : live) (o.side == 1 ? bids : asks)[o.px] += o.qty;
        Snapshot s;
        auto a = asks.begin();
        auto b = bids.rbegin();
        for (int i = 0; i < levels; ++i) {
            if (a != asks.end()) s.v.push_back(a->first), s.v.push_back(a->second), ++a;
            else s.v.push_back(ob::lobster::kEmptyAsk), s.v.push_back(0);
            if (b != bids.rend()) s.v.push_back(b->first), s.v.push_back(b->second), ++b;
            else s.v.push_back(ob::lobster::kEmptyBid), s.v.push_back(0);
        }
        return s;
    };
    auto emit = [&](int type, std::uint64_t id, std::uint32_t size, std::int64_t px, int dir) {
        t += 1 + static_cast<std::int64_t>(rng() % 2'000'000);
        Row r;
        r.time_ns = t;
        r.time = static_cast<double>(t) / 1e9;
        r.type = type;
        r.id = id;
        r.size = size;
        r.price = px;
        r.direction = dir;
        g.rows.push_back(r);
        g.snaps.push_back(snapshot());
    };

    while (g.rows.size() < n) {
        const auto roll = rng() % 100;
        if (live.size() < 6 || roll < 45) {
            const int side = rng() % 2 ? 1 : -1;
            const std::int64_t bb = best(1), ba = best(-1);
            const std::int64_t anchor = side == 1 ? (ba == INT64_MAX ? 100100 : ba) : (bb == 0 ? 99900 : bb);
            const std::int64_t px = anchor - side * 100 * static_cast<std::int64_t>(1 + rng() % 6);
            const auto q = static_cast<std::uint32_t>(1 + rng() % 300);
            live.push_back({next_id, side, px, q});
            emit(1, next_id++, q, px, side);
        } else if (roll < 65) {  // partial cancel
            auto& o = live[rng() % live.size()];
            if (o.qty < 2) continue;
            const auto q = static_cast<std::uint32_t>(1 + rng() % (o.qty - 1));
            o.qty -= q;
            emit(2, o.id, q, o.px, o.side);
        } else if (roll < 82) {  // delete
            const std::size_t i = rng() % live.size();
            const O o = live[i];
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
            emit(3, o.id, o.qty, o.px, o.side);
        } else if (roll < 98) {  // execute the front order at the best level
            const int side = rng() % 2 ? 1 : -1;
            const std::int64_t b = best(side);
            auto it = std::find_if(live.begin(), live.end(), [&](const O& o) { return o.side == side && o.px == b; });
            if (it == live.end()) continue;
            const auto q = static_cast<std::uint32_t>(1 + rng() % it->qty);
            const O o = *it;
            if (q == it->qty) live.erase(it);
            else it->qty -= q;
            emit(4, o.id, q, o.px, o.side);
        } else if (roll < 99) {
            emit(5, 0, static_cast<std::uint32_t>(1 + rng() % 100), best(1) == 0 ? 100000 : best(1), 1);
        } else if (rng() % 5 == 0) {
            emit(7, 0, 0, -1, -1);
        }
    }
    return g;
}

// Random scripted orders over the stream's time span.
json random_actions(std::uint64_t seed, const std::vector<Row>& rows, int n) {
    std::mt19937_64 rng(seed);
    const std::int64_t t0 = rows.front().time_ns, t1 = rows.back().time_ns;
    std::vector<std::int64_t> times;
    for (int i = 0; i < n; ++i) times.push_back(t0 + static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(t1 - t0)));
    std::sort(times.begin(), times.end());
    json acts = json::array();
    std::vector<int> order_actions;  // indices of actions that send an order
    for (int i = 0; i < n; ++i) {
        const auto t = times[static_cast<std::size_t>(i)];
        const auto roll = rng() % 10;
        if (!order_actions.empty() && roll < 3) {
            acts.push_back({{"t", t}, {"cancel", order_actions[rng() % order_actions.size()]}});
            continue;
        }
        const char* type = roll < 7 ? "limit" : roll < 9 ? "ioc" : "market";
        const std::int64_t off_tick = rng() % 50 == 0 ? 50 : 0;  // occasional invalid price
        acts.push_back({{"t", t},
                        {"side", rng() % 2 ? "buy" : "sell"},
                        {"price", 100000 + static_cast<std::int64_t>(rng() % 13) * 100 - 600 + off_tick},
                        {"qty", static_cast<int>(1 + rng() % 200)},
                        {"type", type}});
        order_actions.push_back(i);
    }
    return acts;
}

json base_config() {
    return json::parse(R"({
        "data": {"messages": "generated"},
        "latency_ns": {"feed": 2000, "proc": 1000, "to_exchange": 5000, "from_exchange": 5000,
                       "jitter": {"kind": "uniform", "max": 3000}},
        "risk": {"max_position": 1000, "max_order_size": 200, "max_open_orders": 20},
        "validate": {"book": true, "every": 1, "levels": 5},
        "strategy": {"name": "null"},
        "seed": 3
    })");
}

}  // namespace

TEST_CASE("fuzz: invariants and book parity hold under random strategy actions", "[sim][fuzz]") {
    for (std::uint64_t seed = 1; seed <= 12; ++seed) {
        const Generated g = generate(seed, 4000);
        MarketData data;
        build_market_data(g.rows, g.snaps, data);
        json j = base_config();
        j["strategy"] = {{"name", "scripted"}, {"actions", random_actions(seed * 31, g.rows, 400)}};
        const RunConfig cfg = parse_config(j);
        ScriptedStrategy strat(cfg.strategy_params);
        Simulator<ScriptedStrategy> sim(cfg.sim, data, strat);
        sim.run();
        INFO("seed " << seed << ": " << sim.stats().first_violation << " / " << sim.stats().replay.first_mismatch);
        CHECK(sim.stats().invariant_violations == 0);
        CHECK(sim.stats().replay.checkpoints == g.rows.size());
        CHECK(sim.stats().replay.depth_mismatches == 0);
        CHECK(sim.stats().replay.unknown_refs == 0);
        CHECK(sim.recorder().orders_sent > 0);
        CHECK(sim.portfolio().max_abs_position() <= cfg.sim.risk.max_position);
        // Position equals the signed sum of fills.
        std::int64_t pos = 0;
        for (const auto& f : sim.recorder().fills) pos += sign(f.side) * static_cast<std::int64_t>(f.qty);
        CHECK(pos == sim.portfolio().position());
        // No fill is ever stamped before its order arrived.
        for (const auto& f : sim.recorder().fills) CHECK(f.ts >= f.arrived);
        const Price m = sim.last_mid_x2();
        const auto& p = sim.portfolio();
        CHECK(2 * p.realized() + p.unrealized_x2(m) - 2 * p.fees() == p.equity_x2(m));
    }
}

TEST_CASE("no impact: a heavy market maker leaves the replayed book identical to a do-nothing run", "[sim][parity]") {
    const Generated g = generate(77, 6000);
    MarketData data;
    build_market_data(g.rows, g.snaps, data);
    json j = base_config();
    j["strategy"] = {{"name", "market_maker"}, {"quote_size", 100}, {"requote_min_ns", 0}, {"max_position", 1000}};
    const RunConfig mm_cfg = parse_config(j);
    MarketMakerStrategy mm(mm_cfg.strategy_params);
    Simulator<MarketMakerStrategy> a(mm_cfg.sim, data, mm);
    a.run();
    j["strategy"] = {{"name", "null"}};
    const RunConfig null_cfg = parse_config(j);
    NullStrategy nul;
    Simulator<NullStrategy> b(null_cfg.sim, data, nul);
    b.run();

    CHECK(a.recorder().fills.size() > 10);  // it really traded
    CHECK(a.stats().invariant_violations == 0);
    CHECK(a.stats().replay.depth_mismatches == 0);
    CHECK(b.stats().replay.depth_mismatches == 0);
    CHECK(a.book().depth(Side::Buy) == b.book().depth(Side::Buy));
    CHECK(a.book().depth(Side::Sell) == b.book().depth(Side::Sell));
    CHECK(a.book().last_seq() == b.book().last_seq());
    CHECK(a.recorder().mids == b.recorder().mids);
    CHECK(a.portfolio().max_abs_position() <= 1000);
}

TEST_CASE("determinism: identical output hash on repeat runs, different with another jitter seed", "[sim][determinism]") {
    const Generated g = generate(5, 5000);
    MarketData data;
    build_market_data(g.rows, g.snaps, data);
    json j = base_config();
    j["strategy"] = {{"name", "market_maker"}, {"quote_size", 100}};
    const RunResult r1 = run_simulation(parse_config(j), data, false);
    const RunResult r2 = run_simulation(parse_config(j), data, false);
    CHECK(r1.output_hash == r2.output_hash);
    CHECK(r1.summary.at("file_sha256") == r2.summary.at("file_sha256"));
    j["seed"] = 4;
    const RunResult r3 = run_simulation(parse_config(j), data, false);
    CHECK(r3.output_hash != r1.output_hash);
}

TEST_CASE("fees off vs on: net PnL changes by exactly the fees paid", "[sim][fees]") {
    const Generated g = generate(11, 5000);
    MarketData data;
    build_market_data(g.rows, g.snaps, data);
    json j = base_config();
    j["strategy"] = {{"name", "scripted"}, {"actions", random_actions(99, g.rows, 300)}};
    const RunResult on = run_simulation(parse_config(j), data, false);
    j["fees_per_share"] = {{"taker", 0.0}, {"maker_rebate", 0.0}};
    const RunResult off = run_simulation(parse_config(j), data, false);
    CHECK(on.summary.at("fills") == off.summary.at("fills"));
    CHECK(off.summary.at("fees").get<double>() == 0.0);
    const double diff = off.summary.at("net_pnl").get<double>() - on.summary.at("net_pnl").get<double>();
    CHECK(std::abs(diff - on.summary.at("fees").get<double>()) < 1e-9);
    CHECK(on.summary.at("gross_pnl") == off.summary.at("gross_pnl"));
}

TEST_CASE("LOBSTER fixture: seeded replay stays in parity while a strategy trades", "[sim][lobster]") {
    MarketData data;
    std::string err;
    REQUIRE(load_market_data(std::string(OB_TEST_DATA_DIR) + "/TEST_message_2.csv",
                             std::string(OB_TEST_DATA_DIR) + "/TEST_orderbook_2.csv", data, &err));
    json j = base_config();
    j["validate"] = {{"book", true}, {"every", 1}, {"levels", 2}};
    j["latency_ns"] = {{"feed", 0}, {"proc", 0}, {"to_exchange", 0}, {"from_exchange", 0}};
    j["strategy"] = {{"name", "market_maker"}, {"quote_size", 10}, {"requote_min_ns", 0}};
    const RunConfig cfg = parse_config(j);
    MarketMakerStrategy mm(cfg.strategy_params);
    Simulator<MarketMakerStrategy> s(cfg.sim, data, mm);
    s.run();
    CHECK(s.stats().replay.seeded_levels == 2);
    CHECK(s.stats().replay.checkpoints == 8);
    CHECK(s.stats().replay.depth_mismatches == 0);
    CHECK(s.stats().invariant_violations == 0);
    CHECK(s.recorder().orders_sent > 0);
}
