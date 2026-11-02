// Run recorder: everything that ends up in the output files, collected in
// memory during the run and written once at the end (output.cpp).
#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "sim/events.hpp"

namespace sim {

struct OrderLogRow {
    Time ts = 0;
    OrderId id = 0;
    const char* event = "";  // sent, arrived, acked, rejected, cancel_sent, cancelled, partial, filled
    Side side = Side::Buy;
    Price price = 0;
    Qty qty = 0;
    const char* detail = "";  // order type, reject/cancel reason, maker/taker
};

struct FillRow {
    Time ts = 0;
    OrderId id = 0;
    Side side = Side::Buy;
    Price price = 0;
    Qty qty = 0;
    bool maker = false;
    std::int64_t fee = 0;  // price units, negative = rebate
    std::uint64_t queue_ahead = 0;
    Price mid_x2 = 0;  // mid at the fill, before the triggering event moved the book
    Time arrived = 0;
};

struct PnlRow {
    Time ts = 0;
    std::int64_t cash = 0, position = 0, realized = 0, fees = 0;
    Price mid_x2 = 0;
    std::int64_t unrealized_x2 = 0, equity_x2 = 0;
};

struct Recorder {
    std::vector<OrderLogRow> orders;
    std::vector<FillRow> fills;
    std::vector<PnlRow> pnl;
    std::vector<std::pair<Time, Price>> mids;  // (ts, mid_x2) whenever the mid changes

    // Counters for summary.json.
    std::uint64_t orders_sent = 0, cancels_sent = 0, rejects = 0, cancel_rejects = 0;
    std::uint64_t passive_orders = 0, passive_shares = 0;  // orders/shares that rested
    std::uint64_t passive_filled_shares = 0, taker_filled_shares = 0;
    std::uint64_t queue_waits = 0;
    long double queue_wait_sum_ns = 0;
    std::map<OrderId, Time> waiting;  // rested, not yet filled: arrival time

    void on_rested(OrderId id, Qty leaves, Time arrived) {
        ++passive_orders;
        passive_shares += leaves;
        waiting.emplace(id, arrived);
    }
    void on_maker_fill(OrderId id, Time ts) {
        auto it = waiting.find(id);
        if (it == waiting.end()) return;
        ++queue_waits;
        queue_wait_sum_ns += static_cast<long double>(ts - it->second);
        waiting.erase(it);
    }
    void on_order_done(OrderId id) { waiting.erase(id); }

    // Mid (x2) in force at time t: the last change at or before t (0 if none).
    Price mid_at(Time t) const {
        auto it = std::upper_bound(mids.begin(), mids.end(), t,
                                   [](Time v, const std::pair<Time, Price>& m) { return v < m.first; });
        return it == mids.begin() ? 0 : std::prev(it)->second;
    }
};

}  // namespace sim
