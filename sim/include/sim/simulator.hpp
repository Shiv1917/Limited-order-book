// Simulator<Strategy>: the event loop.
//
// The loop always processes the earliest pending event. Historical messages
// stream from a vector; only simulator-created events (order arrivals,
// deliveries to the strategy, timers) go into the priority queue. Ordering
// key is (ts, priority, seq) with market data first on ties, so the same
// input and config always give the same output.
//
// Strategy dispatch is a template parameter, not a virtual call. A strategy
// implements:
//   void on_market_data(const BookView&, const MdEvent&, OrderApi&);
//   void on_order_event(const BookView&, const OrderEvent&, OrderApi&);
//   void on_timer(const BookView&, int timer_id, OrderApi&);
// and optionally  void on_start(const BookView&, OrderApi&);
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "lobster.hpp"
#include "ob/book.hpp"
#include "sim/book_view.hpp"
#include "sim/event_queue.hpp"
#include "sim/exchange.hpp"
#include "sim/latency.hpp"
#include "sim/metrics.hpp"
#include "sim/portfolio.hpp"

namespace sim {

// One ticker-day of LOBSTER data, parsed once.
struct MarketData {
    std::vector<ob::lobster::Row> rows;
    std::vector<MdEvent> md;                     // same length as rows
    std::vector<ob::lobster::Snapshot> snaps;    // optional (validation + seeding only)
    ob::PriceBand band{};
    std::string messages_path, book_path;
};

// Loads the message file (and the orderbook file if `book_path` is non-empty).
// Returns false and sets `err` on failure.
bool load_market_data(const std::string& messages_path, const std::string& book_path, MarketData& out,
                      std::string* err);
// Converts already-parsed rows (tests build these in memory).
void build_market_data(std::vector<ob::lobster::Row> rows, std::vector<ob::lobster::Snapshot> snaps,
                       MarketData& out);

struct SimConfig {
    LatencyConfig latency{};
    std::uint64_t seed = 42;
    RiskLimits risk{};
    FillModelConfig fill{};
    std::int64_t taker_fee = 30;     // price units per share ($0.0030)
    std::int64_t maker_rebate = 20;  // price units per share ($0.0020)
    Time snapshot_ns = kSecond;      // pnl.csv grid
    bool validate_book = false;      // compare book to orderbook file (layer 3 parity)
    std::size_t validate_every = 1;
    std::size_t validate_levels = 10;
};

struct RunStats {
    std::uint64_t md_events = 0, sim_events = 0, dropped_at_end = 0, ignored_after_close = 0;
    std::uint64_t invariant_violations = 0;
    std::string first_violation;
    ob::lobster::ReplayStats replay{};
    std::uint64_t hidden_volume = 0, halts = 0, unknown_exec_refs = 0;
    Time first_ts = 0, last_ts = 0;
    double wall_seconds = 0;  // throughput only; never part of hashed output
};

class SimCore;

// What a strategy may do. Orders go through the latency stage.
class OrderApi {
public:
    explicit OrderApi(SimCore& core) : core_(core) {}
    OrderId send(Side side, Price price, Qty qty, OrderType type = OrderType::Limit);
    void cancel(OrderId id);
    void set_timer(Time at, int timer_id);
    // Position as known to the strategy: updated when fills are delivered,
    // i.e. after the return latency.
    std::int64_t position() const;
    Time now() const;

private:
    SimCore& core_;
};

// Everything except the strategy. The template below only adds dispatch.
class SimCore {
public:
    SimCore(const SimConfig& cfg, const MarketData& data);
    SimCore(const SimCore&) = delete;
    SimCore& operator=(const SimCore&) = delete;

    const SimConfig& config() const noexcept { return cfg_; }
    const MarketData& data() const noexcept { return data_; }
    const ob::Book& book() const noexcept { return *book_; }
    const Exchange& exchange() const noexcept { return *exchange_; }
    const Portfolio& portfolio() const noexcept { return portfolio_; }
    const Recorder& recorder() const noexcept { return rec_; }
    const RunStats& stats() const noexcept { return stats_; }
    Time clock() const noexcept { return clock_; }
    Price last_mid_x2() const noexcept { return last_mid_x2_; }

protected:
    friend class OrderApi;

    // Applies market-data row k to exchange + book (everything before the strategy sees it).
    void md_step(std::size_t k);
    // Applies a popped simulator event; returns true if the strategy must see it.
    bool sim_step(const SimEvent& ev);
    void begin();
    void finish(double wall_seconds);
    void advance(Time t);
    void drain();
    void violation(const std::string& what);

    // OrderApi backends.
    OrderId api_send(Side side, Price price, Qty qty, OrderType type);
    void api_cancel(OrderId id);
    void api_timer(Time at, int timer_id);

    SimConfig cfg_;
    const MarketData& data_;
    std::unique_ptr<ob::Book> book_;
    std::unique_ptr<ob::lobster::Replayer<ob::Book>> replayer_;
    std::unique_ptr<Exchange> exchange_;
    LatencyModel latency_;
    EventQueue queue_;
    Portfolio portfolio_;
    Recorder rec_;
    RunStats stats_;

    Time clock_ = 0;
    Time next_snapshot_ = kNever;
    Price last_mid_x2_ = 0;
    std::int64_t strategy_position_ = 0;
    bool md_triggered_ = false;  // the strategy is reacting to market data (adds feed latency)
    bool closed_ = false;        // session over: strategy requests are ignored
    OrderId next_id_ = kStrategyIdBase;

    struct OrderTrack {
        Qty qty = 0, filled = 0;
        Time arrived = -1;
    };
    std::map<OrderId, OrderTrack> tracks_;  // invariant bookkeeping per strategy order
};

inline OrderId OrderApi::send(Side side, Price price, Qty qty, OrderType type) {
    return core_.api_send(side, price, qty, type);
}
inline void OrderApi::cancel(OrderId id) { core_.api_cancel(id); }
inline void OrderApi::set_timer(Time at, int timer_id) { core_.api_timer(at, timer_id); }
inline std::int64_t OrderApi::position() const { return core_.strategy_position_; }
inline Time OrderApi::now() const { return core_.clock_; }

template <class Strategy>
class Simulator : public SimCore {
public:
    Simulator(const SimConfig& cfg, const MarketData& data, Strategy& strategy)
        : SimCore(cfg, data), strategy_(strategy), view_(*book_, clock_), api_(*this) {}

    void run();

private:
    Strategy& strategy_;
    BookView view_;
    OrderApi api_;
};

double wall_now_seconds();  // steady clock, used only for the throughput figure

template <class Strategy>
void Simulator<Strategy>::run() {
    const double t0 = wall_now_seconds();
    begin();
    if constexpr (requires { strategy_.on_start(view_, api_); }) {
        md_triggered_ = false;
        strategy_.on_start(view_, api_);
    }
    const auto& md = data_.md;
    const Time end_ts = md.empty() ? 0 : md.back().ts;
    std::size_t k = 0;
    for (;;) {
        const Time t_md = k < md.size() ? md[k].ts : kNever;
        const Time t_pend = queue_.top_ts();
        if (t_md == kNever && t_pend > end_ts) break;  // session over
        if (t_md <= t_pend) {  // market data first on ties
            md_step(k);
            md_triggered_ = true;
            strategy_.on_market_data(view_, md[k], api_);
            ++k;
        } else {
            const SimEvent ev = queue_.pop();
            if (!sim_step(ev)) continue;
            md_triggered_ = false;
            if (ev.prio == Prio::Deliver) strategy_.on_order_event(view_, ev.oev, api_);
            else strategy_.on_timer(view_, ev.timer_id, api_);
        }
    }
    // After the last market event the session is closed: acks/fills already
    // decided are still delivered so the strategy's view ends consistent, but
    // orders still in flight, timers, and anything the strategy sends now are
    // dropped (counted in dropped_at_end).
    closed_ = true;
    while (!queue_.empty()) {
        const SimEvent ev = queue_.pop();
        if (ev.prio != Prio::Deliver) {
            ++stats_.dropped_at_end;
            continue;
        }
        sim_step(ev);
        strategy_.on_order_event(view_, ev.oev, api_);
    }
    finish(wall_now_seconds() - t0);
}

}  // namespace sim
