// Config parsing, strategy selection, and output writing for one run.
#pragma once

#include <string>

#include "sim/params.hpp"
#include "sim/simulator.hpp"

namespace sim {

struct RunConfig {
    json raw;  // the full config, echoed into manifest.json
    SimConfig sim;
    std::string messages, book, out_dir;
    std::string strategy;  // "null", "scripted", "probe", "market_maker", "imbalance"
    json strategy_params;
};

// Throws std::runtime_error with a readable message on a bad config.
RunConfig parse_config(const json& j);
json load_json_file(const std::string& path);

struct RunResult {
    json summary;
    RunStats stats;
    std::string output_hash;  // SHA-256 over orders.csv, fills.csv, pnl.csv
};

// Runs the configured strategy over `data` and writes all output files into
// cfg.out_dir (unless write_files is false, then only the summary is built).
RunResult run_simulation(const RunConfig& cfg, const MarketData& data, bool write_files = true);

// Output builders (exposed for tests).
std::string orders_csv(const Recorder& rec);
std::string fills_csv(const Recorder& rec);
std::string pnl_csv(const Recorder& rec);
json build_summary(const SimCore& core, const std::string& output_hash);
json build_manifest(const RunConfig& cfg, const MarketData& data);

}  // namespace sim
