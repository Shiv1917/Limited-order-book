// Validation layer 1: hand-built scenarios (sim/tests/scenarios/*.scn).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "sim/run.hpp"
#include "sim/strategy/scripted.hpp"

using namespace sim;
namespace fs = std::filesystem;

namespace {

struct Scenario {
    std::string name;
    json config;
    std::vector<ob::lobster::Row> rows;
    std::vector<std::string> expected;
    std::map<std::string, std::int64_t> checks;
};

Scenario load(const fs::path& p) {
    Scenario s;
    s.name = p.filename().string();
    std::ifstream f(p);
    std::string line, section, cfg;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') {
            section = line;
            continue;
        }
        if (section == "[config]") cfg += line + "\n";
        else if (section == "[messages]") {
            ob::lobster::Row r;
            REQUIRE(ob::lobster::parse_row(line.c_str(), r));
            s.rows.push_back(r);
        } else if (section == "[expected]") s.expected.push_back(line);
        else if (section == "[checks]") {
            const auto eq = line.find('=');
            s.checks[line.substr(0, eq)] = std::stoll(line.substr(eq + 1));
        }
    }
    s.config = json::parse(cfg);
    return s;
}

json defaults() {
    return json::parse(R"({
        "data": {"messages": "in-memory"},
        "latency_ns": {"feed": 0, "proc": 0, "to_exchange": 0, "from_exchange": 0},
        "fees_per_share": {"taker": 0.0030, "maker_rebate": 0.0020},
        "risk": {"max_position": 100000, "max_order_size": 100000, "max_open_orders": 100},
        "strategy": {"name": "scripted", "actions": []},
        "seed": 1
    })");
}

std::string row(const OrderLogRow& r) {
    const std::string ev = r.event;
    std::ostringstream o;
    o << fmt_time(r.ts) << ',' << (r.id - kStrategyIdBase) << ',' << ev << ','
      << (r.side == Side::Buy ? "buy" : "sell") << ',' << r.price << ',' << r.qty << ',' << r.detail;
    return o.str();
}

}  // namespace

TEST_CASE("hand-built fill scenarios", "[sim][scenario]") {
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(SIM_SCENARIO_DIR))
        if (e.path().extension() == ".scn") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    REQUIRE(files.size() >= 15);

    for (const auto& path : files) {
        const Scenario sc = load(path);
        DYNAMIC_SECTION(sc.name) {
            json j = defaults();
            j.merge_patch(sc.config);
            j["strategy"]["name"] = "scripted";
            const RunConfig cfg = parse_config(j);
            MarketData data;
            build_market_data(sc.rows, {}, data);
            ScriptedStrategy strat(cfg.strategy_params);
            Simulator<ScriptedStrategy> sim(cfg.sim, data, strat);
            sim.run();

            std::vector<std::string> got;
            for (const auto& r : sim.recorder().orders) {
                const std::string ev = r.event;
                if (ev == "sent" || ev == "cancel_sent" || ev == "arrived") continue;
                got.push_back(row(r));
            }
            INFO("got:\n" << [&] {
                std::string s;
                for (const auto& g : got) s += "  " + g + "\n";
                return s;
            }());
            CHECK(got == sc.expected);
            CHECK(sim.stats().invariant_violations == 0);
            // Every exchange event reaches the strategy exactly once.
            std::size_t exch_events = got.size();
            CHECK(strat.received.size() == exch_events);
            for (const auto& [k, v] : sc.checks) {
                INFO("check " << k);
                if (k == "position") CHECK(sim.portfolio().position() == v);
                else if (k == "fees") CHECK(sim.portfolio().fees() == v);
                else if (k == "cash") CHECK(sim.portfolio().cash() == v);
                else FAIL("unknown check key " << k);
            }
        }
    }
}
