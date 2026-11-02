// ob_mm_backtest: run the market maker against a replayed .obm stream.
//   ob_mm_backtest data/aapl.obm [--size 100] [--max-inv 500] [--skew 1.0] [--half-spread 0]
//                  [--latency 0] [--rebate 0.002] [--no-improve] [--from HH:MM:SS] [--until HH:MM:SS]
//                  [--equity-csv f] [--fills-csv f]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>

#include "market_maker.hpp"
#include "msgfile.hpp"

using namespace ob;

static std::uint64_t hms(const char* s) {
    unsigned h = 0, m = 0, sec = 0;
    std::sscanf(s, "%u:%u:%u", &h, &m, &sec);
    return (std::uint64_t{h} * 3600 + m * 60 + sec) * 1'000'000'000ull;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: ob_mm_backtest STREAM.obm [options]\n";
        return 2;
    }
    strategy::MMConfig cfg;
    std::string equity_csv, fills_csv;
    for (int i = 2; i < argc; ++i) {
        auto arg = [&](const char* n) { return std::strcmp(argv[i], n) == 0 && i + 1 < argc; };
        if (arg("--size")) cfg.quote_size = static_cast<Qty>(std::atoi(argv[++i]));
        else if (arg("--max-inv")) cfg.max_inventory = std::atoll(argv[++i]);
        else if (arg("--skew")) cfg.skew_ticks_per_lot = std::atof(argv[++i]);
        else if (arg("--half-spread")) cfg.min_half_spread_ticks = std::atoi(argv[++i]);
        else if (arg("--latency")) cfg.latency_msgs = std::strtoull(argv[++i], nullptr, 10);
        else if (arg("--rebate")) cfg.rebate_per_share = std::atof(argv[++i]);
        else if (arg("--markout")) cfg.markout_horizon = std::strtoull(argv[++i], nullptr, 10);
        else if (arg("--from")) cfg.quote_from_ns = hms(argv[++i]);
        else if (arg("--until")) cfg.quote_until_ns = hms(argv[++i]);
        else if (std::strcmp(argv[i], "--no-improve") == 0) cfg.improve_wide_spread = false;
        else if (arg("--equity-csv")) equity_csv = argv[++i];
        else if (arg("--fills-csv")) fills_csv = argv[++i];
        else {
            std::cerr << "unknown option " << argv[i] << "\n";
            return 2;
        }
    }
    MessageStream s;
    std::string err;
    if (!load_stream(argv[1], s, &err)) {
        std::cerr << err << "\n";
        return 1;
    }
    const auto r = strategy::run_backtest(s, cfg);
    std::cout << "Market-making backtest on " << s.symbol << " (" << s.msgs.size() << " messages)\n"
              << "config: size=" << cfg.quote_size << " max_inv=" << cfg.max_inventory << " skew=" << cfg.skew_ticks_per_lot
              << " half_spread=" << cfg.min_half_spread_ticks << " improve=" << cfg.improve_wide_spread
              << " latency_msgs=" << cfg.latency_msgs << " rebate=" << cfg.rebate_per_share << "\n"
              << r.summary();
    if (!equity_csv.empty()) {
        std::ofstream f(equity_csv);
        f << "t,pnl\n";
        for (const auto& [t, p] : r.equity) f << t << "," << p << "\n";
    }
    if (!fills_csv.empty()) {
        std::ofstream f(fills_csv);
        f << "msg_index,side,price,qty,mid\n";
        for (const auto& fl : r.fill_log)
            f << fl.msg_index << "," << (fl.side == Side::Buy ? "B" : "S") << "," << fl.price << "," << fl.qty << ","
              << fl.mid_at_fill << "\n";
    }
    return 0;
}
