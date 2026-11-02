#include "sim/run.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "sim/sha256.hpp"
#include "sim/strategy/imbalance.hpp"
#include "sim/strategy/market_maker.hpp"
#include "sim/strategy/null.hpp"
#include "sim/strategy/probe.hpp"
#include "sim/strategy/scripted.hpp"

#ifndef OB_BUILD_FLAGS
#define OB_BUILD_FLAGS "unknown"
#endif
#ifndef OB_SOURCE_DIR
#define OB_SOURCE_DIR "."
#endif

namespace sim {
namespace {

// Dollars per share (e.g. 0.0030) -> integer price units per share. Values
// finer than $0.0001 are refused rather than silently rounded.
std::int64_t dollars_to_units(double d, const char* what) {
    const double u = d * static_cast<double>(kUnitsPerDollar);
    const auto r = static_cast<std::int64_t>(std::llround(u));
    if (std::fabs(u - static_cast<double>(r)) > 1e-6)
        throw std::runtime_error(std::string(what) + " must be a multiple of $0.0001 per share");
    return r;
}

std::string side_str(Side s) { return s == Side::Buy ? "buy" : "sell"; }

std::string run_cmd(const std::string& cmd) {
    std::string out;
    if (std::FILE* p = ::popen(cmd.c_str(), "r")) {
        char buf[256];
        while (std::fgets(buf, sizeof buf, p)) out += buf;
        ::pclose(p);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

void write_file(const std::filesystem::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + p.string());
    f << s;
}

}  // namespace

json load_json_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open config " + path);
    try {
        return json::parse(f, nullptr, true, /*ignore_comments=*/true);
    } catch (const json::exception& e) {
        throw std::runtime_error(path + ": " + e.what());
    }
}

RunConfig parse_config(const json& j) {
    RunConfig c;
    c.raw = j;
    try {
        const json& d = j.at("data");
        c.messages = d.at("messages").get<std::string>();
        c.book = d.value("book", std::string());

        if (j.contains("latency_ns")) {
            const json& l = j.at("latency_ns");
            auto& L = c.sim.latency;
            L.feed_ns = l.value("feed", L.feed_ns);
            L.proc_ns = l.value("proc", L.proc_ns);
            L.to_exchange_ns = l.value("to_exchange", L.to_exchange_ns);
            L.from_exchange_ns = l.value("from_exchange", L.from_exchange_ns);
            if (l.contains("jitter")) {
                const json& jt = l.at("jitter");
                const std::string kind = jt.value("kind", std::string("none"));
                if (kind == "uniform") L.jitter_max_ns = jt.at("max").get<Time>();
                else if (kind != "none") throw std::runtime_error("latency_ns.jitter.kind must be none or uniform");
            }
            if (L.feed_ns < 0 || L.proc_ns < 0 || L.to_exchange_ns < 0 || L.from_exchange_ns < 0 || L.jitter_max_ns < 0)
                throw std::runtime_error("latencies must be >= 0");
        }
        if (j.contains("fees_per_share")) {
            const json& f = j.at("fees_per_share");
            c.sim.taker_fee = dollars_to_units(f.value("taker", 0.0030), "fees_per_share.taker");
            c.sim.maker_rebate = dollars_to_units(f.value("maker_rebate", 0.0020), "fees_per_share.maker_rebate");
        }
        if (j.contains("risk")) {
            const json& r = j.at("risk");
            c.sim.risk.max_position = r.value("max_position", c.sim.risk.max_position);
            c.sim.risk.max_order_size = r.value("max_order_size", c.sim.risk.max_order_size);
            c.sim.risk.max_open_orders = r.value("max_open_orders", c.sim.risk.max_open_orders);
        }
        if (j.contains("fill_model")) {
            const json& f = j.at("fill_model");
            c.sim.fill.trade_through = f.value("trade_through", c.sim.fill.trade_through);
            c.sim.fill.own_taken = f.value("own_taken", c.sim.fill.own_taken);
            const double share = f.value("exec_volume_share", 1.0);
            if (share < 0 || share > 1) throw std::runtime_error("fill_model.exec_volume_share must be in [0, 1]");
            c.sim.fill.exec_volume_pct = static_cast<std::uint32_t>(std::llround(share * 100));
        }
        if (j.contains("validate")) {
            const json& v = j.at("validate");
            c.sim.validate_book = v.value("book", false);
            c.sim.validate_every = std::max<std::size_t>(1, v.value("every", std::size_t{1}));
            c.sim.validate_levels = v.value("levels", std::size_t{10});
        }
        c.sim.snapshot_ns = j.value("snapshot_ns", kSecond);
        if (c.sim.snapshot_ns <= 0) throw std::runtime_error("snapshot_ns must be > 0");
        c.sim.seed = j.value("seed", std::uint64_t{42});
        c.out_dir = j.value("out_dir", std::string("runs/out"));
        const json& s = j.at("strategy");
        c.strategy = s.at("name").get<std::string>();
        c.strategy_params = s;
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("config: ") + e.what());
    }
    return c;
}

// ---- output files ----------------------------------------------------------------

std::string orders_csv(const Recorder& rec) {
    std::string s = "ts,order_id,event,side,price,qty,detail\n";
    for (const auto& r : rec.orders) {
        s += fmt_time(r.ts) + ',' + std::to_string(r.id) + ',' + r.event + ',';
        // cancel_sent rows carry no side/price/qty
        if (std::string_view(r.event) == "cancel_sent") s += ",,";
        else s += side_str(r.side) + ',' + fmt_dollars(r.price) + ',' + std::to_string(r.qty);
        s += ',';
        s += r.detail;
        s += '\n';
    }
    return s;
}

std::string fills_csv(const Recorder& rec) {
    std::string s = "ts,order_id,side,price,qty,liquidity,fee,queue_ahead,arrived,mid,mid_1s,mid_10s\n";
    for (const auto& f : rec.fills) {
        s += fmt_time(f.ts) + ',' + std::to_string(f.id) + ',' + side_str(f.side) + ',' + fmt_dollars(f.price) + ',' +
             std::to_string(f.qty) + ',' + (f.maker ? "maker" : "taker") + ',' + fmt_dollars(f.fee) + ',' +
             std::to_string(f.queue_ahead) + ',' + fmt_time(f.arrived) + ',' + fmt_mid_x2(f.mid_x2) + ',' +
             fmt_mid_x2(rec.mid_at(f.ts + kSecond)) + ',' + fmt_mid_x2(rec.mid_at(f.ts + 10 * kSecond)) + '\n';
    }
    return s;
}

std::string pnl_csv(const Recorder& rec) {
    std::string s = "ts,cash,inventory,mid,realized_pnl,unrealized_pnl,fees,equity\n";
    for (const auto& p : rec.pnl) {
        s += fmt_time(p.ts) + ',' + fmt_dollars(p.cash) + ',' + std::to_string(p.position) + ',' +
             fmt_mid_x2(p.mid_x2) + ',' + fmt_dollars(p.realized) + ',' + fmt_mid_x2(p.unrealized_x2) + ',' +
             fmt_dollars(p.fees) + ',' + fmt_mid_x2(p.equity_x2) + '\n';
    }
    return s;
}

json build_summary(const SimCore& core, const std::string& output_hash) {
    const Recorder& rec = core.recorder();
    const Portfolio& pf = core.portfolio();
    const RunStats& st = core.stats();
    const Price mid_x2 = core.last_mid_x2();
    const auto dollars_x2 = [](auto x2v) { const auto x2 = static_cast<long double>(x2v); return static_cast<double>(x2 / 20000.0L); };
    const double tick = static_cast<double>(core.book().band().tick);

    // All PnL in x2 price units until the final conversion.
    const std::int64_t equity_x2 = pf.equity_x2(mid_x2);
    const std::int64_t gross_x2 = equity_x2 + 2 * pf.fees();
    long double spread_x2 = 0, adv1_x2 = 0, adv10_x2 = 0, adv1_maker_x2 = 0, adv10_maker_x2 = 0;
    std::uint64_t shares = 0, maker_shares = 0;
    for (const auto& f : rec.fills) {
        if (f.mid_x2 == 0) continue;  // no two-sided book at the fill
        const std::int64_t s = sign(f.side), q = f.qty;
        const std::int64_t edge = s * (f.mid_x2 - 2 * f.price);
        const std::int64_t m1 = rec.mid_at(f.ts + kSecond), m10 = rec.mid_at(f.ts + 10 * kSecond);
        // adverse = edge - realized_spread = s * (mid_fill - mid_later)
        const long double a1 = static_cast<long double>(s * (f.mid_x2 - m1) * q);
        const long double a10 = static_cast<long double>(s * (f.mid_x2 - m10) * q);
        spread_x2 += static_cast<long double>(edge * q);
        adv1_x2 += a1, adv10_x2 += a10;
        shares += f.qty;
        if (f.maker) adv1_maker_x2 += a1, adv10_maker_x2 += a10, maker_shares += f.qty;
    }
    const auto per_share = [&](long double x2, std::uint64_t n) { return n ? dollars_x2(x2) / static_cast<double>(n) : 0.0; };
    const auto per_share_ticks = [&](long double x2, std::uint64_t n) {
        return n ? static_cast<double>(x2 / 2.0L) / tick / static_cast<double>(n) : 0.0;
    };

    json s;
    s["net_pnl"] = dollars_x2(equity_x2);
    s["gross_pnl"] = dollars_x2(gross_x2);
    s["spread_capture_pnl"] = dollars_x2(spread_x2);
    s["inventory_pnl"] = dollars_x2(static_cast<long double>(gross_x2) - spread_x2);
    s["fees"] = static_cast<double>(pf.fees()) / static_cast<double>(kUnitsPerDollar);
    s["realized_pnl"] = static_cast<double>(pf.realized()) / static_cast<double>(kUnitsPerDollar);
    s["unrealized_pnl"] = dollars_x2(pf.unrealized_x2(mid_x2));
    s["fills"] = rec.fills.size();
    s["filled_shares"] = pf.turnover();
    s["maker_shares"] = rec.passive_filled_shares;
    s["taker_shares"] = rec.taker_filled_shares;
    s["orders_sent"] = rec.orders_sent;
    s["cancels_sent"] = rec.cancels_sent;
    s["rejects"] = rec.rejects;
    s["cancel_rejects_too_late_or_unknown"] = rec.cancel_rejects;
    s["passive_orders"] = rec.passive_orders;
    s["passive_shares"] = rec.passive_shares;
    s["fill_rate"] = rec.passive_shares ? static_cast<double>(rec.passive_filled_shares) / static_cast<double>(rec.passive_shares) : 0.0;
    s["avg_queue_wait_s"] = rec.queue_waits ? static_cast<double>(rec.queue_wait_sum_ns / static_cast<long double>(rec.queue_waits)) / 1e9 : 0.0;
    s["cancel_to_fill_ratio"] = rec.fills.empty() ? 0.0 : static_cast<double>(rec.cancels_sent) / static_cast<double>(rec.fills.size());
    s["max_abs_inventory"] = pf.max_abs_position();
    s["end_inventory"] = pf.position();
    s["turnover_shares"] = pf.turnover();
    s["adverse_selection"] = {
        {"all_1s_per_share", per_share(adv1_x2, shares)},
        {"all_10s_per_share", per_share(adv10_x2, shares)},
        {"maker_1s_per_share", per_share(adv1_maker_x2, maker_shares)},
        {"maker_10s_per_share", per_share(adv10_maker_x2, maker_shares)},
        {"maker_1s_ticks", per_share_ticks(adv1_maker_x2, maker_shares)},
        {"maker_10s_ticks", per_share_ticks(adv10_maker_x2, maker_shares)},
        {"edge_per_share", per_share(spread_x2, shares)},
    };
    s["md_events"] = st.md_events;
    s["sim_events"] = st.sim_events;
    s["dropped_at_end"] = st.dropped_at_end;
    s["ignored_after_close"] = st.ignored_after_close;
    s["hidden_volume"] = st.hidden_volume;
    s["halts_and_crosses"] = st.halts;
    s["unknown_exec_refs"] = st.unknown_exec_refs;
    s["invariant_violations"] = st.invariant_violations;
    if (!st.first_violation.empty()) s["first_violation"] = st.first_violation;
    s["book_parity"] = {
        {"checkpoints", st.replay.checkpoints},
        {"l1_mismatches", st.replay.l1_mismatches},
        {"depth_mismatches", st.replay.depth_mismatches},
        {"first_mismatch", st.replay.first_mismatch},
        {"seeded_levels", st.replay.seeded_levels},
        {"unknown_refs", st.replay.unknown_refs},
    };
    s["session"] = {{"first_ts", fmt_time(st.first_ts)}, {"last_ts", fmt_time(st.last_ts)}};
    // Throughput: the only wall-clock figure; never part of the hashed files.
    s["wall_seconds"] = st.wall_seconds;
    s["events_per_second"] = st.wall_seconds > 0 ? static_cast<double>(st.md_events + st.sim_events) / st.wall_seconds : 0.0;
    s["output_sha256"] = output_hash;
    return s;
}

json build_manifest(const RunConfig& cfg, const MarketData& data) {
    json m;
    const std::string src = OB_SOURCE_DIR;
    m["git_commit"] = run_cmd("git -C '" + src + "' rev-parse HEAD 2>/dev/null");
    m["git_dirty"] = !run_cmd("git -C '" + src + "' status --porcelain 2>/dev/null").empty();
#if defined(__clang__)
    m["compiler"] = std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    m["compiler"] = std::string("gcc ") + __VERSION__;
#else
    m["compiler"] = "unknown";
#endif
    m["flags"] = OB_BUILD_FLAGS;
    m["inputs"] = {{"messages", {{"path", data.messages_path}, {"sha256", sha256_file(data.messages_path)}}}};
    if (!data.book_path.empty())
        m["inputs"]["book"] = {{"path", data.book_path}, {"sha256", sha256_file(data.book_path)}};
    m["rows"] = data.rows.size();
    m["seed"] = cfg.sim.seed;
    m["config"] = cfg.raw;
    return m;
}

namespace {

template <class Strategy>
RunResult run_with(Strategy& strat, const RunConfig& cfg, const MarketData& data, bool write_files) {
    Simulator<Strategy> s(cfg.sim, data, strat);
    s.run();
    const std::string orders = orders_csv(s.recorder()), fills = fills_csv(s.recorder()), pnl = pnl_csv(s.recorder());
    Sha256 h;  // one digest over the three result files, each prefixed by its name
    const std::pair<const char*, const std::string*> parts[] = {{"orders.csv", &orders}, {"fills.csv", &fills}, {"pnl.csv", &pnl}};
    for (const auto& [name, body] : parts) {
        h.update(name, std::strlen(name) + 1);  // include the NUL as a separator
        h.update(body->data(), body->size());
    }
    RunResult r;
    r.output_hash = h.hex();
    r.summary = build_summary(s, r.output_hash);
    r.summary["file_sha256"] = {{"orders.csv", sha256_hex(orders)}, {"fills.csv", sha256_hex(fills)}, {"pnl.csv", sha256_hex(pnl)}};
    r.summary["strategy"] = cfg.strategy;
    r.stats = s.stats();
    if (write_files) {
        namespace fs = std::filesystem;
        const fs::path dir(cfg.out_dir);
        fs::create_directories(dir);
        write_file(dir / "orders.csv", orders);
        write_file(dir / "fills.csv", fills);
        write_file(dir / "pnl.csv", pnl);
        write_file(dir / "summary.json", r.summary.dump(2) + "\n");
        write_file(dir / "manifest.json", build_manifest(cfg, data).dump(2) + "\n");
    }
    return r;
}

}  // namespace

RunResult run_simulation(const RunConfig& cfg, const MarketData& data, bool write_files) {
    const json& p = cfg.strategy_params;
    if (cfg.strategy == "null") {
        NullStrategy s;
        return run_with(s, cfg, data, write_files);
    }
    if (cfg.strategy == "scripted") {
        ScriptedStrategy s(p);
        return run_with(s, cfg, data, write_files);
    }
    if (cfg.strategy == "probe") {
        ProbeStrategy s(p);
        return run_with(s, cfg, data, write_files);
    }
    if (cfg.strategy == "market_maker") {
        MarketMakerStrategy s(p);
        return run_with(s, cfg, data, write_files);
    }
    if (cfg.strategy == "imbalance") {
        ImbalanceStrategy s(p);
        return run_with(s, cfg, data, write_files);
    }
    throw std::runtime_error("unknown strategy '" + cfg.strategy +
                             "' (null, scripted, probe, market_maker, imbalance)");
}

}  // namespace sim
