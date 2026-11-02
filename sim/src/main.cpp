// ob-sim: event-driven backtester over the limit order book.
//
//   ob-sim run   --config CFG.json [--out DIR]
//   ob-sim sweep --config CFG.json --param latency_ns.to_exchange --values 0,10000,50000 [--out DIR]
//   ob-sim check --config CFG.json      (book parity + invariants, no strategy)
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "sim/run.hpp"

using namespace sim;

namespace {

int usage() {
    std::cerr << "usage:\n"
                 "  ob-sim run   --config CFG.json [--out DIR]\n"
                 "  ob-sim sweep --config CFG.json --param a.b.c --values v1,v2,... [--out DIR]\n"
                 "  ob-sim check --config CFG.json\n";
    return 2;
}

struct Args {
    std::string config, out, param, values;
};

bool parse_args(int argc, char** argv, Args& a) {
    for (int i = 2; i < argc; ++i) {
        auto is = [&](const char* n) { return std::strcmp(argv[i], n) == 0 && i + 1 < argc; };
        if (is("--config")) a.config = argv[++i];
        else if (is("--out")) a.out = argv[++i];
        else if (is("--param")) a.param = argv[++i];
        else if (is("--values")) a.values = argv[++i];
        else {
            std::cerr << "unknown option " << argv[i] << "\n";
            return false;
        }
    }
    if (a.config.empty()) std::cerr << "--config is required\n";
    return !a.config.empty();
}

bool load_data(const RunConfig& cfg, MarketData& data) {
    std::string err;
    if (!load_market_data(cfg.messages, cfg.book, data, &err)) {
        std::cerr << err << "\n";
        return false;
    }
    return true;
}

void print_summary(const json& s) {
    auto d = [&](const char* k) { return s.at(k).get<double>(); };
    std::printf("  net PnL $%.2f (gross $%.2f, spread capture $%.2f, inventory $%.2f, fees $%.2f)\n", d("net_pnl"),
                d("gross_pnl"), d("spread_capture_pnl"), d("inventory_pnl"), d("fees"));
    std::printf("  fills %llu (%llu maker sh, %llu taker sh), fill rate %.3f, avg queue wait %.3f s\n",
                s.at("fills").get<unsigned long long>(), s.at("maker_shares").get<unsigned long long>(),
                s.at("taker_shares").get<unsigned long long>(), d("fill_rate"), d("avg_queue_wait_s"));
    std::printf("  max |inventory| %lld, end inventory %lld, adverse selection (maker) 1s %.2f ticks, 10s %.2f ticks\n",
                s.at("max_abs_inventory").get<long long>(), s.at("end_inventory").get<long long>(),
                s.at("adverse_selection").at("maker_1s_ticks").get<double>(),
                s.at("adverse_selection").at("maker_10s_ticks").get<double>());
    std::printf("  %llu md + %llu sim events, %.0f events/s, invariant violations %llu\n",
                s.at("md_events").get<unsigned long long>(), s.at("sim_events").get<unsigned long long>(),
                d("events_per_second"), s.at("invariant_violations").get<unsigned long long>());
    std::printf("  output sha256 %s\n", s.at("output_sha256").get<std::string>().c_str());
}

int cmd_run(const Args& a) {
    RunConfig cfg = parse_config(load_json_file(a.config));
    if (!a.out.empty()) {
        cfg.out_dir = a.out;
        cfg.raw["out_dir"] = a.out;
    }
    MarketData data;
    if (!load_data(cfg, data)) return 1;
    const RunResult r = run_simulation(cfg, data);
    std::cout << "ob-sim run: " << cfg.strategy << " on " << cfg.messages << " -> " << cfg.out_dir << "\n";
    print_summary(r.summary);
    return r.stats.invariant_violations ? 1 : 0;
}

// Sets a dotted path ("latency_ns.to_exchange") in a JSON object.
void set_path(json& j, const std::string& path, const json& v) {
    json* cur = &j;
    std::stringstream ss(path);
    std::string key;
    std::vector<std::string> keys;
    while (std::getline(ss, key, '.')) keys.push_back(key);
    for (std::size_t i = 0; i + 1 < keys.size(); ++i) cur = &(*cur)[keys[i]];
    (*cur)[keys.back()] = v;
}

int cmd_sweep(const Args& a) {
    if (a.param.empty() || a.values.empty()) return usage();
    const json base = load_json_file(a.config);
    RunConfig base_cfg = parse_config(base);
    const std::string out_root = a.out.empty() ? base_cfg.out_dir : a.out;
    MarketData data;
    if (!load_data(base_cfg, data)) return 1;

    std::vector<std::string> values;
    std::stringstream ss(a.values);
    for (std::string v; std::getline(ss, v, ',');) values.push_back(v);

    std::string table = a.param +
                        ",net_pnl,gross_pnl,spread_capture_pnl,inventory_pnl,fees,fills,maker_shares,taker_shares,"
                        "fill_rate,avg_queue_wait_s,adverse_maker_1s_ticks,adverse_maker_10s_ticks,max_abs_inventory,"
                        "end_inventory,invariant_violations\n";
    std::printf("%-14s %10s %8s %8s %10s %10s %10s\n", a.param.c_str(), "net_pnl", "fills", "fill_rt", "q_wait_s",
                "adv1s_tk", "adv10s_tk");
    int rc = 0;
    for (const auto& v : values) {
        json j = base;
        json val;
        try {
            val = json::parse(v);
        } catch (...) {
            val = v;  // plain string
        }
        set_path(j, a.param, val);
        const std::string leaf = a.param.substr(a.param.rfind('.') + 1);
        j["out_dir"] = (std::filesystem::path(out_root) / (leaf + "=" + v)).string();
        const RunConfig cfg = parse_config(j);
        const RunResult r = run_simulation(cfg, data);
        const json& s = r.summary;
        const auto& adv = s.at("adverse_selection");
        char line[512];
        std::snprintf(line, sizeof line, "%s,%.4f,%.4f,%.4f,%.4f,%.4f,%llu,%llu,%llu,%.6f,%.6f,%.4f,%.4f,%lld,%lld,%llu\n",
                      v.c_str(), s.at("net_pnl").get<double>(), s.at("gross_pnl").get<double>(),
                      s.at("spread_capture_pnl").get<double>(), s.at("inventory_pnl").get<double>(),
                      s.at("fees").get<double>(), s.at("fills").get<unsigned long long>(),
                      s.at("maker_shares").get<unsigned long long>(), s.at("taker_shares").get<unsigned long long>(),
                      s.at("fill_rate").get<double>(), s.at("avg_queue_wait_s").get<double>(),
                      adv.at("maker_1s_ticks").get<double>(), adv.at("maker_10s_ticks").get<double>(),
                      s.at("max_abs_inventory").get<long long>(), s.at("end_inventory").get<long long>(),
                      s.at("invariant_violations").get<unsigned long long>());
        table += line;
        std::printf("%-14s %10.2f %8llu %8.4f %10.3f %10.3f %10.3f\n", v.c_str(), s.at("net_pnl").get<double>(),
                    s.at("fills").get<unsigned long long>(), s.at("fill_rate").get<double>(),
                    s.at("avg_queue_wait_s").get<double>(), adv.at("maker_1s_ticks").get<double>(),
                    adv.at("maker_10s_ticks").get<double>());
        if (r.stats.invariant_violations) rc = 1;
    }
    std::filesystem::create_directories(out_root);
    std::ofstream(std::filesystem::path(out_root) / "sweep.csv") << table;
    std::cout << "wrote " << (std::filesystem::path(out_root) / "sweep.csv").string() << "\n";
    return rc;
}

int cmd_check(const Args& a) {
    json j = load_json_file(a.config);
    j["strategy"] = {{"name", "null"}};
    j["validate"] = {{"book", true}, {"every", 1}, {"levels", j.value("validate", json::object()).value("levels", 10)}};
    const RunConfig cfg = parse_config(j);
    if (cfg.book.empty()) {
        std::cerr << "check needs data.book (the orderbook snapshot file)\n";
        return 1;
    }
    MarketData data;
    if (!load_data(cfg, data)) return 1;
    const RunResult r = run_simulation(cfg, data, /*write_files=*/false);
    const auto& rs = r.stats.replay;
    std::cout << "ob-sim check: " << rs.messages << " rows, " << rs.checkpoints << " checkpoints\n"
              << "  seeded levels " << rs.seeded_levels << ", unknown refs " << rs.unknown_refs << ", rejects " << rs.rejects
              << "\n  top-of-book mismatches " << rs.l1_mismatches << ", depth(" << cfg.sim.validate_levels
              << ") mismatches " << rs.depth_mismatches << "\n"
              << "  invariant violations " << r.stats.invariant_violations << "\n";
    if (!rs.first_mismatch.empty()) std::cout << "  first mismatch at " << rs.first_mismatch << "\n";
    return (rs.depth_mismatches || r.stats.invariant_violations) ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    Args a;
    if (!parse_args(argc, argv, a)) return usage();
    const std::string cmd = argv[1];
    try {
        if (cmd == "run") return cmd_run(a);
        if (cmd == "sweep") return cmd_sweep(a);
        if (cmd == "check") return cmd_check(a);
    } catch (const std::exception& e) {
        std::cerr << "ob-sim: " << e.what() << "\n";
        return 1;
    }
    return usage();
}
